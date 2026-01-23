// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/editor/EditorPipeline.cpp

    \author Andrew Reidmeyer

    \brief  Implementation of pipeline system for NanoVDB Editor.
*/

#include "EditorPipeline.h"
#include "EditorSceneManager.h"
#include "EditorScene.h"
#include "EditorToken.h"
#include "Editor.h"
#include "ShaderParams.h"
#include "Console.h"
#include "Profiler.h"
#include <cstdio>
#include <filesystem>

namespace pnanovdb_editor
{

// ============================================================================
// PipelineExecutionContext Helper Methods
// ============================================================================

pnanovdb_compute_array_t* PipelineExecutionContext::get_named_array(pnanovdb_editor_token_t* name_token) const
{
    if (!named_arrays || !name_token)
    {
        return nullptr;
    }

    auto it = named_arrays->find(name_token->id);
    if (it != named_arrays->end())
    {
        return it->second.array;
    }
    return nullptr;
}

pnanovdb_compute_array_t* PipelineExecutionContext::get_named_array_by_name(const char* name) const
{
    if (!named_arrays || !name)
    {
        return nullptr;
    }

    // Look up token by name
    pnanovdb_editor_token_t* token = EditorToken::getInstance().getToken(name);
    if (!token)
    {
        return nullptr;
    }

    return get_named_array(token);
}

// ============================================================================
// RenderPipelineExecutor
// ============================================================================

PipelineStatus RenderPipelineExecutor::execute(PipelineConfig* config, PipelineExecutionContext* context)
{
    if (!config || !context)
    {
        return PipelineStatus::Failed;
    }

    // For now, mark as completed - actual rendering happens in existing render loop
    // This will be expanded to handle multi-pass rendering for Gaussians
    config->status = PipelineStatus::Completed;

    if (config->on_complete)
    {
        config->on_complete(config, PipelineStatus::Completed);
    }

    return PipelineStatus::Completed;
}

bool RenderPipelineExecutor::can_execute(const PipelineConfig* config, const PipelineExecutionContext* context) const
{
    if (!config || !context)
    {
        return false;
    }

    // Need at least one input (renderable data)
    return !config->input_components.empty();
}

// ============================================================================
// Raster3DPipelineExecutor
// ============================================================================

PipelineStatus Raster3DPipelineExecutor::execute(PipelineConfig* config, PipelineExecutionContext* context)
{
    if (!config || !context || !context->raster || !context->compute || !context->queue)
    {
        Console::getInstance().addLog(
            Console::LogLevel::Error,
            "[Raster3D] Execute failed: missing config, context, compute, queue, or raster interface");
        return PipelineStatus::Failed;
    }

    config->status = PipelineStatus::Running;

    // ========================================================================
    // Get gaussian arrays from named arrays
    // These are added automatically by add_gaussian_data_2()
    // ========================================================================

    auto get_token = [](const char* name) { return EditorToken::getInstance().getToken(name); };

    pnanovdb_compute_array_t* means = context->get_named_array(get_token("means"));
    pnanovdb_compute_array_t* opacities = context->get_named_array(get_token("opacities"));
    pnanovdb_compute_array_t* quaternions = context->get_named_array(get_token("quaternions"));
    pnanovdb_compute_array_t* scales = context->get_named_array(get_token("scales"));
    pnanovdb_compute_array_t* sh_0 = context->get_named_array(get_token("sh_0"));
    pnanovdb_compute_array_t* sh_n = context->get_named_array(get_token("sh_n"));

    // Validate required arrays
    if (!means)
    {
        Console::getInstance().addLog(
            Console::LogLevel::Error, "[Raster3D] Execute failed: 'means' named array not found");
        config->status = PipelineStatus::Failed;
        return PipelineStatus::Failed;
    }

    Console::getInstance().addLog("[Raster3D] Executing Gaussian->NanoVDB rasterization");
    Console::getInstance().addLog("[Raster3D]   means: %s (%llu elements)", means ? "found" : "missing",
                                  means ? (unsigned long long)means->element_count : 0);
    Console::getInstance().addLog("[Raster3D]   opacities: %s", opacities ? "found" : "missing");
    Console::getInstance().addLog("[Raster3D]   quaternions: %s", quaternions ? "found" : "missing");
    Console::getInstance().addLog("[Raster3D]   scales: %s", scales ? "found" : "missing");
    Console::getInstance().addLog("[Raster3D]   sh_0: %s", sh_0 ? "found" : "missing");
    Console::getInstance().addLog("[Raster3D]   sh_n: %s", sh_n ? "found" : "missing");

    // ========================================================================
    // Get voxel size from pipeline params (default if not set)
    // ========================================================================

    float voxel_size = 1.f / 128.f; // Default voxel size

    // Try to read voxel_size from config->params if it's a Raster3DParams struct
    if (config->params)
    {
        // Check if params is a Raster3DParams struct
        Raster3DParams* raster_params = static_cast<Raster3DParams*>(config->params);
        if (raster_params->voxel_size > 0.f)
        {
            voxel_size = raster_params->voxel_size;
            Console::getInstance().addLog("[Raster3D]   voxel_size from params: %f", voxel_size);
        }
        else
        {
            Console::getInstance().addLog("[Raster3D]   voxel_size (default): %f", voxel_size);
        }
    }
    else
    {
        Console::getInstance().addLog("[Raster3D]   voxel_size (default, no params): %f", voxel_size);
    }

    // ========================================================================
    // Execute rasterization
    // ========================================================================

    // colors can be nullptr - the rasterizer handles this
    pnanovdb_compute_array_t* colors = nullptr;

    // shader_params_arrays can be nullptr for now
    pnanovdb_compute_array_t** shader_params_arrays = nullptr;

    pnanovdb_compute_array_t* output =
        context->raster->raster_to_nanovdb(context->compute, context->queue, voxel_size, means, quaternions, scales,
                                           colors, sh_0, sh_n, opacities, shader_params_arrays,
                                           nullptr, // profiler_report
                                           nullptr // userdata
        );

    if (!output)
    {
        Console::getInstance().addLog(Console::LogLevel::Error, "[Raster3D] raster_to_nanovdb returned null");
        config->status = PipelineStatus::Failed;
        return PipelineStatus::Failed;
    }

    Console::getInstance().addLog("[Raster3D] Rasterization complete, output NanoVDB: %llu bytes",
                                  (unsigned long long)(output->element_count * output->element_size));

    // Store output in context
    if (context->output_nanovdb)
    {
        *context->output_nanovdb = output;
    }

    config->status = PipelineStatus::Completed;

    if (config->on_complete)
    {
        config->on_complete(config, PipelineStatus::Completed);
    }

    return PipelineStatus::Completed;
}

bool Raster3DPipelineExecutor::can_execute(const PipelineConfig* config, const PipelineExecutionContext* context) const
{
    if (!config || !context || !context->raster || !context->compute || !context->queue)
    {
        return false;
    }

    // Need at least the 'means' named array (added by add_gaussian_data_2)
    pnanovdb_editor_token_t* means_token = EditorToken::getInstance().getToken("means");
    pnanovdb_compute_array_t* means = context->get_named_array(means_token);

    return means != nullptr;
}

// ============================================================================
// PipelineManager
// ============================================================================

uint64_t PipelineManager::make_pipeline_key(PipelineType type, pnanovdb_editor_token_t* name)
{
    uint64_t type_val = static_cast<uint64_t>(type);
    uint64_t name_val = name ? name->id : 0;
    return (type_val << 32) | name_val;
}

uint64_t PipelineManager::make_object_pipeline_key(pnanovdb_editor_token_t* scene,
                                                   pnanovdb_editor_token_t* object,
                                                   PipelineType type,
                                                   pnanovdb_editor_token_t* name)
{
    // Combine scene_id, object_id, type, and name into a single key
    // We use a hash-like combination to fit into 64 bits
    uint64_t scene_val = scene ? scene->id : 0;
    uint64_t object_val = object ? object->id : 0;
    uint64_t type_val = static_cast<uint64_t>(type);
    uint64_t name_val = name ? name->id : 0;

    // Simple hash combination: rotate and XOR
    uint64_t key = scene_val;
    key = ((key << 13) | (key >> 51)) ^ object_val;
    key = ((key << 17) | (key >> 47)) ^ (type_val << 32);
    key = ((key << 7) | (key >> 57)) ^ name_val;
    return key;
}

IPipelineExecutor* PipelineManager::get_executor(PipelineType type)
{
    auto it = m_executors.find(type);
    if (it != m_executors.end())
    {
        return it->second.get();
    }

    // Create executor on demand
    std::shared_ptr<IPipelineExecutor> executor;

    switch (type)
    {
    case PipelineType::Null:
        executor = std::make_shared<NullPipelineExecutor>();
        break;
    case PipelineType::Render:
        executor = std::make_shared<RenderPipelineExecutor>();
        break;
    case PipelineType::Raster3D:
        executor = std::make_shared<Raster3DPipelineExecutor>();
        break;
    default:
        return nullptr;
    }

    m_executors[type] = executor;
    return executor.get();
}

void PipelineManager::set_conversion_pipeline(PipelineType type, const PipelineConfig& config, pnanovdb_editor_token_t* name)
{
    uint64_t key = make_pipeline_key(type, name);
    m_conversion_pipelines[key] = config;
    m_conversion_pipelines[key].type = type;
    m_conversion_pipelines[key].name_token = name;
    m_conversion_pipelines[key].status = PipelineStatus::NotRun;
    m_conversion_pipelines[key].needs_run = true;
}

void PipelineManager::set_render_pipeline(const PipelineConfig& config, pnanovdb_editor_token_t* name)
{
    uint64_t key = make_pipeline_key(PipelineType::Render, name);
    m_render_pipelines[key] = config;
    m_render_pipelines[key].type = PipelineType::Render;
    m_render_pipelines[key].name_token = name;
}

PipelineConfig* PipelineManager::get_conversion_pipeline(PipelineType type, pnanovdb_editor_token_t* name)
{
    uint64_t key = make_pipeline_key(type, name);
    auto it = m_conversion_pipelines.find(key);
    return (it != m_conversion_pipelines.end()) ? &it->second : nullptr;
}

PipelineConfig* PipelineManager::get_render_pipeline(pnanovdb_editor_token_t* name)
{
    uint64_t key = make_pipeline_key(PipelineType::Render, name);
    auto it = m_render_pipelines.find(key);
    return (it != m_render_pipelines.end()) ? &it->second : nullptr;
}

void PipelineManager::mark_dirty(PipelineType type, pnanovdb_editor_token_t* name)
{
    uint64_t key = make_pipeline_key(type, name);
    auto it = m_conversion_pipelines.find(key);
    if (it != m_conversion_pipelines.end())
    {
        it->second.status = PipelineStatus::Dirty;
        it->second.needs_run = it->second.auto_execute;
    }
}

void PipelineManager::mark_all_dirty()
{
    for (auto& pair : m_conversion_pipelines)
    {
        pair.second.status = PipelineStatus::Dirty;
        pair.second.needs_run = pair.second.auto_execute;
    }
}

PipelineStatus PipelineManager::execute_pipeline(PipelineType type,
                                                 PipelineExecutionContext* context,
                                                 pnanovdb_editor_token_t* name)
{
    uint64_t key = make_pipeline_key(type, name);

    // Check conversion pipelines first
    auto it = m_conversion_pipelines.find(key);
    if (it != m_conversion_pipelines.end())
    {
        IPipelineExecutor* executor = get_executor(type);
        if (!executor)
        {
            printf("[Pipeline] No executor found for pipeline type %d\n", (int)type);
            return PipelineStatus::Failed;
        }

        if (!executor->can_execute(&it->second, context))
        {
            printf("[Pipeline] Pipeline cannot execute (missing inputs or resources)\n");
            return PipelineStatus::Failed;
        }

        PipelineStatus status = executor->execute(&it->second, context);
        it->second.needs_run = false;
        return status;
    }

    // Check render pipelines
    auto rit = m_render_pipelines.find(key);
    if (rit != m_render_pipelines.end())
    {
        IPipelineExecutor* executor = get_executor(PipelineType::Render);
        if (!executor)
        {
            return PipelineStatus::Failed;
        }

        if (!executor->can_execute(&rit->second, context))
        {
            return PipelineStatus::Failed;
        }

        return executor->execute(&rit->second, context);
    }

    printf("[Pipeline] Pipeline not found for type %d\n", (int)type);
    return PipelineStatus::Failed;
}

void PipelineManager::execute_dirty_pipelines(PipelineExecutionContext* context)
{
    if (!context)
    {
        return;
    }

    // Execute all conversion pipelines that need running
    for (auto& pair : m_conversion_pipelines)
    {
        PipelineConfig& config = pair.second;

        if (config.needs_run || config.status == PipelineStatus::Dirty)
        {
            if (config.auto_execute)
            {
                IPipelineExecutor* executor = get_executor(config.type);
                if (executor && executor->can_execute(&config, context))
                {
                    executor->execute(&config, context);
                    config.needs_run = false;
                }
            }
        }
    }
}

bool PipelineManager::has_pending_work() const
{
    for (const auto& pair : m_conversion_pipelines)
    {
        const PipelineConfig& config = pair.second;
        if (config.needs_run || config.status == PipelineStatus::Dirty)
        {
            return true;
        }
    }
    return false;
}

PipelineStatus PipelineManager::run_pipeline_manual(PipelineType type,
                                                    PipelineExecutionContext* context,
                                                    pnanovdb_editor_token_t* name)
{
    uint64_t key = make_pipeline_key(type, name);

    // Find the pipeline
    auto it = m_conversion_pipelines.find(key);
    if (it == m_conversion_pipelines.end())
    {
        printf("[Pipeline] Manual run: pipeline type %d not found\n", (int)type);
        return PipelineStatus::Failed;
    }

    IPipelineExecutor* executor = get_executor(type);
    if (!executor)
    {
        printf("[Pipeline] Manual run: no executor for pipeline type %d\n", (int)type);
        return PipelineStatus::Failed;
    }

    if (!executor->can_execute(&it->second, context))
    {
        printf("[Pipeline] Manual run: pipeline cannot execute (missing inputs or resources)\n");
        return PipelineStatus::Failed;
    }

    // Force execution regardless of dirty state
    printf("[Pipeline] Manual run: executing pipeline type %d\n", (int)type);
    PipelineStatus status = executor->execute(&it->second, context);
    it->second.needs_run = false;
    it->second.status = status;
    return status;
}

void PipelineManager::create_default_pipelines(SceneObjectType data_type, const pnanovdb_compute_t* compute)
{
    switch (data_type)
    {
    case SceneObjectType::NanoVDB:
    {
        // For NanoVDB, default is Null conversion pipeline (pass-through)
        // and a Render pipeline
        PipelineConfig null_config;
        null_config.type = PipelineType::Null;
        null_config.auto_execute = true;
        set_conversion_pipeline(PipelineType::Null, null_config);

        PipelineConfig render_config;
        render_config.type = PipelineType::Render;
        render_config.auto_execute = true;
        // NanoVDB render params loaded dynamically from shader-specific JSON
        // (set params_json_name when shader is selected)
        set_render_pipeline(render_config);
        break;
    }

    case SceneObjectType::GaussianData:
    {
        // For Gaussian data:
        // 1. Raster3D conversion pipeline (Gaussian -> NanoVDB)
        // 2. Render pipeline for 2D visualization
        //
        // Raster3D params are stored directly in config->params
        PipelineConfig raster3d_config;
        raster3d_config.type = PipelineType::Raster3D;
        raster3d_config.auto_execute = true;

        // Add default shaders for Raster3D pipeline
        raster3d_config.add_shader(PipelineShader::create(s_raster3d_gaussian_shader));

        // Allocate and initialize Raster3DParams
        if (compute)
        {
            raster3d_config.params_array = compute->create_array(sizeof(Raster3DParams), 1, nullptr);
            if (raster3d_config.params_array && raster3d_config.params_array->data)
            {
                raster3d_config.params = raster3d_config.params_array->data;
                // Initialize with defaults
                Raster3DParams* params = static_cast<Raster3DParams*>(raster3d_config.params);
                params->voxel_size = 1.f / 128.f;
            }
        }
        set_conversion_pipeline(PipelineType::Raster3D, raster3d_config);

        // Render pipeline for 2D gaussian splatting
        PipelineConfig render_config;
        render_config.type = PipelineType::Render;
        render_config.auto_execute = true;
        // Use the gaussian shader name for JSON params (loads raster/gaussian_rasterize_2d.slang.json)
        render_config.params_json_name = s_raster2d_gaussian_shader;

        // Add default shaders for Gaussian 2D render pipeline
        // These execute in order: preprocessing, rasterization, compositing
        render_config.add_shader(PipelineShader::create(s_raster2d_gaussian_shader));

        set_render_pipeline(render_config);
        break;
    }

    default:
        break;
    }
}

// ============================================================================
// Dynamic Parameter Initialization
// ============================================================================

void PipelineManager::initialize_params_from_json(PipelineConfig& config,
                                                  ShaderParams& shader_params,
                                                  const pnanovdb_compute_t* compute)
{
    if (!compute)
    {
        return;
    }

    // Only initialize if using dynamic JSON-based params
    if (!config.uses_dynamic_params())
    {
        return;
    }

    const std::string& shader_name = config.params_json_name;

    // Note: config.parameters reflection info is populated separately if needed
    // The ShaderParams class handles the actual JSON loading and array creation

    // Load shader group first (for fallback/group defaults), then shader JSON
    // This matches the logic in EditorSceneManager::create_initialized_shader_params
    shader_params.loadGroup(s_raster2d_shader_group, false);
    shader_params.load(shader_name.c_str(), false);

    // Create compute array with defaults from JSON
    pnanovdb_compute_array_t* params_array = shader_params.get_compute_array_for_shader(shader_name, compute);

    if (params_array)
    {
        // Store the backing array and set up ownership
        config.params_array = params_array;
        config.params = params_array->data;
        config.params_array_owner = std::shared_ptr<pnanovdb_compute_array_t>(
            params_array,
            [destroy_fn = compute->destroy_array](pnanovdb_compute_array_t* ptr)
            {
                if (ptr)
                {
                    destroy_fn(ptr);
                }
            });

        // Track reflect type for backward-compatible copy across struct versions
        config.params_reflect_type = PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_raster_shader_params_t);
    }
}

