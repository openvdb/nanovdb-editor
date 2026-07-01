// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   editor/PipelineRuntime.cpp

    \brief  Per-editor async pipeline runtime state
*/

#include "PipelineRuntime.h"

#include "Console.h"
#include "Editor.h"
#include "EditorScene.h"
#include "EditorSceneManager.h"
#include "EditorToken.h"
#include "Profiler.h"
#include "Renderer.h"
#include "raster/Raster.h"

#include "nanovdb_editor/putil/FileFormat.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <vector>

namespace pnanovdb_editor
{

// ============================================================================
// Internal helpers
// ============================================================================

namespace
{

const char* voxelbvh_source_label(VoxelBVHBuildSource source)
{
    switch (source)
    {
    case VoxelBVHBuildSource::GaussianFile:
        return "GaussianFile";
    case VoxelBVHBuildSource::Triangles:
        return "Triangles";
    case VoxelBVHBuildSource::Lines:
        return "Lines";
    case VoxelBVHBuildSource::GaussianArrays:
        return "GaussianArrays";
    }
    return "<unknown>";
}

} // namespace

// ============================================================================
// RasterFileShaderParams (per-worker scratch state)
// ============================================================================

RasterFileShaderParams::~RasterFileShaderParams()
{
    reset();
}

void RasterFileShaderParams::prepare(const pnanovdb_compute_t* compute, EditorSceneManager* scene_manager)
{
    if (!compute)
    {
        return;
    }

    if (!m_initialized)
    {
        m_compute = compute;
        m_data_type = PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_raster_shader_params_t);
        const auto* defaults = static_cast<const pnanovdb_raster_shader_params_t*>(m_data_type->default_value);
        m_params = *defaults;
        m_initialized = true;
    }

    if (scene_manager)
    {
        if (m_arrays[pnanovdb_raster::gaussian_frag_color_slang])
        {
            m_compute->destroy_array(m_arrays[pnanovdb_raster::gaussian_frag_color_slang]);
        }
        m_arrays[pnanovdb_raster::gaussian_frag_color_slang] =
            scene_manager->shader_params.get_compute_array_for_shader("raster/gaussian_frag_color.slang", m_compute);
    }

    m_params.name = nullptr;
    m_params.data_type = m_data_type;
}

void RasterFileShaderParams::reset()
{
    if (m_compute)
    {
        for (auto& arr : m_arrays)
        {
            if (arr)
            {
                m_compute->destroy_array(arr);
                arr = nullptr;
            }
        }
    }
    m_initialized = false;
    m_compute = nullptr;
}

// ============================================================================
// AsyncWorker (shared base)
// ============================================================================

bool AsyncWorker::get_progress(std::string& text, float& value)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_worker || !m_enqueued)
    {
        return false;
    }
    if (m_worker->isTaskCompleted(m_task_id))
    {
        return false;
    }
    if (m_worker->isTaskRunning(m_task_id))
    {
        text = m_worker->getTaskProgressText(m_task_id);
        if (text.empty())
        {
            text = progress_running_fallback_text();
        }
        value = m_worker->getTaskProgress(m_task_id);
        return true;
    }
    text = progress_waiting_text();
    value = 0.0f;
    return true;
}

void AsyncWorker::request_user_cancel(SceneObject* scene_obj)
{
    if (scene_obj && !scene_obj->pipeline.process_user_cancel_requested)
    {
        return;
    }
    if (m_user_cancelled)
    {
        request_cancel();
        return;
    }
    m_user_cancelled = true;
    m_cancel_snapshot_restored = false;
    // restore pre-Run state immediately so the viewport responds without waiting for the worker
    with_pending_process_step(
        [this](SceneObject* obj, PipelineStage&)
        {
            if (obj->pipeline.process_run_snapshot)
            {
                obj->restore_process_run_snapshot();
                m_cancel_snapshot_restored = true;
            }
        });
    request_cancel();
}

bool AsyncWorker::user_cancel_requested() const
{
    return m_user_cancelled;
}

bool AsyncWorker::consume_user_cancelled()
{
    if (!m_user_cancelled)
    {
        return false;
    }
    const bool snapshot_already_restored = m_cancel_snapshot_restored;
    m_cancel_snapshot_restored = false;
    if (!snapshot_already_restored)
    {
        with_pending_process_step(
            [](SceneObject* obj, PipelineStage&)
            {
                if (obj->pipeline.process_run_snapshot)
                {
                    obj->restore_process_run_snapshot();
                }
                else
                {
                    const size_t running_step = static_cast<size_t>(obj->pipeline.active_process_step);
                    obj->cancel_running_process_step_without_snapshot(running_step);
                }
            });
    }
    return true;
}

void AsyncWorker::clear_pending_cancel_ui()
{
    with_pending_object([](SceneObject* obj) { obj->clear_process_cancel_state(); });
}

bool AsyncWorker::pending_target_matches(uint64_t scene_id, uint64_t name_id, uint64_t lifetime_id) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return lifetime_id != 0 && m_pending_scene_token_id == scene_id && m_pending_name_token_id == name_id &&
           m_pending_object_lifetime_id == lifetime_id;
}

bool AsyncWorker::retarget_pending_target(
    uint64_t old_scene_id, uint64_t old_name_id, uint64_t new_scene_id, uint64_t new_name_id, uint64_t lifetime_id)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_enqueued || lifetime_id == 0 || m_pending_object_lifetime_id != lifetime_id)
    {
        return false;
    }
    if (m_pending_scene_token_id == new_scene_id && m_pending_name_token_id == new_name_id)
    {
        return true;
    }
    if (m_pending_scene_token_id != old_scene_id || m_pending_name_token_id != old_name_id)
    {
        return false;
    }
    m_pending_scene_token_id = new_scene_id;
    m_pending_name_token_id = new_name_id;
    return true;
}

void AsyncWorker::set_pending_target(SceneObject* scene_obj,
                                     EditorSceneManager* scene_manager,
                                     const pnanovdb_compute_t* compute)
{
    m_pending_scene_token_id = (scene_obj && scene_obj->scene_token) ? scene_obj->scene_token->id : 0;
    m_pending_name_token_id = (scene_obj && scene_obj->name_token) ? scene_obj->name_token->id : 0;
    m_pending_object_lifetime_id = scene_obj ? scene_obj->lifetime_id : 0;
    m_pending_scene_manager = scene_manager;
    m_pending_compute = compute;
    m_pending_process_step = scene_obj ? scene_obj->pipeline.active_process_step : -1;
    if (scene_obj && m_pending_process_step >= 0 && (size_t)m_pending_process_step < scene_obj->pipeline.process_count())
    {
        const PipelineStage& step = scene_obj->pipeline.process_step((size_t)m_pending_process_step);
        m_pending_process_type = step.type;
        m_pending_process_revision = step.revision;
    }
    else
    {
        m_pending_process_type = pnanovdb_pipeline_type_noop;
        m_pending_process_revision = 0;
    }
}