// ============================================================================
// Object-scoped Pipeline Methods
// ============================================================================

void PipelineManager::set_object_conversion_pipeline(pnanovdb_editor_token_t* scene,
                                                     pnanovdb_editor_token_t* object,
                                                     PipelineType type,
                                                     const PipelineConfig& config,
                                                     pnanovdb_editor_token_t* name)
{
    uint64_t key = make_object_pipeline_key(scene, object, type, name);
    m_conversion_pipelines[key] = config;
    m_conversion_pipelines[key].type = type;
    m_conversion_pipelines[key].name_token = name;
    m_conversion_pipelines[key].scene_token = scene;
    m_conversion_pipelines[key].object_token = object;
    m_conversion_pipelines[key].status = PipelineStatus::NotRun;
    m_conversion_pipelines[key].needs_run = true;
}

void PipelineManager::set_object_render_pipeline(pnanovdb_editor_token_t* scene,
                                                 pnanovdb_editor_token_t* object,
                                                 const PipelineConfig& config,
                                                 pnanovdb_editor_token_t* name)
{
    uint64_t key = make_object_pipeline_key(scene, object, PipelineType::Render, name);
    m_render_pipelines[key] = config;
    m_render_pipelines[key].type = PipelineType::Render;
    m_render_pipelines[key].name_token = name;
    m_render_pipelines[key].scene_token = scene;
    m_render_pipelines[key].object_token = object;
}