void AsyncWorker::pending_target_tokens(pnanovdb_editor_token_t** scene, pnanovdb_editor_token_t** name) const
{
    uint64_t scene_id = 0;
    uint64_t name_id = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        scene_id = m_pending_scene_token_id;
        name_id = m_pending_name_token_id;
    }
    if (scene)
        *scene = EditorToken::getInstance().getTokenById(scene_id);
    if (name)
        *name = EditorToken::getInstance().getTokenById(name_id);
}

bool AsyncWorker::with_pending_object(const std::function<void(SceneObject*)>& fn)
{
    uint64_t scene_id = 0;
    uint64_t name_id = 0;
    uint64_t lifetime_id = 0;
    EditorSceneManager* scene_manager = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        scene_id = m_pending_scene_token_id;
        name_id = m_pending_name_token_id;
        lifetime_id = m_pending_object_lifetime_id;
        scene_manager = m_pending_scene_manager;
    }
    pnanovdb_editor_token_t* scene_token = EditorToken::getInstance().getTokenById(scene_id);
    pnanovdb_editor_token_t* name_token = EditorToken::getInstance().getTokenById(name_id);
    if (!scene_manager || !scene_token || !name_token || lifetime_id == 0)
    {
        return false;
    }
    bool found = false;
    uint64_t resolved_scene_id = 0;
    uint64_t resolved_name_id = 0;
    scene_manager->with_object_lifetime(scene_token, name_token, lifetime_id,
                                        [&](SceneObject* obj)
                                        {
                                            if (obj)
                                            {
                                                found = true;
                                                resolved_scene_id = obj->scene_token ? obj->scene_token->id : 0;
                                                resolved_name_id = obj->name_token ? obj->name_token->id : 0;
                                                fn(obj);
                                            }
                                        });
    if (found && resolved_scene_id != 0 && resolved_name_id != 0)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_pending_object_lifetime_id == lifetime_id && m_pending_scene_token_id == scene_id &&
            m_pending_name_token_id == name_id)
        {
            m_pending_scene_token_id = resolved_scene_id;
            m_pending_name_token_id = resolved_name_id;
        }
    }
    return found;
}

bool AsyncWorker::with_pending_process_step(const std::function<void(SceneObject*, PipelineStage&)>& fn)
{
    bool matched = false;
    with_pending_object(
        [&](SceneObject* obj)
        {
            if (m_pending_process_step < 0 || (size_t)m_pending_process_step >= obj->pipeline.process_count())
            {
                return;
            }
            PipelineStage& step = obj->pipeline.process_step((size_t)m_pending_process_step);
            if (step.type != m_pending_process_type || step.revision != m_pending_process_revision)
            {
                return;
            }
            obj->pipeline.active_process_step = m_pending_process_step;
            matched = true;
            fn(obj, step);
        });
    return matched;
}

// ============================================================================
// GaussianVoxelizeWorker - Gaussian -> NanoVDB conversion (gaussian_voxelize pipeline)
// ============================================================================

GaussianVoxelizeWorker::~GaussianVoxelizeWorker()
{
    release();
}

void GaussianVoxelizeWorker::release_resources()
{
    release_compute_array(m_pending_compute, m_pending_nanovdb_array);
    pipeline_params_release(&m_pending_params);
    m_shader_params.reset();
}

void GaussianVoxelizeWorker::init(const PipelineContext& ctx, EditorScene* editor_scene)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    (void)ctx;
    m_editor_scene = editor_scene;
}

float GaussianVoxelizeWorker::get_running_voxels_per_unit()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return pipeline_params_get_voxels_per_unit(&m_pending_params);
}

bool GaussianVoxelizeWorker::start(SceneObject* scene_obj,
                                   EditorSceneManager* scene_manager,
                                   const pnanovdb_pipeline_context_t* ctx)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!scene_obj || !scene_manager || !ctx || !ctx->raster || !ctx->compute)
    {
        Console::getInstance().addLog(Console::LogLevel::Error,
                                      "start_conversion: null pointer (obj=%p, mgr=%p, ctx=%p, raster=%p, compute=%p)",
                                      (void*)scene_obj, (void*)scene_manager, (void*)ctx,
                                      ctx ? (void*)ctx->raster : nullptr, ctx ? (void*)ctx->compute : nullptr);
        return false;
    }

    if (scene_obj->resources.source_filepath.empty())
    {
        Console::getInstance().addLog(
            Console::LogLevel::Error, "start_conversion: no source file path for re-conversion");
        return false;
    }

    pnanovdb_compute_queue_t* worker_queue = ctx->compute_queue ? ctx->compute_queue : ctx->queue;
    if (!worker_queue)
    {
        Console::getInstance().addLog(
            Console::LogLevel::Error, "start_conversion: no compute queue available for re-conversion");
        return false;
    }

    if (!m_worker || m_worker->hasRunningTask())
    {
        Console::getInstance().addLog("Warning: Conversion already in progress, skipping");
        return false;
    }

    set_pending_target(scene_obj, scene_manager, ctx->compute);
    m_pending_nanovdb_array = nullptr;

    pipeline_params_assign_copy(
        &scene_obj->pipeline.process_step((size_t)scene_obj->pipeline.active_process_step).params, &m_pending_params);

    m_pending_filepath = scene_obj->resources.source_filepath;
    m_enqueued = true;

    m_shader_params.prepare(ctx->compute, scene_manager);

    const float vpu = pipeline_params_get_voxels_per_unit(&m_pending_params);
    const float voxel_sz = 1.0f / vpu;

    m_task_id = m_worker->enqueue(
        [voxel_sz, this](pnanovdb_raster_t* raster, const pnanovdb_compute_t* compute, pnanovdb_compute_queue_t* queue,
                         const char* filepath, pnanovdb_compute_array_t** out_nanovdb,
                         pnanovdb_compute_array_t** shader_arrays_arg,
                         pnanovdb_raster_shader_params_t* raster_params) -> bool
        {
            return raster->raster_file(raster, compute, queue, filepath, voxel_sz, out_nanovdb,
                                       nullptr, // gaussian_data
                                       nullptr, // raster_context
                                       shader_arrays_arg, raster_params,
                                       nullptr, // profiler
                                       (void*)m_worker.get());
        },
        const_cast<pnanovdb_raster_t*>(ctx->raster), ctx->compute, worker_queue, m_pending_filepath.c_str(),
        &m_pending_nanovdb_array, m_shader_params.arrays(), m_shader_params.params());

    Console::getInstance().addLog("Starting re-conversion from '%s' (voxels_per_unit=%.1f, voxel_size=%.6f)...",
                                  m_pending_filepath.c_str(), vpu, voxel_sz);
    request_user_cancel(scene_obj);
    return true;
}

bool GaussianVoxelizeWorker::handle_completion()
{
    if (!pending_completion())
        return false;

    if (consume_user_cancelled())
    {
        release_compute_array(m_pending_compute, m_pending_nanovdb_array);
        m_pending_nanovdb_array = nullptr;
        pipeline_params_release(&m_pending_params);
        clear_pending_cancel_ui();
        finish_task();
        Console::getInstance().addLog("Gaussian->NanoVDB conversion cancelled");
        return true;
    }

    bool success = m_worker->isTaskSuccessful(m_task_id);
    const pnanovdb_compute_t* pending_compute = m_pending_compute;
    pnanovdb_editor_token_t* scene_token = nullptr;
    pnanovdb_editor_token_t* name_token = nullptr;
    pending_target_tokens(&scene_token, &name_token);

    Console::getInstance().addLog(Console::LogLevel::Debug,
                                  "handle_conversion_completion: success=%d, scene_token=%p, name_token=%p ('%s'), "
                                  "scene_manager=%p, nanovdb_array=%p",
                                  (int)success, (void*)scene_token, (void*)name_token,
                                  (name_token && name_token->str) ? name_token->str : "<null>",
                                  (void*)m_pending_scene_manager, (void*)m_pending_nanovdb_array);

    if (success && m_pending_nanovdb_array)
    {
        const bool object_found = with_pending_process_step(
            [&](SceneObject* scene_obj, PipelineStage& process_step)
            {
                std::shared_ptr<pnanovdb_compute_array_t> owner(m_pending_nanovdb_array,
                                                                [compute = pending_compute](pnanovdb_compute_array_t* arr)
                                                                {
                                                                    if (arr && compute)
                                                                        compute->destroy_array(arr);
                                                                });
                process_step.output.set_array(k_stage_output_nanovdb, m_pending_nanovdb_array, owner);

                scene_obj->render_pipeline() = pnanovdb_pipeline_type_nanovdb_render;
                pnanovdb_pipeline_get_default_params(pnanovdb_pipeline_type_nanovdb_render, &scene_obj->render_params());

                float used_vpu = pipeline_params_get_voxels_per_unit(&m_pending_params);
                float current_vpu = pipeline_params_get_voxels_per_unit(&process_step.params);
                bool params_changed = (current_vpu != used_vpu);
                scene_obj->advance_process_chain(true);
                if (params_changed)
                {
                    process_step.bump_revision();
                    process_step.dirty = true;
                }
                Console::getInstance().addLog(Console::LogLevel::Debug,
                                              "Conversion complete: used_vpu=%.1f, current_vpu=%.1f, dirty=%s",
                                              used_vpu, current_vpu, params_changed ? "true (re-convert)" : "false");

                scene_obj->shader_name() = nullptr;
                scene_obj->shader_params() = nullptr;
                scene_obj->shader_params_data_type() = nullptr;
            });

        if (object_found)
        {
            Console::getInstance().addLog("Gaussian->NanoVDB conversion complete");
        }
        else
        {
            if (pending_compute && m_pending_nanovdb_array)
            {
                pending_compute->destroy_array(m_pending_nanovdb_array);
            }
            Console::getInstance().addLog(
                Console::LogLevel::Warning,
                "Conversion completed but its scene object or process step was removed or changed");
        }
    }
    else
    {
        if (success)
        {
            Console::getInstance().addLog(
                Console::LogLevel::Error,
                "Conversion succeeded but produced no NanoVDB array (raster_file returned true with null output)");
        }
        else
        {
            Console::getInstance().addLog(Console::LogLevel::Error, "Gaussian->NanoVDB conversion failed");
        }
        with_pending_process_step([](SceneObject* scene_obj, PipelineStage&)
                                  { scene_obj->advance_process_chain(false); });
    }

    m_pending_nanovdb_array = nullptr;
    pipeline_params_release(&m_pending_params);
    clear_pending_cancel_ui();
    finish_task();

    if (m_editor_scene && scene_token && name_token)
    {
        m_editor_scene->update_scene_tree_after_conversion(scene_token, name_token);
    }

    return true;
}

// ============================================================================
// VoxelBVHWorker
// ============================================================================

static pnanovdb_bool_t voxelbvh_cancel_requested(void* userdata)
{
    const auto* cancel = static_cast<const std::atomic<pnanovdb_uint32_t>*>(userdata);
    return cancel && cancel->load(std::memory_order_relaxed) != 0u ? PNANOVDB_TRUE : PNANOVDB_FALSE;
}

VoxelBVHWorker::~VoxelBVHWorker()
{
    release();
}

void VoxelBVHWorker::finish_file_replacement(bool success)
{
    EditorSceneManager* scene_manager = nullptr;
    uint64_t lifetime_id = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        scene_manager = m_pending_scene_manager;
        lifetime_id = m_pending_object_lifetime_id;
    }
    if (!scene_manager || lifetime_id == 0)
        return;

    std::shared_ptr<pnanovdb_raster_gaussian_data_t> old_gaussian_owner;
    if (scene_manager->finish_file_object_replacement(lifetime_id, success, &old_gaussian_owner) &&
        old_gaussian_owner && m_editor_scene && m_editor_scene->get_editor())
    {
        defer_gaussian_data_destruction(m_editor_scene->get_editor()->impl, std::move(old_gaussian_owner));
    }
}

void VoxelBVHWorker::release_resources()
{
    finish_file_replacement(false);
    release_compute_array(m_pending_compute, m_pending_result);
    if (m_worker_ctx && m_iface && m_iface->destroy_context && m_worker_queue)
    {
        m_iface->destroy_context(m_iface->compute, m_worker_queue, m_worker_ctx);
        m_worker_ctx = nullptr;
    }
    m_pending_request = VoxelBVHBuildRequest{};
}