PipelineConfig* PipelineManager::get_object_pipeline(pnanovdb_editor_token_t* scene,
                                                     pnanovdb_editor_token_t* object,
                                                     PipelineType type,
                                                     pnanovdb_editor_token_t* name)
{
    uint64_t key = make_object_pipeline_key(scene, object, type, name);

    if (type == PipelineType::Render)
    {
        auto it = m_render_pipelines.find(key);
        return (it != m_render_pipelines.end()) ? &it->second : nullptr;
    }
    else
    {
        auto it = m_conversion_pipelines.find(key);
        return (it != m_conversion_pipelines.end()) ? &it->second : nullptr;
    }
}

void PipelineManager::mark_object_dirty(pnanovdb_editor_token_t* scene,
                                        pnanovdb_editor_token_t* object,
                                        PipelineType type,
                                        pnanovdb_editor_token_t* name)
{
    uint64_t key = make_object_pipeline_key(scene, object, type, name);
    auto it = m_conversion_pipelines.find(key);
    if (it != m_conversion_pipelines.end())
    {
        it->second.status = PipelineStatus::Dirty;
        it->second.needs_run = it->second.auto_execute;
    }
}

void PipelineManager::mark_object_all_dirty(pnanovdb_editor_token_t* scene, pnanovdb_editor_token_t* object)
{
    // Mark all pipelines for this object as dirty
    // Since we use a hash key, we need to iterate and check
    // This is a simplified approach - in production you might want to track object->keys mapping
    for (auto& pair : m_conversion_pipelines)
    {
        // We can't easily reverse the hash, so mark all dirty
        // A more sophisticated approach would store the scene/object in the config
        pair.second.status = PipelineStatus::Dirty;
        pair.second.needs_run = pair.second.auto_execute;
    }
}