bool VoxelBVHWorker::start(SceneObject* scene_obj,
                           EditorSceneManager* scene_manager,
                           const pnanovdb_pipeline_context_t* ctx,
                           VoxelBVHBuildRequest req)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!scene_obj || !scene_manager || !ctx || !ctx->compute || !ctx->voxelbvh)
    {
        return false;
    }
    pnanovdb_compute_queue_t* worker_queue = ctx->compute_queue ? ctx->compute_queue : ctx->queue;
    if (!worker_queue)
    {
        return false;
    }
    if (!m_worker || m_worker->hasRunningTask())
    {
        return false;
    }

    switch (req.source)
    {
    case VoxelBVHBuildSource::GaussianFile:
        if (req.filepath.empty() || !ctx->voxelbvh->nanovdb_from_gaussians_file)
            return false;
        break;
    case VoxelBVHBuildSource::Triangles:
        if (!ctx->voxelbvh->nanovdb_from_triangles_array || !req.array_ptrs[0] || !req.array_ptrs[1] ||
            !req.array_ptrs[2])
            return false;
        break;
    case VoxelBVHBuildSource::Lines:
        if (!ctx->voxelbvh->nanovdb_from_lines_array || !req.array_ptrs[0] || !req.array_ptrs[1] || !req.array_ptrs[2])
            return false;
        break;
    case VoxelBVHBuildSource::GaussianArrays:
        if (!ctx->voxelbvh->nanovdb_from_gaussians_array)
            return false;
        for (int i = 0; i < VoxelBVHBuildRequest::k_max_arrays; ++i)
        {
            if (!req.array_ptrs[i])
                return false;
        }
        break;
    }

    m_iface = ctx->voxelbvh;
    m_worker_queue = worker_queue;

    set_pending_target(scene_obj, scene_manager, ctx->compute);
    m_pending_request = std::move(req);
    m_pending_result = nullptr;
    m_cancel.store(0u, std::memory_order_relaxed);
    m_enqueued = true;

    m_task_id = m_worker->enqueue(
        [this]() -> bool
        {
            if (!m_worker_ctx && m_iface && m_iface->create_context && m_worker_queue)
            {
                m_worker_ctx = m_iface->create_context(m_iface->compute, m_worker_queue);
            }
            if (!m_worker_ctx || !m_iface || !m_pending_compute || !m_worker_queue)
            {
                return false;
            }

            if (m_iface->context_set_cancel)
            {
                m_iface->context_set_cancel(m_worker_ctx, voxelbvh_cancel_requested, &m_cancel);
            }

            const VoxelBVHBuildRequest& r = m_pending_request;
            switch (r.source)
            {
            case VoxelBVHBuildSource::GaussianFile:
                m_pending_result = m_iface->nanovdb_from_gaussians_file(
                    m_pending_compute, m_worker_queue, m_worker_ctx, r.filepath.c_str(), r.resolution);
                break;
            case VoxelBVHBuildSource::Triangles:
                m_pending_result = m_iface->nanovdb_from_triangles_array(
                    m_pending_compute, m_worker_queue, m_worker_ctx, r.array_ptrs[0], r.array_ptrs[1], r.array_ptrs[2],
                    r.inflation_radius, r.resolution);
                break;
            case VoxelBVHBuildSource::Lines:
                m_pending_result = m_iface->nanovdb_from_lines_array(m_pending_compute, m_worker_queue, m_worker_ctx,
                                                                     r.array_ptrs[0], r.array_ptrs[1], r.array_ptrs[2],
                                                                     r.inflation_radius, r.resolution);
                break;
            case VoxelBVHBuildSource::GaussianArrays:
            {
                pnanovdb_compute_array_t* arrays[VoxelBVHBuildRequest::k_max_arrays] = {
                    r.array_ptrs[0], r.array_ptrs[1], r.array_ptrs[2], r.array_ptrs[3], r.array_ptrs[4], r.array_ptrs[5]
                };
                m_pending_result = m_iface->nanovdb_from_gaussians_array(
                    m_pending_compute, m_worker_queue, m_worker_ctx, arrays,
                    (pnanovdb_uint32_t)VoxelBVHBuildRequest::k_max_arrays, r.resolution);
                break;
            }
            }
            return m_pending_result != nullptr;
        });

    if (m_pending_request.source == VoxelBVHBuildSource::GaussianFile)
    {
        Console::getInstance().addLog("Starting VoxelBVH build from '%s'...", m_pending_request.filepath.c_str());
    }
    else
    {
        Console::getInstance().addLog("Starting VoxelBVH build (%s)...", voxelbvh_source_label(m_pending_request.source));
    }
    request_user_cancel(scene_obj);
    return true;
}

bool VoxelBVHWorker::handle_completion()
{
    if (!pending_completion())
    {
        return false;
    }

    const pnanovdb_compute_t* pending_compute = m_pending_compute;

    if (consume_user_cancelled())
    {
        release_compute_array(pending_compute, m_pending_result);
        finish_file_replacement(false);
        m_pending_request = VoxelBVHBuildRequest{};
        m_pending_result = nullptr;
        clear_pending_cancel_ui();
        finish_task();
        Console::getInstance().addLog("VoxelBVH build cancelled");
        return true;
    }

    const bool success = m_worker->isTaskSuccessful(m_task_id);
    pnanovdb_compute_array_t* result_array = m_pending_result;

    const VoxelBVHBuildSource source = m_pending_request.source;
    const std::string& source_filepath = m_pending_request.filepath;
    auto describe_input = [&]() -> std::string
    {
        if (source == VoxelBVHBuildSource::GaussianFile)
        {
            return "'" + source_filepath + "'";
        }
        return std::string("(") + voxelbvh_source_label(source) + ")";
    };

    if (success && result_array)
    {
        const bool object_found = with_pending_process_step(
            [&](SceneObject* obj, PipelineStage& process_step)
            {
                std::shared_ptr<pnanovdb_compute_array_t> owner(
                    result_array,
                    [pending_compute](pnanovdb_compute_array_t* arr)
                    {
                        if (arr && pending_compute && pending_compute->destroy_array)
                            pending_compute->destroy_array(arr);
                    });
                process_step.output.set_array(k_stage_output_nanovdb, result_array, owner);
                obj->sync_render_to_chain();
                obj->advance_process_chain(true);
            });

        if (object_found)
        {
            finish_file_replacement(true);
            Console::getInstance().addLog("VoxelBVH build of %s was successful (%llu elements)",
                                          describe_input().c_str(), (unsigned long long)result_array->element_count);
        }
        else
        {
            if (pending_compute && pending_compute->destroy_array)
                pending_compute->destroy_array(result_array);
            finish_file_replacement(false);
            Console::getInstance().addLog(
                Console::LogLevel::Warning,
                "VoxelBVH build completed but its scene object or process step was removed or changed");
        }
    }
    else
    {
        if (success && !result_array)
        {
            Console::getInstance().addLog(
                Console::LogLevel::Error, "VoxelBVH build of %s returned null", describe_input().c_str());
        }
        else if (!success)
        {
            Console::getInstance().addLog(
                Console::LogLevel::Error, "VoxelBVH build of %s failed", describe_input().c_str());
        }
        with_pending_process_step([](SceneObject* obj, PipelineStage&) { obj->advance_process_chain(false); });
        finish_file_replacement(false);
    }

    m_pending_request = VoxelBVHBuildRequest{};
    m_pending_result = nullptr;
    clear_pending_cancel_ui();
    finish_task();

    return true;
}

// ============================================================================
// VoxelBVHRgba8Worker
// ============================================================================

VoxelBVHRgba8Worker::~VoxelBVHRgba8Worker()
{
    release();
}

void VoxelBVHRgba8Worker::release_resources()
{
    release_compute_array(m_pending_compute, m_pending_result);
    m_pending_src = nullptr;
    m_pending_src_owner.reset();
    if (m_worker_ctx && m_iface && m_iface->destroy_context && m_worker_queue)
    {
        m_iface->destroy_context(m_iface->compute, m_worker_queue, m_worker_ctx);
        m_worker_ctx = nullptr;
    }
}

static void rgba8_progress_to_worker(void* userdata, float fraction)
{
    auto* worker = static_cast<pnanovdb_util::WorkerThread*>(userdata);
    if (worker)
    {
        worker->updateTaskProgress(fraction, "Converting VoxelBVH to RGBA8");
    }
}

bool VoxelBVHRgba8Worker::start(SceneObject* scene_obj,
                                EditorSceneManager* scene_manager,
                                const pnanovdb_pipeline_context_t* ctx,
                                pnanovdb_compute_array_t* src_nanovdb,
                                std::shared_ptr<pnanovdb_compute_array_t> src_owner,
                                pnanovdb_uint32_t resolution,
                                pnanovdb_bool_t upsample)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!scene_obj || !scene_manager || !ctx || !ctx->compute || !ctx->voxelbvh)
    {
        return false;
    }
    if (!src_nanovdb || !ctx->voxelbvh->nanovdb_duplicate_topology_array ||
        !ctx->voxelbvh->nanovdb_rgba8_from_voxelbvh_array)
    {
        return false;
    }
    pnanovdb_compute_queue_t* worker_queue = ctx->compute_queue ? ctx->compute_queue : ctx->queue;
    if (!worker_queue)
    {
        return false;
    }
    if (!m_worker || m_worker->hasRunningTask())
    {
        return false;
    }

    m_iface = ctx->voxelbvh;
    m_worker_queue = worker_queue;

    set_pending_target(scene_obj, scene_manager, ctx->compute);
    m_pending_src = src_nanovdb;
    m_pending_src_owner = std::move(src_owner);
    m_pending_resolution = resolution;
    m_pending_upsample = upsample;
    m_pending_result = nullptr;
    m_cancel.store(0u, std::memory_order_relaxed);
    m_enqueued = true;

    m_task_id = m_worker->enqueue(
        [this]() -> bool
        {
            if (!m_worker_ctx && m_iface && m_iface->create_context && m_worker_queue)
            {
                m_worker_ctx = m_iface->create_context(m_iface->compute, m_worker_queue);
            }
            if (!m_worker_ctx || !m_iface || !m_pending_compute || !m_worker_queue || !m_pending_src)
            {
                return false;
            }

            if (m_iface->context_set_cancel)
            {
                m_iface->context_set_cancel(m_worker_ctx, voxelbvh_cancel_requested, &m_cancel);
            }

            if (m_iface->context_set_progress)
            {
                m_iface->context_set_progress(m_worker_ctx, rgba8_progress_to_worker, m_worker.get());
            }

            m_worker->updateTaskProgress(0.f, "Converting VoxelBVH to RGBA8");

            pnanovdb_compute_array_t* dst = nullptr;
            const pnanovdb_uint32_t upsample_factor = m_pending_upsample ? 2u : 0u;
            m_iface->nanovdb_duplicate_topology_array(m_pending_compute, m_worker_queue, m_worker_ctx, &dst,
                                                      m_pending_src, PNANOVDB_GRID_TYPE_RGBA8, upsample_factor);
            if (!dst)
            {
                return false;
            }
            const pnanovdb_vec3_t index_space_ray_direction = { 0.f, 0.f, -1.f };
            m_iface->nanovdb_rgba8_from_voxelbvh_array(
                m_pending_compute, m_worker_queue, m_worker_ctx, dst, m_pending_src, index_space_ray_direction);
            m_pending_result = dst;
            return m_pending_result != nullptr;
        });

    Console::getInstance().addLog(
        "Starting VoxelBVH -> RGBA8 conversion (resolution=%u, upsample=%d)...", (unsigned)resolution, (int)upsample);
    request_user_cancel(scene_obj);
    return true;
}

bool VoxelBVHRgba8Worker::handle_completion()
{
    if (!pending_completion())
    {
        return false;
    }

    const pnanovdb_compute_t* pending_compute = m_pending_compute;

    if (consume_user_cancelled())
    {
        release_compute_array(pending_compute, m_pending_result);
        m_pending_src = nullptr;
        m_pending_src_owner.reset();
        m_pending_result = nullptr;
        clear_pending_cancel_ui();
        finish_task();
        Console::getInstance().addLog("VoxelBVH -> RGBA8 conversion cancelled");
        return true;
    }

    const bool success = m_worker->isTaskSuccessful(m_task_id);
    pnanovdb_compute_array_t* result_array = m_pending_result;

    if (success && result_array)
    {
        const bool object_found = with_pending_process_step(
            [&](SceneObject* obj, PipelineStage& process_step)
            {
                std::shared_ptr<pnanovdb_compute_array_t> owner(
                    result_array,
                    [pending_compute](pnanovdb_compute_array_t* arr)
                    {
                        if (arr && pending_compute && pending_compute->destroy_array)
                            pending_compute->destroy_array(arr);
                    });
                process_step.output.set_array(k_stage_output_nanovdb, result_array, owner);
                obj->sync_render_to_chain();
                obj->advance_process_chain(true);
            });

        if (object_found)
        {
            Console::getInstance().addLog("VoxelBVH -> RGBA8 conversion complete (%llu elements)",
                                          (unsigned long long)result_array->element_count);
        }
        else
        {
            if (pending_compute && pending_compute->destroy_array)
                pending_compute->destroy_array(result_array);
            Console::getInstance().addLog(
                Console::LogLevel::Warning,
                "VoxelBVH -> RGBA8 conversion completed but its scene object or process step was removed or changed");
        }
    }
    else
    {
        if (success && !result_array)
        {
            Console::getInstance().addLog(Console::LogLevel::Error, "VoxelBVH -> RGBA8 conversion returned null");
        }
        else if (!success)
        {
            Console::getInstance().addLog(Console::LogLevel::Error, "VoxelBVH -> RGBA8 conversion failed");
        }
        with_pending_process_step([](SceneObject* obj, PipelineStage&) { obj->advance_process_chain(false); });
    }

    m_pending_src = nullptr;
    m_pending_src_owner.reset();
    m_pending_result = nullptr;
    clear_pending_cancel_ui();
    finish_task();

    return true;
}

// ============================================================================
// GaussianLoadWorker - file import -> Gaussian splats / NanoVDB (gaussian_load pipeline)
// ============================================================================

GaussianLoadWorker::~GaussianLoadWorker()
{
    release();
}

void GaussianLoadWorker::release_resources()
{
    release_pending_resources();
    pipeline_params_release(&m_pending_process_params);
    m_shader_params.reset();
}

void GaussianLoadWorker::release_pending_resources()
{
    release_compute_array(m_compute, m_pending_nanovdb_array);

    pnanovdb_compute_queue_t* queue = cleanup_queue();
    if (m_raster && m_compute && queue)
    {
        if (m_pending_gaussian_data && m_raster->destroy_gaussian_data)
        {
            m_raster->destroy_gaussian_data(m_compute, queue, m_pending_gaussian_data);
            m_pending_gaussian_data = nullptr;
        }
        if (m_pending_raster_ctx && m_raster->destroy_context)
        {
            m_raster->destroy_context(m_compute, queue, m_pending_raster_ctx);
            m_pending_raster_ctx = nullptr;
        }
    }
}

void GaussianLoadWorker::init(const PipelineContext& ctx, EditorScene* editor_scene)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    m_compute = ctx.compute;
    m_raster = ctx.raster;
    m_device_queue = ctx.queue;
    m_compute_queue = ctx.compute_queue;
    m_editor_scene = editor_scene;
}