PipelineStatus PipelineManager::execute_object_pipeline(pnanovdb_editor_token_t* scene,
                                                        pnanovdb_editor_token_t* object,
                                                        PipelineType type,
                                                        PipelineExecutionContext* context,
                                                        pnanovdb_editor_token_t* name)
{
    uint64_t key = make_object_pipeline_key(scene, object, type, name);

    // Check conversion pipelines
    auto it = m_conversion_pipelines.find(key);
    if (it != m_conversion_pipelines.end())
    {
        IPipelineExecutor* executor = get_executor(type);
        if (!executor)
        {
            printf("[Pipeline] No executor found for pipeline type %d\n", (int)type);
            return PipelineStatus::Failed;
        }

        if (!executor->can_execute(&it->second, context))
        {
            printf("[Pipeline] Pipeline cannot execute (missing inputs or resources)\n");
            return PipelineStatus::Failed;
        }

        PipelineStatus status = executor->execute(&it->second, context);
        it->second.needs_run = false;
        return status;
    }

    // Check render pipelines
    auto rit = m_render_pipelines.find(key);
    if (rit != m_render_pipelines.end())
    {
        IPipelineExecutor* executor = get_executor(PipelineType::Render);
        if (!executor)
        {
            return PipelineStatus::Failed;
        }

        if (!executor->can_execute(&rit->second, context))
        {
            return PipelineStatus::Failed;
        }

        return executor->execute(&rit->second, context);
    }

    printf("[Pipeline] Object pipeline not found\n");
    return PipelineStatus::Failed;
}