bool GaussianLoadWorker::start(const char* raster_filepath,
                               const pnanovdb_pipeline_params_t* process_params,
                               bool rasterize_to_nanovdb,
                               pnanovdb_pipeline_type_t process_pipeline,
                               pnanovdb_pipeline_type_t render_pipeline,
                               EditorSceneManager* scene_manager,
                               pnanovdb_editor_token_t* scene_token,
                               pnanovdb_editor_token_t* name_token,
                               uint64_t reservation_id)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_compute || !m_raster)
    {
        Console::getInstance().addLog(
            Console::LogLevel::Error,
            "GaussianLoadWorker::start: not initialized -- pipeline_init must run before start()");
        return false;
    }
    if (!raster_filepath)
    {
        return false;
    }

    if (!m_worker || m_worker->hasRunningTask())
    {
        Console::getInstance().addLog("Error: Rasterization already in progress");
        return false;
    }

    m_pending_process_pipeline = process_pipeline;
    m_pending_render_pipeline = render_pipeline;
    pnanovdb_pipeline_get_default_params(process_pipeline, &m_pending_process_params);
    if (process_params && process_params->data && process_params->size > 0)
    {
        pipeline_params_assign_copy(process_params, &m_pending_process_params);
    }

    m_pending_filepath = raster_filepath;
    m_pending_scene_token = scene_token;
    m_pending_name_token = name_token;
    m_pending_reservation_id = reservation_id;

    m_pending_nanovdb_array = nullptr;
    m_pending_gaussian_data = nullptr;
    m_pending_raster_ctx = nullptr;

    m_shader_params.prepare(m_compute, scene_manager);

    pnanovdb_compute_queue_t* worker_queue = m_compute_queue ? m_compute_queue : m_device_queue;

    pnanovdb_compute_interface_t* log_iface = m_compute->device_interface.get_compute_interface(m_device_queue);
    pnanovdb_compute_context_t* log_ctx = m_compute->device_interface.get_compute_context(m_device_queue);
    auto log_print = log_iface ? log_iface->get_log_print(log_ctx) : nullptr;
    if (log_print)
    {
        log_print(PNANOVDB_COMPUTE_LOG_LEVEL_INFO, "pipeline_load: worker_queue=%p (compute=%p device=%p) using_%s_queue",
                  (void*)worker_queue, (void*)m_compute_queue, (void*)m_device_queue,
                  (worker_queue == m_compute_queue) ? "compute" : "device");
    }

    m_enqueued = true;

    const float voxel_size = 1.f / pipeline_params_get_voxels_per_unit(&m_pending_process_params);

    m_task_id = m_worker->enqueue(
        [this](pnanovdb_raster_t* raster, const pnanovdb_compute_t* compute, pnanovdb_compute_queue_t* queue,
               const char* filepath, float voxel_size_arg, pnanovdb_compute_array_t** nanovdb_array,
               pnanovdb_raster_gaussian_data_t** gaussian_data, pnanovdb_raster_context_t** raster_context,
               pnanovdb_compute_array_t** shader_params_arrays, pnanovdb_raster_shader_params_t* raster_params,
               pnanovdb_profiler_report_t profiler) -> bool
        {
            return raster->raster_file(raster, compute, queue, filepath, voxel_size_arg, nanovdb_array, gaussian_data,
                                       raster_context, shader_params_arrays, raster_params, profiler,
                                       (void*)m_worker.get());
        },
        m_raster, m_compute, worker_queue, m_pending_filepath.c_str(), voxel_size,
        rasterize_to_nanovdb ? &m_pending_nanovdb_array : nullptr,
        rasterize_to_nanovdb ? nullptr : &m_pending_gaussian_data, rasterize_to_nanovdb ? nullptr : &m_pending_raster_ctx,
        m_shader_params.arrays(), m_shader_params.params(), Profiler::report_callback);

    Console::getInstance().addLog("Running rasterization: '%s'...", m_pending_filepath.c_str());
    return true;
}

bool GaussianLoadWorker::start_from_request(const PipelineLoadRequest& request,
                                            EditorSceneManager* scene_manager,
                                            pnanovdb_editor_token_t* scene_token)
{
    if (request.load_pipeline != pnanovdb_pipeline_type_gaussian_load)
    {
        return false;
    }
    const bool rasterize_to_nanovdb = (request.process_pipeline == pnanovdb_pipeline_type_gaussian_voxelize);
    return start(request.source_filepath, request.process_params, rasterize_to_nanovdb, request.process_pipeline,
                 request.render_pipeline, scene_manager, scene_token, request.name_token, request.reservation_id);
}

bool GaussianLoadWorker::handle_completion()
{
    if (!pending_completion())
    {
        return false;
    }

    EditorScene* editor_scene = m_editor_scene;
    pnanovdb_editor_token_t* scene_token = nullptr;
    pnanovdb_editor_token_t* name_token = nullptr;
    uint64_t target_lifetime_id = 0;
    const bool target_valid =
        editor_scene && editor_scene->resolve_async_load_target(
                            m_pending_reservation_id, &scene_token, &name_token, &target_lifetime_id);
    bool consumed = false;
    if (m_worker->isTaskSuccessful(m_task_id) && target_valid)
    {
        if (m_pending_nanovdb_array)
        {
            consumed = editor_scene->handle_nanovdb_data_load(
                scene_token, m_pending_nanovdb_array, m_pending_filepath.c_str(), pnanovdb_pipeline_type_nanovdb_render,
                name_token, target_lifetime_id);

            if (consumed && m_pending_process_pipeline == pnanovdb_pipeline_type_gaussian_voxelize)
            {
                auto* scene_manager = editor_scene->get_scene_manager();
                if (scene_manager)
                {
                    scene_manager->with_object(
                        scene_token, name_token,
                        [&](SceneObject* obj)
                        {
                            if (!obj)
                                return;

                            obj->resources.source_filepath = m_pending_filepath;
                            obj->load_pipeline() = pnanovdb_pipeline_type_gaussian_load;
                            obj->process_pipeline() = m_pending_process_pipeline;
                            obj->pipeline.process().bump_revision();
                            obj->process_dirty() = false;
                            pnanovdb_pipeline_get_default_params(m_pending_process_pipeline, &obj->process_params());
                            if (m_pending_process_params.data && m_pending_process_params.size > 0)
                            {
                                pipeline_params_assign_copy(&m_pending_process_params, &obj->process_params());
                            }

                            float pending_vpu = pipeline_params_get_voxels_per_unit(&m_pending_process_params);
                            Console::getInstance().addLog(
                                Console::LogLevel::Debug,
                                "Rasterization: set gaussian_voxelize pipeline on '%s' (vpu=%.1f, filepath='%s')",
                                name_token && name_token->str ? name_token->str : "?", pending_vpu,
                                m_pending_filepath.c_str());
                        });
                }
            }
        }
        else if (m_pending_gaussian_data)
        {
            consumed = editor_scene->handle_gaussian_data_load(
                scene_token, m_pending_gaussian_data, m_shader_params.params(), m_pending_filepath.c_str(),
                m_pending_process_pipeline, m_pending_render_pipeline,
                m_pending_process_params.data ? &m_pending_process_params : nullptr, name_token, target_lifetime_id);
        }
        if (consumed)
        {
            m_pending_nanovdb_array = nullptr;
            m_pending_gaussian_data = nullptr;
            Console::getInstance().addLog("Rasterization of '%s' was successful", m_pending_filepath.c_str());
            editor_scene->sync_selected_view_with_current();
        }
    }
    else if (!editor_scene)
    {
        Console::getInstance().addLog(
            Console::LogLevel::Error,
            "GaussianLoadWorker::handle_completion: no EditorScene captured -- init() must run before handle_completion");
    }
    else
    {
        Console::getInstance().addLog(target_valid ? "Rasterization of '%s' failed" :
                                                     "Discarding rasterization of '%s': target changed or was removed",
                                      m_pending_filepath.c_str());
    }

    if (editor_scene)
        editor_scene->finish_async_load_target(m_pending_reservation_id, consumed);
    release_pending_resources();

    m_pending_filepath = "";
    m_pending_scene_token = nullptr;
    m_pending_name_token = nullptr;
    m_pending_reservation_id = 0;
    pipeline_params_release(&m_pending_process_params);
    clear_pending_cancel_ui();
    finish_task();

    return true;
}

// ============================================================================
// MeshLoadWorker - PLY file import -> position/index/(color) compute arrays
// ============================================================================

MeshLoadWorker::~MeshLoadWorker()
{
    release();
}

void MeshLoadWorker::release_resources()
{
    release_pending_arrays();
}

void MeshLoadWorker::release_pending_arrays()
{
    release_compute_array(m_compute, m_pending_positions);
    release_compute_array(m_compute, m_pending_indices);
    release_compute_array(m_compute, m_pending_colors);
}

void MeshLoadWorker::init(const PipelineContext& ctx, EditorScene* editor_scene)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_compute = ctx.compute;
    m_editor_scene = editor_scene;
}

bool MeshLoadWorker::start(const char* mesh_filepath,
                           const MeshLoadParams& load_params,
                           pnanovdb_editor_token_t* scene_token,
                           pnanovdb_editor_token_t* name_token,
                           uint64_t reservation_id)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_compute)
    {
        Console::getInstance().addLog(
            Console::LogLevel::Error, "MeshLoadWorker::start: not initialized -- pipeline_init must run before start()");
        return false;
    }
    if (!mesh_filepath)
    {
        return false;
    }
    if (!m_worker || m_worker->hasRunningTask())
    {
        Console::getInstance().addLog("Error: Mesh load already in progress");
        return false;
    }

    m_pending_filepath = mesh_filepath;
    m_pending_params = load_params;
    m_pending_scene_token = scene_token;
    m_pending_name_token = name_token;
    m_pending_reservation_id = reservation_id;
    m_pending_positions = nullptr;
    m_pending_indices = nullptr;
    m_pending_colors = nullptr;
    m_enqueued = true;

    m_task_id = m_worker->enqueue(
        [this]() -> bool
        {
            pnanovdb_fileformat_t fileformat = {};
            pnanovdb_fileformat_load(&fileformat, m_compute);
            if (!fileformat.load_file)
            {
                Console::getInstance().addLog(Console::LogLevel::Error, "Import Mesh: file format module unavailable");
                pnanovdb_fileformat_free(&fileformat);
                return false;
            }

            static const char* array_names[] = { "positions", "indices", "colors" };
            pnanovdb_compute_array_t* arrays[3] = {};
            pnanovdb_bool_t loaded = fileformat.load_file(m_pending_filepath.c_str(), 3u, array_names, arrays);
            pnanovdb_fileformat_free(&fileformat);

            if (!loaded || !arrays[0] || !arrays[1])
            {
                for (int i = 0; i < 3; ++i)
                {
                    if (arrays[i])
                        m_compute->destroy_array(arrays[i]);
                }
                Console::getInstance().addLog(Console::LogLevel::Error,
                                              "Import Mesh: failed to read positions+indices from '%s'",
                                              m_pending_filepath.c_str());
                return false;
            }

            m_pending_positions = arrays[0];
            m_pending_indices = arrays[1];
            m_pending_colors = arrays[2]; // may be nullptr -- handled on the render thread
            return true;
        });

    Console::getInstance().addLog("Loading mesh from '%s'...", m_pending_filepath.c_str());
    return true;
}

bool MeshLoadWorker::start_from_request(const PipelineLoadRequest& request,
                                        EditorSceneManager* scene_manager,
                                        pnanovdb_editor_token_t* scene_token)
{
    (void)scene_manager;
    if (request.load_pipeline != pnanovdb_pipeline_type_mesh_load)
    {
        return false;
    }
    MeshLoadParams load_params{};
    if (request.load_params)
    {
        load_params.inflation_radius = pipeline_params_get_mesh_load_inflation_radius(request.load_params);
        load_params.resolution = pipeline_params_get_mesh_load_resolution(request.load_params);
        load_params.show_debug = pipeline_params_get_mesh_load_show_debug(request.load_params) ? 1u : 0u;
    }
    m_pending_process_pipeline = request.process_pipeline;
    return start(request.source_filepath, load_params, scene_token, request.name_token, request.reservation_id);
}