void PipelineManager::remove_object_pipelines(pnanovdb_editor_token_t* scene, pnanovdb_editor_token_t* object)
{
    if (!scene || !object)
    {
        return;
    }

    uint64_t scene_id = scene->id;
    uint64_t object_id = object->id;

    // Remove matching conversion pipelines
    for (auto it = m_conversion_pipelines.begin(); it != m_conversion_pipelines.end();)
    {
        const PipelineConfig& config = it->second;
        bool matches = (config.scene_token && config.scene_token->id == scene_id) &&
                       (config.object_token && config.object_token->id == object_id);
        if (matches)
        {
            Console::getInstance().addLog(
                Console::LogLevel::Debug,
                "[PipelineManager] Removing conversion pipeline for scene=%llu, object=%llu, type=%d",
                (unsigned long long)scene_id, (unsigned long long)object_id, (int)config.type);
            it = m_conversion_pipelines.erase(it);
        }
        else
        {
            ++it;
        }
    }

    // Remove matching render pipelines
    for (auto it = m_render_pipelines.begin(); it != m_render_pipelines.end();)
    {
        const PipelineConfig& config = it->second;
        bool matches = (config.scene_token && config.scene_token->id == scene_id) &&
                       (config.object_token && config.object_token->id == object_id);
        if (matches)
        {
            Console::getInstance().addLog(Console::LogLevel::Debug,
                                          "[PipelineManager] Removing render pipeline for scene=%llu, object=%llu",
                                          (unsigned long long)scene_id, (unsigned long long)object_id);
            it = m_render_pipelines.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

// ============================================================================
// FileImportConfig
// ============================================================================

bool FileImportConfig::is_nanovdb_file() const
{
    if (filepath.size() < 5)
    {
        return false;
    }
    std::string ext = filepath.substr(filepath.size() - 5);
    // Case-insensitive comparison
    for (auto& c : ext)
    {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return ext == ".nvdb";
}

// ============================================================================
// FileImportPipelineExecutor
// ============================================================================

FileImportPipelineExecutor::FileImportPipelineExecutor()
{
    // Initialize shader params arrays
    for (pnanovdb_uint32_t idx = 0u; idx < pnanovdb_raster::shader_param_count; idx++)
    {
        m_pending_shader_params_arrays[idx] = nullptr;
    }
}

void FileImportPipelineExecutor::init(const pnanovdb_compute_t* compute,
                                      pnanovdb_raster_t* raster,
                                      pnanovdb_compute_queue_t* device_queue,
                                      pnanovdb_compute_queue_t* compute_queue)
{
    m_compute = compute;
    m_raster = raster;
    m_device_queue = device_queue;
    m_compute_queue = compute_queue;
    m_initialized = (compute != nullptr && raster != nullptr && device_queue != nullptr);

    // Initialize raster shader params data type
    m_raster_shader_params_data_type = PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_raster_shader_params_t);
    const pnanovdb_raster_shader_params_t* default_raster_shader_params =
        (const pnanovdb_raster_shader_params_t*)m_raster_shader_params_data_type->default_value;
    m_init_raster_shader_params = *default_raster_shader_params;
}

PipelineStatus FileImportPipelineExecutor::execute(PipelineConfig* config, PipelineExecutionContext* context)
{
    if (!config || !m_initialized || !m_editor_scene || !m_scene_manager)
    {
        Console::getInstance().addLog(Console::LogLevel::Error, "[Pipeline] FileImport: missing dependencies");
        return PipelineStatus::Failed;
    }

    // Get the import config from the pipeline config's params
    auto* import_config = static_cast<FileImportConfig*>(config->params);
    if (!import_config || import_config->filepath.empty())
    {
        Console::getInstance().addLog(Console::LogLevel::Error, "[Pipeline] FileImport: no file path specified");
        return PipelineStatus::Failed;
    }

    if (m_worker.hasRunningTask())
    {
        Console::getInstance().addLog(Console::LogLevel::Error, "[Pipeline] FileImport: import already in progress");
        return PipelineStatus::Failed;
    }

    config->status = PipelineStatus::Running;
    m_pending_filepath = import_config->filepath;
    m_is_nanovdb_import = import_config->is_nanovdb_file();

    // Handle NanoVDB file loading
    if (m_is_nanovdb_import)
    {
        Console::getInstance().addLog("[Pipeline] Starting NanoVDB import: '%s'", import_config->filepath.c_str());

        m_task_id = m_worker.enqueue(
            [](const pnanovdb_compute_t* compute, const char* filepath, pnanovdb_compute_array_t** nanovdb_array) -> bool
            {
                *nanovdb_array = compute->load_nanovdb(filepath);
                return *nanovdb_array != nullptr;
            },
            m_compute, m_pending_filepath.c_str(), &m_pending_nanovdb_array);

        return PipelineStatus::Running;
    }

    // Handle Gaussian file import
    m_pending_voxel_size = 1.f / import_config->voxels_per_unit;

    // Get user params for the raster shader
    m_compute->destroy_array(m_pending_shader_params_arrays[pnanovdb_raster::gaussian_frag_color_slang]);
    m_pending_shader_params_arrays[pnanovdb_raster::gaussian_frag_color_slang] =
        m_scene_manager->shader_params.get_compute_array_for_shader("raster/gaussian_frag_color.slang", m_compute);

    // Create new default params
    m_pending_raster_params = &m_init_raster_shader_params;
    m_pending_raster_params->name = nullptr;
    m_pending_raster_params->data_type = m_raster_shader_params_data_type;

    pnanovdb_compute_queue_t* worker_queue = import_config->rasterize_to_nanovdb ? m_compute_queue : m_device_queue;

    Console::getInstance().addLog("[Pipeline] Starting Gaussian import: '%s' (mode: %s)", import_config->filepath.c_str(),
                                  import_config->rasterize_to_nanovdb ? "Raster3D->NanoVDB" : "Raster2D->Gaussian");

    m_task_id = m_worker.enqueue(
        [&](pnanovdb_raster_t* raster, const pnanovdb_compute_t* compute, pnanovdb_compute_queue_t* queue,
            const char* filepath, float voxel_size, pnanovdb_compute_array_t** nanovdb_array,
            pnanovdb_raster_gaussian_data_t** gaussian_data, pnanovdb_raster_context_t** raster_context,
            pnanovdb_compute_array_t** shader_params_arrays, pnanovdb_raster_shader_params_t* raster_params,
            pnanovdb_profiler_report_t profiler) -> bool
        {
            return raster->raster_file(raster, compute, queue, filepath, voxel_size, nanovdb_array, gaussian_data,
                                       raster_context, shader_params_arrays, raster_params, profiler, (void*)(&m_worker));
        },
        m_raster, m_compute, worker_queue, m_pending_filepath.c_str(), m_pending_voxel_size,
        import_config->rasterize_to_nanovdb ? &m_pending_nanovdb_array : nullptr,
        import_config->rasterize_to_nanovdb ? nullptr : &m_pending_gaussian_data,
        import_config->rasterize_to_nanovdb ? nullptr : &m_pending_raster_ctx, m_pending_shader_params_arrays,
        m_pending_raster_params, pnanovdb_editor::Profiler::report_callback);

    return PipelineStatus::Running;
}

bool FileImportPipelineExecutor::can_execute(const PipelineConfig* config, const PipelineExecutionContext* context) const
{
    if (!m_initialized || !m_editor_scene || !m_scene_manager)
    {
        return false;
    }

    // Check if another import is already in progress
    if (m_worker.hasRunningTask())
    {
        return false;
    }

    // Validate import config
    auto* import_config = static_cast<FileImportConfig*>(config->params);
    return import_config && !import_config->filepath.empty();
}

bool FileImportPipelineExecutor::is_importing() const
{
    return m_worker.isTaskRunning(m_task_id);
}

bool FileImportPipelineExecutor::get_progress(std::string& progress_text, float& progress_value)
{
    if (m_worker.isTaskRunning(m_task_id))
    {
        progress_text = m_worker.getTaskProgressText(m_task_id);
        progress_value = m_worker.getTaskProgress(m_task_id);
        return true;
    }
    return false;
}

bool FileImportPipelineExecutor::handle_completion(std::shared_ptr<pnanovdb_raster_gaussian_data_t>& old_gaussian_data_ptr)
{
    if (!m_worker.isTaskCompleted(m_task_id))
    {
        return false;
    }

    if (m_worker.isTaskSuccessful(m_task_id))
    {
        // Get the object name from filepath for shader override application
        std::filesystem::path fsPath(m_pending_filepath);
        std::string view_name = fsPath.stem().string();
        pnanovdb_editor_token_t* scene_token = m_editor_scene->get_current_scene_token();
        pnanovdb_editor_token_t* name_token = EditorToken::getInstance().getToken(view_name.c_str());

        if (m_pending_nanovdb_array)
        {
            m_editor_scene->handle_nanovdb_data_load(m_pending_nanovdb_array, m_pending_filepath.c_str());
        }
        else if (m_pending_gaussian_data)
        {
            m_editor_scene->handle_gaussian_data_load(
                m_pending_gaussian_data, m_pending_raster_params, m_pending_filepath.c_str(), old_gaussian_data_ptr);
        }

        Console::getInstance().addLog("[Pipeline] Import of '%s' was successful", m_pending_filepath.c_str());

        // Re-sync to pick up the newly selected view for immediate rendering
        m_editor_scene->sync_selected_view_with_current();
    }
    else
    {
        Console::getInstance().addLog(
            Console::LogLevel::Error, "[Pipeline] Import of '%s' failed", m_pending_filepath.c_str());
    }

    // Clean up temporary worker thread raster context (if created during rasterization)
    if (m_pending_raster_ctx)
    {
        m_raster->destroy_context(m_compute, m_device_queue, m_pending_raster_ctx);
        m_pending_raster_ctx = nullptr;
    }

    m_pending_filepath = "";
    m_worker.removeCompletedTask(m_task_id);

    return true;
}

// ============================================================================
// PipelineManager - File Import Methods
// ============================================================================

void PipelineManager::configure_file_import(const pnanovdb_compute_t* compute,
                                            pnanovdb_raster_t* raster,
                                            pnanovdb_compute_queue_t* device_queue,
                                            pnanovdb_compute_queue_t* compute_queue,
                                            EditorScene* editor_scene,
                                            EditorSceneManager* scene_manager)
{
    // Create or reuse the file import executor
    if (!m_file_import_executor)
    {
        m_file_import_executor = std::make_shared<FileImportPipelineExecutor>();
    }

    m_file_import_executor->init(compute, raster, device_queue, compute_queue);
    m_file_import_executor->set_editor_scene(editor_scene);
    m_file_import_executor->set_scene_manager(scene_manager);

    // Register in executors map
    m_executors[PipelineType::FileImport] = m_file_import_executor;
}

bool PipelineManager::import_file(const std::string& filepath,
                                  bool rasterize_to_nanovdb,
                                  const pnanovdb_pipeline_settings_t* settings)
{
    if (!m_file_import_executor)
    {
        Console::getInstance().addLog(
            Console::LogLevel::Error, "[Pipeline] File import not configured - call configure_file_import() first");
        return false;
    }

    // Create import configuration
    m_import_config = std::make_unique<FileImportConfig>();
    m_import_config->filepath = filepath;
    m_import_config->rasterize_to_nanovdb = rasterize_to_nanovdb;

    // Default voxels_per_unit
    float voxels_per_unit = 128.f;

    // Extract parameters from settings if provided
    if (settings != nullptr)
    {
        // Look for voxel_size parameter
        if (settings->params != nullptr && settings->param_count > 0)
        {
            for (pnanovdb_uint32_t i = 0; i < settings->param_count; ++i)
            {
                const pnanovdb_pipeline_param_t& param = settings->params[i];
                if (param.name != nullptr && strcmp(param.name, "voxel_size") == 0)
                {
                    voxels_per_unit = param.value;
                }
            }
        }
    }
    m_import_config->voxels_per_unit = voxels_per_unit;

    // Create a pipeline config for this import
    PipelineConfig pipeline_config;
    pipeline_config.type = PipelineType::FileImport;
    pipeline_config.params = m_import_config.get();
    pipeline_config.status = PipelineStatus::NotRun;
    pipeline_config.auto_execute = false; // Manual execution only

    // Store the pipeline config
    uint64_t key = make_pipeline_key(PipelineType::FileImport, nullptr);
    m_conversion_pipelines[key] = pipeline_config;

    // Execute immediately
    PipelineExecutionContext context;
    PipelineStatus status = execute_pipeline(PipelineType::FileImport, &context, nullptr);

    return status == PipelineStatus::Running || status == PipelineStatus::Completed;
}

bool PipelineManager::is_importing() const
{
    if (m_file_import_executor)
    {
        return m_file_import_executor->is_importing();
    }
    return false;
}

FileImportConfig* PipelineManager::get_import_config()
{
    return m_import_config.get();
}

bool PipelineManager::get_import_progress(std::string& progress_text, float& progress_value)
{
    if (m_file_import_executor)
    {
        return m_file_import_executor->get_progress(progress_text, progress_value);
    }
    return false;
}

bool PipelineManager::handle_import_completion(std::shared_ptr<pnanovdb_raster_gaussian_data_t>& old_gaussian_data_ptr)
{
    if (m_file_import_executor)
    {
        return m_file_import_executor->handle_completion(old_gaussian_data_ptr);
    }
    return false;
}

void PipelineManager::configure_pipeline(pnanovdb_editor_token_t* scene,
                                         pnanovdb_editor_token_t* object,
                                         PipelineType type,
                                         const pnanovdb_pipeline_settings_t* settings,
                                         pnanovdb_editor_token_t* name)
{
    if (!scene || !object || !settings)
    {
        return;
    }

    PipelineConfig* config = get_object_pipeline(scene, object, type, name);
    if (!config)
    {
        Console::getInstance().addLog(Console::LogLevel::Warning,
                                      "configure_pipeline: Pipeline not found for object '%s' type %d", object->str,
                                      (int)type);
        return;
    }

    bool modified = false;

    // Apply shader path if specified
    if (settings->shader_path != nullptr)
    {
        const char* entry = settings->shader_entry_point ? settings->shader_entry_point : "main";
        if (config->set_shader(0, settings->shader_path, entry))
        {
            Console::getInstance().addLog(Console::LogLevel::Debug,
                                          "configure_pipeline: Set shader '%s' entry '%s' for object '%s'",
                                          settings->shader_path, entry, object->str);
            modified = true;
        }
    }

    // Apply named parameters (TODO: implement when params storage is a map)
    if (settings->params && settings->param_count > 0)
    {
        Console::getInstance().addLog(
            Console::LogLevel::Warning, "configure_pipeline: Named parameters not yet supported");
    }

    // Mark dirty if modified
    if (modified)
    {
        config->status = PipelineStatus::Dirty;
        config->needs_run = true;
    }
}

} // namespace pnanovdb_editor