bool MeshLoadWorker::handle_completion()
{
    if (!pending_completion())
    {
        return false;
    }

    EditorScene* editor_scene = m_editor_scene;
    const bool success = m_worker->isTaskSuccessful(m_task_id);
    bool reservation_consumed = false;

    auto cleanup_and_finish = [&]()
    {
        if (editor_scene)
            editor_scene->finish_async_load_target(m_pending_reservation_id, reservation_consumed);
        m_pending_filepath = "";
        m_pending_scene_token = nullptr;
        m_pending_name_token = nullptr;
        m_pending_reservation_id = 0;
        m_pending_params = MeshLoadParams{};
        m_pending_process_pipeline = pnanovdb_pipeline_type_voxelbvh_build;
        clear_pending_cancel_ui();
        finish_task();
    };

    if (!editor_scene)
    {
        Console::getInstance().addLog(
            Console::LogLevel::Error,
            "MeshLoadWorker::handle_completion: no EditorScene captured -- init() must run before handle_completion");
        release_pending_arrays();
        cleanup_and_finish();
        return true;
    }

    if (!success)
    {
        Console::getInstance().addLog("Mesh load of '%s' failed", m_pending_filepath.c_str());
        release_pending_arrays();
        cleanup_and_finish();
        return true;
    }

    pnanovdb_editor_token_t* target_scene = nullptr;
    pnanovdb_editor_token_t* target_name = nullptr;
    uint64_t target_lifetime_id = 0;
    if (!editor_scene->resolve_async_load_target(
            m_pending_reservation_id, &target_scene, &target_name, &target_lifetime_id))
    {
        Console::getInstance().addLog(Console::LogLevel::Warning,
                                      "Discarding mesh load of '%s': target changed or was removed",
                                      m_pending_filepath.c_str());
        release_pending_arrays();
        cleanup_and_finish();
        return true;
    }

    pnanovdb_compute_array_t* positions = m_pending_positions;
    pnanovdb_compute_array_t* indices = m_pending_indices;
    pnanovdb_compute_array_t* colors = m_pending_colors;

    const uint64_t index_element_count = indices->element_count;
    const uint32_t index_element_size = indices->element_size;
    const bool is_line_indices = (index_element_size == 2u * sizeof(uint32_t));
    const uint64_t prim_count = is_line_indices ? index_element_count : (index_element_count / 3u);
    const bool malformed = (index_element_count == 0u) || (!is_line_indices && (index_element_count % 3u != 0u));
    if (malformed)
    {
        Console::getInstance().addLog(
            Console::LogLevel::Error,
            "Import Mesh: index buffer is not a triangle stream and not a line stream (%llu indices, "
            "element_size=%u) in '%s'",
            (unsigned long long)index_element_count, (unsigned)index_element_size, m_pending_filepath.c_str());
        release_pending_arrays();
        cleanup_and_finish();
        return true;
    }

    auto synthesize_white_colors = [this](pnanovdb_compute_array_t* positions_arr) -> pnanovdb_compute_array_t*
    {
        std::vector<float> white(positions_arr->element_count, 1.0f);
        return m_compute->create_array(sizeof(float), positions_arr->element_count, white.data());
    };

    const bool has_ply_colors = (colors != nullptr);
    if (!has_ply_colors)
    {
        colors = synthesize_white_colors(positions);
        m_pending_colors = colors;
    }
    else if (colors->element_count != positions->element_count)
    {
        Console::getInstance().addLog(
            Console::LogLevel::Warning,
            "Import Mesh: '%s' has 'colors' with %llu floats but 'positions' has %llu; falling back to white",
            m_pending_filepath.c_str(), (unsigned long long)colors->element_count,
            (unsigned long long)positions->element_count);
        m_compute->destroy_array(colors);
        colors = synthesize_white_colors(positions);
        m_pending_colors = colors;
    }

    pnanovdb_pipeline_type_t effective_render_pipeline;
    const bool show_debug = (m_pending_params.show_debug != 0u);
    if (is_line_indices)
    {
        effective_render_pipeline = pnanovdb_pipeline_type_voxelbvh_lines_render;
        if (show_debug)
        {
            Console::getInstance().addLog(
                "Import Mesh: '%s' is a line PLY; the lines render pipeline has no debug variant, "
                "Show Debug is ignored",
                m_pending_filepath.c_str());
        }
    }
    else
    {
        effective_render_pipeline = show_debug ? pnanovdb_pipeline_type_voxelbvh_triangles_debug_render :
                                                 pnanovdb_pipeline_type_voxelbvh_triangles_render;
    }

    reservation_consumed = editor_scene->handle_mesh_data_load(
        target_scene, indices, positions, colors, m_pending_filepath.c_str(), effective_render_pipeline,
        is_line_indices, m_pending_params.inflation_radius, m_pending_params.resolution, m_pending_process_pipeline,
        target_name, target_lifetime_id);

    if (!reservation_consumed)
    {
        Console::getInstance().addLog(Console::LogLevel::Warning,
                                      "Discarding mesh load of '%s': target changed before publish",
                                      m_pending_filepath.c_str());
        release_pending_arrays();
        cleanup_and_finish();
        return true;
    }

    Console::getInstance().addLog(
        "Loaded mesh from '%s' (vertices=%llu, %s=%llu, colors=%s, render_pipeline=%d, inflation=%.5f, int_space=%u)",
        m_pending_filepath.c_str(), (unsigned long long)(positions->element_count / 3u),
        is_line_indices ? "lines" : "triangles", (unsigned long long)prim_count, has_ply_colors ? "ply" : "white",
        static_cast<int>(effective_render_pipeline), m_pending_params.inflation_radius, m_pending_params.resolution);

    m_pending_positions = nullptr;
    m_pending_indices = nullptr;
    m_pending_colors = nullptr;

    editor_scene->sync_selected_view_with_current();

    cleanup_and_finish();
    return true;
}

// ============================================================================
// PipelineRuntime
// ============================================================================

PNANOVDB_REGISTER_WORKER(GaussianLoadWorker);
PNANOVDB_REGISTER_WORKER(MeshLoadWorker);
PNANOVDB_REGISTER_WORKER(GaussianVoxelizeWorker);
PNANOVDB_REGISTER_WORKER(VoxelBVHWorker);
PNANOVDB_REGISTER_WORKER(VoxelBVHRgba8Worker);

PipelineRuntime::PipelineRuntime()
{
    const auto& factories = detail::get_worker_factories();
    m_workers.reserve(factories.size());
    for (AsyncWorkerFactory factory : factories)
    {
        if (factory)
        {
            m_workers.push_back(factory());
        }
    }
}

PipelineRuntime::~PipelineRuntime()
{
    quiesce();
}

void PipelineRuntime::set_editor_scene(EditorScene* editor_scene)
{
    if (editor_scene && !m_session_active)
    {
        for (auto& worker : m_workers)
        {
            if (worker)
            {
                worker->prepare_for_session();
            }
        }
    }
    m_editor_scene = editor_scene;
    m_session_active = editor_scene != nullptr;
}

void PipelineRuntime::quiesce()
{
    if (!m_session_active && !any_worker_busy())
        return;

    begin_quiesce();
    finish_quiesce();
}

void PipelineRuntime::begin_quiesce()
{
    for (auto& worker : m_workers)
    {
        if (worker)
        {
            worker->cancel_and_join();
        }
    }
}

void PipelineRuntime::finish_quiesce()
{
    for (auto& worker : m_workers)
    {
        if (worker)
        {
            worker->release_after_join();
        }
    }
    m_editor_scene = nullptr;
    m_session_active = false;
}

// ============================================================================
// Worker factory registry (singleton; populated at static-init time)
// ============================================================================

namespace detail
{
namespace
{
std::vector<AsyncWorkerFactory>& mutable_worker_factories()
{
    static std::vector<AsyncWorkerFactory> factories;
    return factories;
}
} // namespace

void register_worker_factory(AsyncWorkerFactory factory)
{
    if (factory)
    {
        mutable_worker_factories().push_back(factory);
    }
}

const std::vector<AsyncWorkerFactory>& get_worker_factories()
{
    return mutable_worker_factories();
}
} // namespace detail

// ============================================================================
// Thread-local "current runtime"
// ============================================================================

namespace
{
thread_local PipelineRuntime* g_current_runtime = nullptr;
}

PipelineRuntime* current_runtime()
{
    return g_current_runtime;
}

namespace detail
{
void set_current_runtime(PipelineRuntime* runtime)
{
    g_current_runtime = runtime;
}
} // namespace detail

RuntimeScope::RuntimeScope(PipelineRuntime* runtime) : m_previous(g_current_runtime)
{
    g_current_runtime = runtime;
}

RuntimeScope::~RuntimeScope()
{
    g_current_runtime = m_previous;
}

} // namespace pnanovdb_editor
