// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/editor/EditorScene.cpp

    \author Petra Hapalova

    \brief  Handles all view switching and state management
*/

#include "EditorScene.h"
#include "SceneView.h"
#include "Editor.h"
#include "EditorImport.h"
#include "EditorSceneManager.h"
#include "EditorToken.h"
#include "ShaderMonitor.h"
#include "ShaderCompileUtils.h"
#include "Console.h"
#include "Pipeline.h"
#include "PipelineRuntime.h"
#include "PipelineRegistry.h"
#include "SceneSerializer.h"

#include "raster/Raster.h"

#include <nanovdb/io/IO.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>
#include <vector>

namespace pnanovdb_editor
{

static pnanovdb_editor_token_t* resolve_import_name(pnanovdb_editor_token_t* name, const char* filename)
{
    if (name || !filename)
    {
        return name;
    }
    const std::string stem = std::filesystem::path(filename).stem().string();
    return EditorToken::getInstance().getToken(stem.c_str());
}

static bool decode_scene_pipeline_type(const nlohmann::json& stage_json,
                                       pnanovdb_pipeline_stage_t expected_stage,
                                       pnanovdb_pipeline_type_t& type,
                                       const char* object_name,
                                       const char* stage_name)
{
    pnanovdb_pipeline_type_t decoded = pnanovdb_pipeline_type_noop;
    std::string error;
    if (!pipeline_type_from_json(stage_json, decoded, &error))
    {
        Console::getInstance().addLog(Console::LogLevel::Warning,
                                      "Load scene: object '%s' has invalid %s pipeline (%s); preserving current stage",
                                      object_name ? object_name : "?", stage_name, error.c_str());
        return false;
    }
    const pnanovdb_pipeline_descriptor_t* desc = pnanovdb_pipeline_get_descriptor(decoded);
    if (decoded != pnanovdb_pipeline_type_noop && (!desc || desc->stage != expected_stage))
    {
        Console::getInstance().addLog(
            Console::LogLevel::Warning,
            "Load scene: object '%s' has pipeline %u in the wrong %s stage; preserving current stage",
            object_name ? object_name : "?", (unsigned)decoded, stage_name);
        return false;
    }
    type = decoded;
    return true;
}

static float json_float_or(const nlohmann::json& object, const char* key, float fallback)
{
    auto it = object.find(key);
    return it != object.end() && it->is_number() ? it->get<float>() : fallback;
}

static pnanovdb_bool_t json_bool_or(const nlohmann::json& object, const char* key, pnanovdb_bool_t fallback)
{
    auto it = object.find(key);
    return it != object.end() && it->is_boolean() ? (it->get<bool>() ? PNANOVDB_TRUE : PNANOVDB_FALSE) : fallback;
}

static void json_to_vec3(const nlohmann::json& value, pnanovdb_vec3_t& out)
{
    if (!value.is_array() || value.size() != 3 || !value[0].is_number() || !value[1].is_number() || !value[2].is_number())
    {
        return;
    }
    out = { value[0].get<float>(), value[1].get<float>(), value[2].get<float>() };
}

static CameraViewContext camera_from_json(const nlohmann::json& camera_json,
                                          pnanovdb_editor_token_t* name_token,
                                          pnanovdb_bool_t is_y_up)
{
    const auto entries_it = camera_json.find("entries");
    const size_t serialized_count = entries_it != camera_json.end() && entries_it->is_array() ? entries_it->size() : 0;
    const size_t count = std::max<size_t>(serialized_count, 1u);

    CameraViewContext context;
    context.camera_view = std::make_shared<pnanovdb_camera_view_t>();
    pnanovdb_camera_view_default(context.camera_view.get());
    context.camera_view->name = name_token;
    context.camera_view->num_cameras = (pnanovdb_uint32_t)count;
    context.camera_view->axis_length = json_float_or(camera_json, "axis_length", context.camera_view->axis_length);
    context.camera_view->axis_thickness =
        json_float_or(camera_json, "axis_thickness", context.camera_view->axis_thickness);
    context.camera_view->frustum_line_width =
        json_float_or(camera_json, "frustum_line_width", context.camera_view->frustum_line_width);
    context.camera_view->frustum_scale = json_float_or(camera_json, "frustum_scale", context.camera_view->frustum_scale);
    context.camera_view->is_visible = json_bool_or(camera_json, "visible", context.camera_view->is_visible);
    if (auto color = camera_json.find("frustum_color"); color != camera_json.end())
    {
        json_to_vec3(*color, context.camera_view->frustum_color);
    }

    context.camera_config = std::shared_ptr<pnanovdb_camera_config_t>(
        new pnanovdb_camera_config_t[count], [](pnanovdb_camera_config_t* ptr) { delete[] ptr; });
    context.camera_state = std::shared_ptr<pnanovdb_camera_state_t>(
        new pnanovdb_camera_state_t[count], [](pnanovdb_camera_state_t* ptr) { delete[] ptr; });
    context.camera_view->configs = context.camera_config.get();
    context.camera_view->states = context.camera_state.get();

    for (size_t i = 0; i < count; ++i)
    {
        pnanovdb_camera_config_t& config = context.camera_view->configs[i];
        pnanovdb_camera_state_t& state = context.camera_view->states[i];
        pnanovdb_camera_config_default(&config);
        pnanovdb_camera_state_default(&state, is_y_up);

        if (i >= serialized_count)
        {
            continue;
        }
        const nlohmann::json& entry = (*entries_it)[i];
        if (!entry.is_object())
        {
            continue;
        }
        if (auto sj = entry.find("state"); sj != entry.end() && sj->is_object())
        {
            if (auto v = sj->find("position"); v != sj->end())
                json_to_vec3(*v, state.position);
            if (auto v = sj->find("eye_direction"); v != sj->end())
                json_to_vec3(*v, state.eye_direction);
            if (auto v = sj->find("eye_up"); v != sj->end())
                json_to_vec3(*v, state.eye_up);
            state.eye_distance_from_position = json_float_or(*sj, "eye_distance", state.eye_distance_from_position);
            state.orthographic_scale = json_float_or(*sj, "orthographic_scale", state.orthographic_scale);
        }
        if (auto cj = entry.find("config"); cj != entry.end() && cj->is_object())
        {
            config.is_projection_rh = json_bool_or(*cj, "projection_rh", config.is_projection_rh);
            config.is_orthographic = json_bool_or(*cj, "orthographic", config.is_orthographic);
            config.is_reverse_z = json_bool_or(*cj, "reverse_z", config.is_reverse_z);
            config.near_plane = json_float_or(*cj, "near_plane", config.near_plane);
            config.far_plane = json_float_or(*cj, "far_plane", config.far_plane);
            config.fov_angle_y = json_float_or(*cj, "fov_y", config.fov_angle_y);
            config.orthographic_y = json_float_or(*cj, "orthographic_y", config.orthographic_y);
            config.aspect_ratio = json_float_or(*cj, "aspect_ratio", config.aspect_ratio);
            config.pan_rate = json_float_or(*cj, "pan_rate", config.pan_rate);
            config.tilt_rate = json_float_or(*cj, "tilt_rate", config.tilt_rate);
            config.zoom_rate = json_float_or(*cj, "zoom_rate", config.zoom_rate);
            config.key_translation_rate = json_float_or(*cj, "key_translation_rate", config.key_translation_rate);
            config.scroll_zoom_rate = json_float_or(*cj, "scroll_zoom_rate", config.scroll_zoom_rate);
        }
    }
    return context;
}

bool SceneSelection::operator==(const SceneSelection& other) const
{
    // Compare token IDs for efficiency
    uint64_t this_name_id = name_token ? name_token->id : 0;
    uint64_t other_name_id = other.name_token ? other.name_token->id : 0;
    uint64_t this_scene_id = scene_token ? scene_token->id : 0;
    uint64_t other_scene_id = other.scene_token ? other.scene_token->id : 0;

    return type == other.type && this_name_id == other_name_id && this_scene_id == other_scene_id;
}

bool SceneSelection::operator!=(const SceneSelection& other) const
{
    return !(*this == other);
}

EditorScene::EditorScene(const EditorSceneConfig& config)
    : m_imgui_instance(config.imgui_instance),
      m_editor(config.editor),
      m_scene_manager(*config.editor->impl->scene_manager),
      m_scene_view(*config.editor->impl->scene_view),
      m_compute(config.editor->impl->compute),
      m_imgui_settings(config.imgui_settings),
      m_device_queue(config.device_queue)
{
    // Setup views UI - ImguiInstance accesses views through EditorScene
    m_imgui_instance->editor_scene = this;

    // Provide render settings to SceneView so it can derive is_y_up on scene creation
    m_scene_view.set_render_settings(m_imgui_settings);

    // Sync editor's camera from the default scene's camera (picks up is_y_up setting from render settings)
    sync_editor_camera_from_scene();

    // Initialize NanoVDB shader params arrays with defaults
    {
        m_nanovdb_params.shader_name = config.default_shader_name;
        m_nanovdb_params.size = PNANOVDB_COMPUTE_CONSTANT_BUFFER_MAX_SIZE;
        m_nanovdb_params.default_array = m_scene_manager.create_initialized_shader_params(
            m_compute, config.default_shader_name, nullptr, PNANOVDB_COMPUTE_CONSTANT_BUFFER_MAX_SIZE);
    }

    // Initialize raster shader params arrays with defaults
    {
        m_raster_shader_params_data_type = PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_raster_shader_params_t);
        const char* gaussian_shader_group = pnanovdb_pipeline_get_shader_group(pnanovdb_pipeline_type_gaussian_splat);
        m_gaussian_params.shader_name = gaussian_shader_group;
        m_gaussian_params.size = m_raster_shader_params_data_type->element_size;
        m_gaussian_params.default_array = m_scene_manager.create_initialized_shader_params(
            m_compute, nullptr, gaussian_shader_group, m_raster_shader_params_data_type->element_size,
            m_raster_shader_params_data_type);
    }

    // Setup shader monitoring
    ShaderCallback callback =
        pnanovdb_editor::get_shader_recompile_callback(m_imgui_instance, m_editor->impl->compiler, config.compiler_inst);
    monitor_shader_dir(pnanovdb_shader::getShaderDir().c_str(), callback);
}

EditorScene::~EditorScene()
{
    if (m_editor && m_editor->impl && m_editor->impl->pipeline_runtime)
    {
        m_editor->impl->pipeline_runtime->quiesce();
    }

    if (m_async_load_target)
    {
        m_scene_manager.cancel_load_target(
            m_async_load_target->scene, m_async_load_target->name, m_async_load_target->lifetime_id);
        m_async_load_target.reset();
    }

    // Destroy shader params arrays (destroy_array handles nullptr safely)
    // Note: current_array might be the same as default_array, avoid double-free
    if (m_nanovdb_params.current_array && m_nanovdb_params.current_array != m_nanovdb_params.default_array)
    {
        m_compute->destroy_array(m_nanovdb_params.current_array);
    }
    m_compute->destroy_array(m_nanovdb_params.default_array);

    if (m_gaussian_params.current_array && m_gaussian_params.current_array != m_gaussian_params.default_array)
    {
        m_compute->destroy_array(m_gaussian_params.current_array);
    }
    m_compute->destroy_array(m_gaussian_params.default_array);
}

SceneObject* EditorScene::get_scene_object(pnanovdb_editor_token_t* view_name_token, ViewType view_type) const
{
    if (!view_name_token)
    {
        return nullptr;
    }

    pnanovdb_editor_token_t* scene_token = get_current_scene_token();
    return m_scene_manager.get(scene_token, view_name_token);
}

SceneObject* EditorScene::get_current_scene_object() const
{
    if (!m_view_selection.name_token)
    {
        return nullptr;
    }

    return get_scene_object(m_view_selection.name_token, m_view_selection.type);
}

SceneObject* EditorScene::get_render_scene_object() const
{
    if (!m_render_view_selection.is_valid())
    {
        return nullptr;
    }

    return get_scene_object(m_render_view_selection.name_token, m_render_view_selection.type);
}

void EditorScene::sync_current_view_state(SyncDirection sync_direction)
{
    // Copy scene object data while holding the mutex to avoid UAF if worker thread replaces map entry
    pnanovdb_pipeline_render_method_t render_method = pnanovdb_pipeline_render_method_nanovdb;
    void* obj_shader_params = nullptr;
    std::string obj_shader_name;

    m_scene_manager.for_each_object(
        [&](SceneObject* scene_obj)
        {
            if (!scene_obj)
            {
                return true;
            }

            // Check if this is the render view object
            if (!m_render_view_selection.is_valid())
            {
                return false;
            }

            pnanovdb_editor_token_t* scene_token = get_current_scene_token();
            if (scene_obj->scene_token == scene_token && scene_obj->name_token == m_render_view_selection.name_token)
            {
                // Use render pipeline to determine params type (handles Gaussian->NanoVDB conversion)
                render_method = pipeline_get_render_method(scene_obj->render_pipeline());
                obj_shader_params = scene_obj->shader_params();
                const char* shader = pipeline_get_shader(scene_obj);
                obj_shader_name = shader ? shader : "";
                return false;
            }
            return true;
        });

    // Now process with the copied data (no longer holding mutex)
    if (obj_shader_params || !obj_shader_name.empty())
    {
        // Check if shader name changed and trigger recompilation if needed
        if (!obj_shader_name.empty() && m_editor->impl->shader_name != obj_shader_name)
        {
            m_editor->impl->shader_name = obj_shader_name;
            m_imgui_instance->pending.update_shader = true;
        }

        copy_shader_params(render_method, obj_shader_params, obj_shader_name, sync_direction);
    }
}

void EditorScene::clear_editor_view_state()
{
    // Clear all view-related pointers before switching to a new view
    m_editor->impl->nanovdb_array = nullptr;
    m_editor->impl->gaussian_data = nullptr;
    m_editor->impl->shader_params = nullptr;
    m_editor->impl->shader_params_data_type = nullptr;
}

void EditorScene::copy_editor_shader_params_to_ui(SceneShaderParams* params)
{
    if (!params || !m_editor->impl->shader_params)
    {
        return;
    }

    params->current_array = m_compute->create_array(params->size, 1u, m_editor->impl->shader_params);
    m_scene_manager.shader_params.set_compute_array_for_shader(params->shader_name, params->current_array);
}

void EditorScene::copy_shader_params_from_ui_to_view(SceneShaderParams* params, void* view_params)
{
    if (!params || !view_params || params->size == 0)
    {
        return;
    }

    m_scene_manager.shader_params.copy_params_to_buffer(params->shader_name, view_params, params->size);
}

void EditorScene::copy_ui_shader_params_from_to_editor(SceneShaderParams* params)
{
    if (params && params->current_array)
    {
        std::memcpy(m_editor->impl->shader_params, params->current_array->data, params->size);
    }
}

void EditorScene::copy_shader_params(pnanovdb_pipeline_render_method_t render_method,
                                     void* obj_shader_params,
                                     const std::string& obj_shader_name,
                                     SyncDirection sync_direction,
                                     void** view_params_out)
{
    SceneShaderParams* params = nullptr;
    void* view_params = nullptr;

    // Use render_method to determine which params struct to use (gaussian vs nanovdb)
    if (render_method == pnanovdb_pipeline_render_method_gaussian)
    {
        params = &m_gaussian_params;
        view_params = get_view_params_with_fallback(m_gaussian_params, obj_shader_params);
    }
    else if (render_method == pnanovdb_pipeline_render_method_nanovdb)
    {
        params = &m_nanovdb_params;
        view_params = get_view_params_with_fallback(m_nanovdb_params, obj_shader_params);
    }

    if (params && view_params)
    {
        params->shader_name = obj_shader_name;

        if (sync_direction == SyncDirection::UIToEditor)
        {
            copy_ui_shader_params_from_to_editor(params);
        }
        else if (sync_direction == SyncDirection::EditorToUI)
        {
            copy_editor_shader_params_to_ui(params);
        }
        else if (sync_direction == SyncDirection::UiToView)
        {
            copy_shader_params_from_ui_to_view(params, view_params);
            if (view_params_out)
            {
                *view_params_out = view_params;
            }
        }
    }
}

void EditorScene::load_view_into_editor_and_ui(SceneObject* scene_obj)
{
    if (!scene_obj)
    {
        return;
    }

    clear_editor_view_state();

    // Load selected view's pointers into the editor state so rendering uses the active selection
    auto render_method = pipeline_get_render_method(scene_obj->render_pipeline());
    void* obj_shader_params = scene_obj->shader_params();
    const char* shader = pipeline_get_shader(scene_obj);
    std::string obj_shader_name = shader ? shader : "";

    if (render_method == pnanovdb_pipeline_render_method_nanovdb)
    {
        // NanoVDB rendering - use nanovdb_array or converted_nanovdb (Raster3D stores in both; fallback for robustness)
        pnanovdb_compute_array_t* array =
            scene_obj->nanovdb_array() ? scene_obj->nanovdb_array() : scene_obj->converted_nanovdb();
        m_editor->impl->nanovdb_array = array;
        m_editor->impl->shader_params = obj_shader_params;
        m_editor->impl->shader_params_data_type = nullptr;
    }
    else if (render_method == pnanovdb_pipeline_render_method_gaussian)
    {
        // Gaussian 2D splatting - load gaussian_data
        m_editor->impl->gaussian_data = scene_obj->gaussian_data();
        m_editor->impl->shader_params = obj_shader_params;
        m_editor->impl->shader_params_data_type = m_raster_shader_params_data_type;
    }

    // Update the shader name if it differs from current, and trigger shader recompilation
    if (!obj_shader_name.empty() && m_editor->impl->shader_name != obj_shader_name)
    {
        m_editor->impl->shader_name = obj_shader_name;
        m_imgui_instance->pending.update_shader = true;
    }

    // Use render method for shader param copying (determines param struct layout)
    copy_shader_params(render_method, obj_shader_params, obj_shader_name, SyncDirection::EditorToUI);

    // Note: Camera sync is NOT done here - camera is per-scene, not per-view
    // Camera syncing happens when:
    //   1. At startup (EditorScene constructor)
    //   2. When switching scenes (set_current_scene)
    //   3. When user moves viewport (sync_scene_camera_from_editor in render loop)
}

bool EditorScene::handle_pending_view_changes()
{
    // Handle UI-triggered view changes (from scene tree)
    std::string pending_view_name;
    const char* current_view_name =
        (m_view_selection.name_token && m_view_selection.name_token->str) ? m_view_selection.name_token->str : "";

    if (!m_imgui_instance->pending.viewport_gaussian_view.empty() &&
        m_imgui_instance->pending.viewport_gaussian_view != current_view_name)
    {
        pending_view_name = m_imgui_instance->pending.viewport_gaussian_view;
        m_imgui_instance->pending.viewport_gaussian_view.clear();
    }
    else if (!m_imgui_instance->pending.viewport_nanovdb_array.empty() &&
             m_imgui_instance->pending.viewport_nanovdb_array != current_view_name)
    {
        pending_view_name = m_imgui_instance->pending.viewport_nanovdb_array;
        m_imgui_instance->pending.viewport_nanovdb_array.clear();
    }

    if (!pending_view_name.empty())
    {
        pnanovdb_editor_token_t* view_token = EditorToken::getInstance().getToken(pending_view_name.c_str());
        m_scene_view.set_current_view(view_token);
        return true;
    }
    return false;
}

void EditorScene::process_pending_editor_changes()
{
    auto* worker = m_editor->impl->editor_worker;
    if (!worker)
    {
        return;
    }

    bool updated = false;

    // Process pending NanoVDB array
    pnanovdb_compute_array_t* old_nanovdb_array = nullptr;
    updated = worker->pending_nanovdb.process_pending(m_editor->impl->nanovdb_array, old_nanovdb_array);

    // Process pending data array
    pnanovdb_compute_array_t* old_array = nullptr;
    worker->pending_data_array.process_pending(m_editor->impl->data_array, old_array);

    // Process pending Gaussian data
    pnanovdb_raster_gaussian_data_t* old_gaussian_data = nullptr;
    worker->pending_gaussian_data.process_pending(m_editor->impl->gaussian_data, old_gaussian_data);

    pnanovdb_camera_t* old_camera = nullptr;
    updated = worker->pending_camera.process_pending(m_editor->impl->camera, old_camera);
    if (updated)
    {
        if (old_camera)
        {
            delete old_camera;
        }

        m_imgui_settings->camera_state = m_editor->impl->camera->state;
        m_imgui_settings->camera_config = m_editor->impl->camera->config;
        m_imgui_settings->sync_camera = PNANOVDB_TRUE;
    }

    // Process pending camera views
    // Note: Camera views are added to scene manager in add_camera_view_2(), we just update pointers here
    for (uint32_t idx = 0u; idx < 32u; idx++)
    {
        pnanovdb_camera_view_t* old_camera_view = nullptr;
        worker->pending_camera_view[idx].process_pending(m_editor->impl->camera_view, old_camera_view);
    }

    // Process pending shader params
    // Note: Shader params are set in scene objects, we just update the global pointer for legacy renderer access
    {
        std::lock_guard<std::recursive_mutex> lock(m_editor->impl->editor_worker->shader_params_mutex);
        void* old_shader_params = nullptr;
        worker->pending_shader_params.process_pending(m_editor->impl->shader_params, old_shader_params);

        const pnanovdb_reflect_data_type_t* old_shader_params_data_type = nullptr;
        worker->pending_shader_params_data_type.process_pending(
            m_editor->impl->shader_params_data_type, old_shader_params_data_type);
    }


    // Sync views from scene_manager if signaled (worker thread modified scene_manager)
    if (worker->views_need_sync.exchange(false, std::memory_order_acq_rel))
    {
        const uint64_t selected_scene_id = worker->last_added_scene_token_id.exchange(0, std::memory_order_relaxed);
        const uint64_t selected_name_id = worker->last_added_name_token_id.exchange(0, std::memory_order_relaxed);
        sync_views_from_scene_manager(selected_scene_id, selected_name_id);
    }
}

void EditorScene::process_pending_ui_changes()
{
    // TODO: remove those
    if (m_imgui_instance->pending.print_slice)
    {
        m_imgui_instance->pending.print_slice = false;
#if TEST_IO
        FILE* input_file = fopen("./data/smoke.nvdb", "rb");
        FILE* output_file = fopen("./data/slice_output.bmp", "wb");
        test_pnanovdb_io_print_slice(input_file, output_file);
        fclose(input_file);
        fclose(output_file);
#endif
    }
}

ViewType EditorScene::determine_view_type(pnanovdb_editor_token_t* view_token, pnanovdb_editor_token_t* scene_token) const
{
    if (!view_token)
    {
        return ViewType::None;
    }

    if (is_selection_valid(SceneSelection{ ViewType::NanoVDBs, view_token, scene_token }))
    {
        return ViewType::NanoVDBs;
    }
    if (is_selection_valid(SceneSelection{ ViewType::GaussianScenes, view_token, scene_token }))
    {
        return ViewType::GaussianScenes;
    }

    return ViewType::None;
}

void EditorScene::sync_selected_view_with_current()
{
    pnanovdb_editor_token_t* prev_view_token = m_scene_view.get_current_view();
    const uint64_t current_epoch = get_current_view_epoch();
    bool has_pending_request = handle_pending_view_changes();

    pnanovdb_editor_token_t* view_token = m_scene_view.get_current_view();

    // If a camera is selected and no explicit request was made, keep camera selection ONLY
    // when the content view hasn't changed (by token ID and by epoch) this frame. This ensures
    // views created earlier (via API/worker) can still grab selection.
    uint64_t prev_id = prev_view_token ? prev_view_token->id : 0;
    uint64_t curr_id = view_token ? view_token->id : 0;
    if (!has_pending_request && m_view_selection.type == ViewType::Cameras && prev_id == curr_id &&
        m_last_synced_epoch == current_epoch)
    {
        return;
    }

    // Check if we have a valid view token
    if (!view_token)
    {
        // No views in scene - clear the display
        clear_selection();
        m_last_synced_epoch = current_epoch;
        return;
    }

    // Get current scene token for validation
    pnanovdb_editor_token_t* current_scene_token = get_current_scene_token();

    // Determine view type using helper function
    ViewType new_view_type = determine_view_type(view_token, current_scene_token);

    // Create the expected new selection to compare against
    SceneSelection new_selection{ new_view_type, view_token, current_scene_token };

    // Check if both properties selection AND render view are already set to this exact selection
    // (including scene_token and type) AND the epoch hasn't changed. Only skip update if both
    // match completely and content hasn't changed (epoch check ensures new content triggers update).
    if (m_view_selection == new_selection && m_render_view_selection == new_selection &&
        m_last_synced_epoch == current_epoch)
    {
        return;
    }

    Console::getInstance().addLog(Console::LogLevel::Debug, "sync_selected_view_with_current: Updating selection");
    set_properties_selection(new_view_type, view_token, current_scene_token);
    set_render_view(new_view_type, view_token, current_scene_token);
    m_last_synced_epoch = current_epoch;
}

void EditorScene::sync_shader_params_from_editor()
{
    if (m_editor->impl->editor_worker)
    {
        std::lock_guard<std::recursive_mutex> lock(m_editor->impl->editor_worker->shader_params_mutex);

        if (m_editor->impl->editor_worker->params_dirty.exchange(false))
        {
            // Sync editor params to UI
            sync_current_view_state(SyncDirection::EditorToUI);
        }

        sync_current_view_state(SyncDirection::UiToView);
    }
    else
    {
        sync_current_view_state(SyncDirection::UiToView);

        // Clear editor params pointer to mark UI as source of truth
        m_editor->impl->shader_params = nullptr;
        m_editor->impl->shader_params_data_type = nullptr;
    }
}

void EditorScene::sync_views_from_scene_manager(uint64_t selected_scene_id, uint64_t selected_name_id)
{
    // This function syncs SceneView from EditorSceneManager state
    // Called from the render thread after draining editor commands.
    // Ensures thread-safe updates by only touching views from render thread

    pnanovdb_editor_token_t* last_added_scene_token = nullptr;
    pnanovdb_editor_token_t* last_added_name_token = nullptr;
    if (selected_scene_id != 0 && selected_name_id != 0)
    {
        last_added_scene_token = EditorToken::getInstance().getTokenById(selected_scene_id);
        last_added_name_token = EditorToken::getInstance().getTokenById(selected_name_id);
    }

    // Iterate over all objects while holding the scene_manager mutex
    // This prevents race conditions where objects might be removed by worker thread during iteration
    m_scene_manager.for_each_object(
        [this](SceneObject* obj)
        {
            if (!obj || !obj->scene_token || !obj->name_token || !obj->name_token->str)
            {
                return true;
            }

            if (obj->type == SceneObjectType::Camera)
            {
                if (obj->camera_view() && obj->resources.camera_view_owner)
                {
                    m_scene_view.sync_camera_owner(obj->scene_token, obj->name_token, obj->resources.camera_view_owner);
                }
            }
            else
            {
                auto render_method = pipeline_get_render_method(obj->render_pipeline());
                bool has_nanovdb = (obj->nanovdb_array() && obj->resources.nanovdb_array_owner) ||
                                   (obj->converted_nanovdb() && obj->resources.converted_nanovdb_owner);
                if (render_method == pnanovdb_pipeline_render_method_nanovdb && has_nanovdb)
                {
                    const auto& owner = obj->resources.nanovdb_array_owner ? obj->resources.nanovdb_array_owner :
                                                                             obj->resources.converted_nanovdb_owner;
                    NanoVDBContext ctx{ owner, obj->shader_params(), obj->params.shader_params_array_owner };
                    m_scene_view.add_nanovdb(obj->scene_token, obj->name_token, ctx);
                }
                else if (render_method == pnanovdb_pipeline_render_method_gaussian && obj->gaussian_data() &&
                         obj->resources.gaussian_data_owner)
                {
                    GaussianDataContext ctx{ obj->resources.gaussian_data_owner,
                                             (pnanovdb_raster_shader_params_t*)obj->shader_params() };
                    m_scene_view.add_gaussian(obj->scene_token, obj->name_token, ctx);
                }
            }

            return true;
        });

    // After syncing, if worker specified a view to select, do so
    if (last_added_scene_token && last_added_name_token)
    {
        pnanovdb_editor_token_t* old_scene = m_scene_view.get_current_scene_token();
        m_scene_view.set_current_scene(last_added_scene_token);

        // Sync camera if we switched scenes
        if (is_switching_scenes(old_scene, last_added_scene_token))
        {
            sync_editor_camera_from_scene();
            apply_editor_camera_to_viewport();
        }

        // Determine what view to select - prefer content views over cameras
        pnanovdb_editor_token_t* view_to_select = last_added_name_token;
        ViewType view_type = determine_view_type(view_to_select, last_added_scene_token);

        // If worker token is a camera (not a content view), try scene's last_added_view_token_id
        if (view_type == ViewType::None)
        {
            SceneViewData* scene = m_scene_view.get_or_create_scene(last_added_scene_token);
            if (scene && scene->last_added_view_token_id != 0)
            {
                view_to_select = EditorToken::getInstance().getTokenById(scene->last_added_view_token_id);
                if (view_to_select)
                {
                    // Verify the view actually exists in the scene
                    view_type = determine_view_type(view_to_select, last_added_scene_token);

                    // If last_added view doesn't exist anymore, find any available view
                    if (view_type == ViewType::None)
                    {
                        view_to_select = find_any_view_in_scene(last_added_scene_token);
                        if (view_to_select)
                        {
                            view_type = determine_view_type(view_to_select, last_added_scene_token);
                        }
                    }
                }
            }
            else
            {
                // No last_added tracked, find any available view
                view_to_select = find_any_view_in_scene(last_added_scene_token);
                if (view_to_select)
                {
                    view_type = determine_view_type(view_to_select, last_added_scene_token);
                }
            }
        }

        // Set the view and update selection
        if (view_to_select && view_type != ViewType::None)
        {
            m_scene_view.set_current_view(last_added_scene_token, view_to_select);
            set_properties_selection(view_type, view_to_select, last_added_scene_token);
            set_render_view(view_type, view_to_select, last_added_scene_token);
        }
    }
}

void EditorScene::reload_shader_params_for_current_view()
{
    pnanovdb_pipeline_render_method_t render_method = pnanovdb_pipeline_render_method_none;
    if (m_render_view_selection.is_valid())
    {
        pnanovdb_editor_token_t* scene_token = get_current_scene_token();
        m_scene_manager.with_object(scene_token, m_render_view_selection.name_token,
                                    [&](SceneObject* scene_obj)
                                    {
                                        if (scene_obj)
                                            render_method = pipeline_get_render_method(scene_obj->render_pipeline());
                                    });
    }

    if (render_method == pnanovdb_pipeline_render_method_none)
    {
        if (m_render_view_selection.type == ViewType::GaussianScenes)
        {
            render_method = pnanovdb_pipeline_render_method_gaussian;
        }
        else if (m_render_view_selection.type == ViewType::NanoVDBs)
        {
            render_method = pnanovdb_pipeline_render_method_nanovdb;
        }
    }

    reload_shader_params_for_current_view(render_method);
}

void EditorScene::reload_shader_params_for_current_view(pnanovdb_pipeline_render_method_t render_method)
{
    auto destroy_default_array = [&](SceneShaderParams& params)
    {
        if (params.default_array)
        {
            m_compute->destroy_array(params.default_array);
            params.default_array = nullptr;
        }
    };

    if (render_method == pnanovdb_pipeline_render_method_gaussian)
    {
        destroy_default_array(m_gaussian_params);
        m_gaussian_params.default_array = m_scene_manager.create_initialized_shader_params(
            m_compute, nullptr, pnanovdb_pipeline_get_shader_group(pnanovdb_pipeline_type_gaussian_splat),
            m_gaussian_params.size, m_raster_shader_params_data_type);
    }
    else if (render_method == pnanovdb_pipeline_render_method_nanovdb)
    {
        destroy_default_array(m_nanovdb_params);
        // Use the already-synced editor shader name to avoid nested locks
        m_nanovdb_params.shader_name = m_editor->impl->shader_name;
        m_nanovdb_params.default_array = m_scene_manager.create_initialized_shader_params(
            m_compute, m_nanovdb_params.shader_name.c_str(), nullptr, PNANOVDB_COMPUTE_CONSTANT_BUFFER_MAX_SIZE);
    }
}

void EditorScene::sync_editor_camera_from_scene()
{
    if (!m_editor || !m_editor->impl)
    {
        return;
    }

    // Get the viewport camera for the current scene
    pnanovdb_editor_token_t* current_scene = get_current_scene_token();
    if (!current_scene)
    {
        return;
    }

    // Ensure the scene exists (creates with default camera if needed)
    m_scene_view.get_or_create_scene(current_scene);

    pnanovdb_editor_token_t* viewport_token = m_scene_view.get_viewport_camera_token(current_scene);
    pnanovdb_camera_view_t* viewport_view = m_scene_view.get_camera(current_scene, viewport_token);
    if (viewport_view && viewport_view->num_cameras > 0)
    {
        // Verify that configs and states pointers are still valid
        // These pointers may point to map entries that can be invalidated on rehashing
        if (!viewport_view->configs || !viewport_view->states)
        {
            return;
        }

        // Update the global camera from the viewport camera
        // Copy the values directly instead of accessing through potentially-invalid pointers
        // This ensures we get the actual camera values regardless of map rehashing
        m_editor->impl->camera->config = viewport_view->configs[0];
        m_editor->impl->camera->state = viewport_view->states[0];
    }
}

void EditorScene::sync_scene_camera_from_editor()
{
    if (!m_editor || !m_editor->impl || !m_editor->impl->camera)
    {
        return;
    }

    // Get the viewport camera for the current scene
    pnanovdb_editor_token_t* current_scene = get_current_scene_token();
    if (!current_scene)
    {
        return;
    }

    // Ensure the scene exists (creates with default camera if needed)
    m_scene_view.get_or_create_scene(current_scene);

    pnanovdb_editor_token_t* viewport_token = m_scene_view.get_viewport_camera_token(current_scene);
    pnanovdb_camera_view_t* viewport_view = m_scene_view.get_camera(current_scene, viewport_token);
    if (viewport_view && viewport_view->num_cameras > 0)
    {
        // Verify that configs and states pointers are still valid
        if (!viewport_view->configs || !viewport_view->states)
        {
            return;
        }

        // Update the scene's viewport camera from the global camera
        // This syncs user camera movements back to the scene for properties display
        viewport_view->configs[0] = m_editor->impl->camera->config;
        viewport_view->states[0] = m_editor->impl->camera->state;
    }
}

void EditorScene::get_shader_params_for_current_view(void* shader_params_data)
{
    if (!shader_params_data || !m_render_view_selection.is_valid())
    {
        return;
    }

    pnanovdb_editor_token_t* scene_token = get_current_scene_token();
    m_scene_manager.with_object(
        scene_token, m_render_view_selection.name_token,
        [&](SceneObject* scene_obj)
        {
            if (!scene_obj)
            {
                return;
            }

            auto render_method = pipeline_get_render_method(scene_obj->render_pipeline());
            void* obj_shader_params = scene_obj->shader_params();
            const char* shader = pipeline_get_shader(scene_obj);
            std::string obj_shader_name = shader ? shader : "";

            void* view_params = nullptr;
            size_t copy_size = (render_method == pnanovdb_pipeline_render_method_gaussian) ? m_gaussian_params.size :
                                                                                             m_nanovdb_params.size;
            copy_shader_params(render_method, obj_shader_params, obj_shader_name, SyncDirection::UiToView, &view_params);

            if (view_params && copy_size > 0)
            {
                std::memcpy(shader_params_data, view_params, copy_size);
            }
        });
}

void snapshot_object_shader_params_readonly(EditorSceneManager& scene_manager,
                                            pnanovdb_editor_token_t* scene_token,
                                            pnanovdb_editor_token_t* name_token,
                                            size_t gaussian_params_size,
                                            size_t nanovdb_params_size,
                                            const void* gaussian_default_data,
                                            const void* nanovdb_default_data,
                                            void* shader_params_data)
{
    if (!shader_params_data || !scene_token || !name_token)
    {
        return;
    }

    scene_manager.with_object(scene_token, name_token,
                              [&](SceneObject* scene_obj)
                              {
                                  if (!scene_obj)
                                  {
                                      return;
                                  }

                                  auto render_method = pipeline_get_render_method(scene_obj->render_pipeline());
                                  const bool is_gaussian = (render_method == pnanovdb_pipeline_render_method_gaussian);
                                  const size_t copy_size = is_gaussian ? gaussian_params_size : nanovdb_params_size;
                                  if (copy_size == 0)
                                  {
                                      return;
                                  }

                                  // Per-object buffer takes priority
                                  const void* src = scene_obj->shader_params();
                                  if (!src)
                                  {
                                      src = is_gaussian ? gaussian_default_data : nanovdb_default_data;
                                  }
                                  if (!src)
                                  {
                                      return;
                                  }
                                  std::memcpy(shader_params_data, src, copy_size);
                              });
}

void EditorScene::get_shader_params_for_object(pnanovdb_editor_token_t* scene_token,
                                               pnanovdb_editor_token_t* name_token,
                                               void* shader_params_data)
{
    const void* gaussian_default = (m_gaussian_params.default_array && m_gaussian_params.default_array->data) ?
                                       m_gaussian_params.default_array->data :
                                       nullptr;
    const void* nanovdb_default = (m_nanovdb_params.default_array && m_nanovdb_params.default_array->data) ?
                                      m_nanovdb_params.default_array->data :
                                      nullptr;
    snapshot_object_shader_params_readonly(m_scene_manager, scene_token, name_token, m_gaussian_params.size,
                                           m_nanovdb_params.size, gaussian_default, nanovdb_default, shader_params_data);
}

void EditorScene::update_scene_tree_after_conversion(pnanovdb_editor_token_t* scene_token,
                                                     pnanovdb_editor_token_t* name_token)
{
    if (!scene_token || !name_token)
    {
        return;
    }

    bool entry_replaced = false;
    bool need_params_init = false;
    m_scene_manager.with_object(
        scene_token, name_token,
        [&](SceneObject* obj)
        {
            if (!obj)
            {
                Console::getInstance().addLog(Console::LogLevel::Warning, "update_scene_tree: object not found for '%s'",
                                              name_token->str ? name_token->str : "?");
                return;
            }

            const auto& owner = obj->resources.nanovdb_array_owner ? obj->resources.nanovdb_array_owner :
                                                                     obj->resources.converted_nanovdb_owner;
            if (!owner)
            {
                Console::getInstance().addLog(
                    Console::LogLevel::Warning,
                    "update_scene_tree: no nanovdb owner for '%s' (nanovdb_array=%p, converted=%p)",
                    name_token->str ? name_token->str : "?", (void*)obj->nanovdb_array(),
                    (void*)obj->converted_nanovdb());
                return;
            }

            need_params_init = (obj->shader_params() == nullptr);

            // Remove old entry (placeholder or Gaussian) and replace with real NanoVDB
            m_scene_view.remove(scene_token, name_token);
            NanoVDBContext ctx{ owner, obj->shader_params(), obj->params.shader_params_array_owner };
            m_scene_view.add_nanovdb(scene_token, name_token, ctx);
            entry_replaced = true;

            Console::getInstance().addLog(Console::LogLevel::Debug,
                                          "update_scene_tree: replaced entry for '%s' (array=%p)",
                                          name_token->str ? name_token->str : "?", (void*)owner.get());
        });

    // Converted objects have null shader_params; give them a copy of default NanoVDB params so they display
    if (entry_replaced && need_params_init && m_nanovdb_params.default_array && m_nanovdb_params.default_array->data)
    {
        pnanovdb_compute_array_t* params_copy = m_compute->create_array(m_nanovdb_params.default_array->element_size,
                                                                        m_nanovdb_params.default_array->element_count,
                                                                        m_nanovdb_params.default_array->data);
        if (params_copy)
        {
            m_scene_manager.set_params_array(scene_token, name_token, params_copy, m_compute);
            // Refresh scene view context so it points at the new params
            m_scene_manager.with_object(
                scene_token, name_token,
                [&](SceneObject* obj)
                {
                    if (obj && obj->shader_params())
                    {
                        const auto& owner = obj->resources.nanovdb_array_owner ? obj->resources.nanovdb_array_owner :
                                                                                 obj->resources.converted_nanovdb_owner;
                        if (owner)
                        {
                            m_scene_view.remove(scene_token, name_token);
                            NanoVDBContext ctx{ owner, obj->shader_params(), obj->params.shader_params_array_owner };
                            m_scene_view.add_nanovdb(scene_token, name_token, ctx);
                        }
                    }
                });
        }
    }

    // Update the render view selection to NanoVDB type so rendering picks it up correctly
    if (entry_replaced && m_render_view_selection.is_valid() && m_render_view_selection.name_token == name_token)
    {
        set_render_view(ViewType::NanoVDBs, name_token, scene_token);
    }

    // Only update global editor pointers if this object IS the active render view.
    if (entry_replaced && m_render_view_selection.is_valid() && m_render_view_selection.name_token == name_token)
    {
        m_scene_manager.with_object(scene_token, name_token,
                                    [&](SceneObject* obj)
                                    {
                                        if (obj)
                                            load_view_into_editor_and_ui(obj);
                                    });
    }
}

void EditorScene::add_nanovdb_placeholder(pnanovdb_editor_token_t* scene_token, pnanovdb_editor_token_t* name_token)
{
    if (!scene_token || !name_token)
    {
        return;
    }

    NanoVDBContext placeholder;
    m_scene_view.add_nanovdb(scene_token, name_token, placeholder);
}

void EditorScene::create_empty_object(pnanovdb_editor_token_t* scene_token,
                                      pnanovdb_editor_token_t* name_token,
                                      const std::string& object_type)
{
    restore_empty_scene_object(m_scene_manager, m_scene_view, scene_token, name_token, object_type);
}

pnanovdb_editor_token_t* EditorScene::add_empty_object(pnanovdb_editor_token_t* scene_token)
{
    if (!scene_token)
    {
        scene_token = get_current_scene_token();
    }
    if (!scene_token)
    {
        return nullptr;
    }

    // Pick the first unused "Object N" name in this scene.
    pnanovdb_editor_token_t* name_token = nullptr;
    for (int i = 1; i < 100000 && !name_token; ++i)
    {
        const std::string candidate = "Object " + std::to_string(i);
        pnanovdb_editor_token_t* tok = EditorToken::getInstance().getToken(candidate.c_str());
        bool exists = false;
        m_scene_manager.with_object(scene_token, tok, [&](SceneObject* obj) { exists = (obj != nullptr); });
        if (!exists)
        {
            name_token = tok;
        }
    }
    if (!name_token)
    {
        return nullptr;
    }

    create_empty_object(scene_token, name_token, "array");
    select_render_view(scene_token, name_token);
    set_properties_selection(ViewType::NanoVDBs, name_token, scene_token);

    return name_token;
}

bool EditorScene::handle_nanovdb_data_load(pnanovdb_editor_token_t* scene,
                                           pnanovdb_compute_array_t* nanovdb_array,
                                           const char* filename,
                                           pnanovdb_pipeline_type_t render_pipeline,
                                           pnanovdb_editor_token_t* name,
                                           uint64_t reserved_lifetime_id)
{
    if (!scene || !filename)
    {
        return false;
    }

    // Store in SceneManager as the single source of truth
    pnanovdb_editor_token_t* scene_token = scene;
    pnanovdb_editor_token_t* name_token = resolve_import_name(name, filename);

    // Do not create per-object params now; use shared defaults until user edits (copy-on-write)
    pnanovdb_compute_array_t* params_array = nullptr;

    const char* pipeline_shader = pnanovdb_pipeline_get_shader_name(render_pipeline);
    const char* shader_name =
        (pipeline_shader && pipeline_shader[0] != '\0') ? pipeline_shader : m_nanovdb_params.shader_name.c_str();
    pnanovdb_editor_token_t* shader_name_token = EditorToken::getInstance().getToken(shader_name);
    std::shared_ptr<pnanovdb_raster_gaussian_data_t> old_gaussian_owner;

    if (reserved_lifetime_id)
    {
        if (!m_scene_manager.commit_reserved_nanovdb(scene_token, name_token, reserved_lifetime_id, nanovdb_array,
                                                     params_array, m_compute, shader_name_token,
                                                     pnanovdb_pipeline_type_noop, render_pipeline, &old_gaussian_owner))
            return false;
        record_async_load_publish(scene_token, name_token, reserved_lifetime_id);
    }
    else
    {
        if (!m_scene_manager.add_nanovdb(scene_token, name_token, nanovdb_array, params_array, m_compute,
                                         shader_name_token, pnanovdb_pipeline_type_noop, render_pipeline,
                                         &old_gaussian_owner))
            return false;
    }
    if (old_gaussian_owner)
        m_editor->impl->gaussian_data_destruction_queue_pending.push_back(std::move(old_gaussian_owner));

    m_scene_manager.with_object(scene_token, name_token,
                                [filename](SceneObject* obj)
                                {
                                    if (obj)
                                        obj->resources.source_filepath = filename;
                                });

    // Register in SceneView (for scene tree display)
    m_scene_view.add_nanovdb_to_scene(
        scene_token, name_token, nanovdb_array, params_array ? params_array->data : nullptr);
    return true;
}

bool EditorScene::handle_mesh_data_load(pnanovdb_editor_token_t* scene,
                                        pnanovdb_compute_array_t* indices,
                                        pnanovdb_compute_array_t* positions,
                                        pnanovdb_compute_array_t* colors,
                                        const char* filename,
                                        pnanovdb_pipeline_type_t render_pipeline,
                                        bool is_line_indices,
                                        float inflation_radius,
                                        pnanovdb_uint32_t resolution,
                                        pnanovdb_pipeline_type_t process_pipeline,
                                        pnanovdb_editor_token_t* name,
                                        uint64_t reserved_lifetime_id)
{
    if (!scene || !filename || !indices || !positions || !colors)
    {
        return false;
    }

    pnanovdb_editor_token_t* scene_token = scene;
    pnanovdb_editor_token_t* name_token = resolve_import_name(name, filename);
    std::shared_ptr<pnanovdb_raster_gaussian_data_t> old_gaussian_owner;

    if (reserved_lifetime_id)
    {
        if (!m_scene_manager.commit_reserved_mesh(scene_token, name_token, reserved_lifetime_id, indices, positions,
                                                  colors, m_compute, process_pipeline, render_pipeline,
                                                  &old_gaussian_owner))
            return false;
        record_async_load_publish(scene_token, name_token, reserved_lifetime_id);
    }
    else
    {
        if (!m_scene_manager.add_mesh(scene_token, name_token, indices, positions, colors, m_compute, process_pipeline,
                                      render_pipeline, &old_gaussian_owner))
            return false;
    }
    if (old_gaussian_owner)
        m_editor->impl->gaussian_data_destruction_queue_pending.push_back(std::move(old_gaussian_owner));

    const bool builds_voxelbvh = (process_pipeline == pnanovdb_pipeline_type_voxelbvh_build);
    const pnanovdb_pipeline_voxelbvh_source_t source_type =
        is_line_indices ? pnanovdb_pipeline_voxelbvh_source_lines : pnanovdb_pipeline_voxelbvh_source_triangles;

    std::string filepath_copy = filename;
    m_scene_manager.with_object(
        scene_token, name_token,
        [filepath_copy, source_type, inflation_radius, resolution, builds_voxelbvh](SceneObject* obj)
        {
            if (!obj)
                return;
            obj->resources.source_filepath = filepath_copy;
            obj->load_pipeline() = pnanovdb_pipeline_type_mesh_load;
            if (builds_voxelbvh)
            {
                auto& process_params = obj->process_params();
                pnanovdb_pipeline_voxelbvh_build_params_set_source_type(&process_params, source_type);
                pnanovdb_pipeline_voxelbvh_build_params_set_inflation_radius(&process_params, inflation_radius);
                pnanovdb_pipeline_voxelbvh_build_params_set_resolution(&process_params, resolution);
            }
            obj->mark_process_dirty();
        });

    add_nanovdb_placeholder(scene_token, name_token);
    select_render_view(scene_token, name_token);
    return true;
}

bool EditorScene::handle_gaussian_data_load(pnanovdb_editor_token_t* scene,
                                            pnanovdb_raster_gaussian_data_t* gaussian_data,
                                            pnanovdb_raster_shader_params_t* raster_params,
                                            const char* filename,
                                            pnanovdb_pipeline_type_t process_pipeline,
                                            pnanovdb_pipeline_type_t render_pipeline,
                                            const pnanovdb_pipeline_params_t* process_params,
                                            pnanovdb_editor_token_t* name,
                                            uint64_t reserved_lifetime_id)
{
    if (!scene || !filename)
    {
        return false;
    }

    // Use the scene token captured when the load was initiated (not current scene,
    // which may have changed if the user switched scenes during async rasterization)
    pnanovdb_editor_token_t* scene_token = scene;
    pnanovdb_editor_token_t* name_token = resolve_import_name(name, filename);

    // Do not create per-object params now; use shared defaults until user edits (copy-on-write)
    pnanovdb_compute_array_t* params_array = nullptr;

    // Add with explicit pipeline configuration
    std::shared_ptr<pnanovdb_raster_gaussian_data_t> old_owner;
    const char* shader_name = pnanovdb_pipeline_get_shader_name(pnanovdb_pipeline_type_gaussian_splat);
    if (reserved_lifetime_id)
    {
        if (!m_scene_manager.commit_reserved_gaussian_data(scene_token, name_token, reserved_lifetime_id, gaussian_data,
                                                           params_array, m_raster_shader_params_data_type, m_compute,
                                                           m_editor->impl->raster, m_device_queue, shader_name,
                                                           process_pipeline, render_pipeline, &old_owner))
            return false;
        record_async_load_publish(scene_token, name_token, reserved_lifetime_id);
    }
    else
    {
        if (!m_scene_manager.add_gaussian_data(
                scene_token, name_token, gaussian_data, params_array, m_raster_shader_params_data_type, m_compute,
                m_editor->impl->raster, m_device_queue, shader_name, process_pipeline, render_pipeline, &old_owner))
            return false;
    }

    // Store source filepath and copy process params for re-conversion
    m_scene_manager.with_object(scene_token, name_token,
                                [filename, process_params](SceneObject* obj)
                                {
                                    if (!obj)
                                        return;
                                    obj->resources.source_filepath = filename;
                                    obj->load_pipeline() = pnanovdb_pipeline_type_gaussian_load;
                                    if (process_params && process_params->data && process_params->size > 0)
                                    {
                                        void* copy = malloc(process_params->size);
                                        if (copy)
                                        {
                                            memcpy(copy, process_params->data, process_params->size);
                                            void* old_data = obj->process_params().data;
                                            if (old_data)
                                            {
                                                free(old_data);
                                            }
                                            obj->process_params().data = copy;
                                            obj->process_params().size = process_params->size;
                                            obj->process_params().type = process_params->type;
                                        }
                                    }
                                });

    if (old_owner)
        m_editor->impl->gaussian_data_destruction_queue_pending.push_back(std::move(old_owner));

    // Register in SceneView (for scene tree display)
    m_scene_view.add_gaussian_to_scene(
        scene_token, name_token, gaussian_data,
        static_cast<pnanovdb_raster_shader_params_t*>(params_array ? params_array->data : nullptr));

    m_scene_manager.with_object(scene_token, name_token,
                                [&](SceneObject* scene_obj)
                                {
                                    if (scene_obj && scene_obj->type == SceneObjectType::GaussianData)
                                    {
                                        load_view_into_editor_and_ui(scene_obj);
                                    }
                                });
    return true;
}

bool EditorScene::remove_object(pnanovdb_editor_token_t* scene_token, const char* name)
{
    if (!name)
    {
        return false;
    }

    bool removed = false;
    std::string name_str(name);
    pnanovdb_editor_token_t* name_token = EditorToken::getInstance().getToken(name);

    if (m_imgui_instance->source_scene_token && m_imgui_instance->source_name_token && scene_token &&
        m_imgui_instance->source_scene_token->id == scene_token->id &&
        m_imgui_instance->source_name_token->id == name_token->id)
    {
        m_imgui_instance->source_scene_token = nullptr;
        m_imgui_instance->source_name_token = nullptr;
    }

    // Try to remove from SceneView (this handles UI scene tree)
    if (m_scene_view.remove(scene_token, name_token))
    {
        Console::getInstance().addLog("Removed view '%s' from scene", name);
        removed = true;
    }

    if (removed)
    {
        // Clear selection if the removed object is currently selected
        clear_selection_if_matches(
            m_view_selection, name, scene_token, "Cleared selection (removed object was selected)");

        // Clear render view if the removed object is currently rendered
        clear_selection_if_matches(m_render_view_selection, name, scene_token,
                                   "Cleared render view selection (renderer will update next frame)");

        // If current view was removed, try to switch to another view
        pnanovdb_editor_token_t* current_view_token = m_scene_view.get_current_view(scene_token);
        if (current_view_token && current_view_token->id == name_token->id)
        {
            pnanovdb_editor_token_t* new_view_token = find_next_available_view(scene_token);
            m_scene_view.set_current_view(scene_token, new_view_token);
        }
    }

    return removed;
}

pnanovdb_editor_token_t* EditorScene::add_new_camera(const char* name)
{
    pnanovdb_editor_token_t* scene_token = get_current_scene_token();
    if (!scene_token)
    {
        return nullptr;
    }

    std::string camera_name = name ? name : "Camera";
    pnanovdb_editor_token_t* requested = EditorToken::getInstance().getToken(camera_name.c_str());
    bool occupied = false;
    m_scene_manager.with_object(scene_token, requested, [&](SceneObject* obj) { occupied = obj != nullptr; });
    if (name && occupied)
    {
        Console::getInstance().addLog(
            Console::LogLevel::Error, "Cannot create camera '%s': that name is already in use", name);
        return nullptr;
    }
    for (int suffix = 1; !name && occupied; ++suffix)
    {
        camera_name = "Camera " + std::to_string(suffix);
        requested = EditorToken::getInstance().getToken(camera_name.c_str());
        m_scene_manager.with_object(scene_token, requested, [&](SceneObject* obj) { occupied = obj != nullptr; });
    }

    pnanovdb_editor_token_t* name_token = m_scene_view.add_new_camera(scene_token, requested->str);

    if (name_token && scene_token)
    {
        // Register with EditorSceneManager for proper removal support
        // Get the camera's shared_ptr from SceneView
        const auto& cameras = m_scene_view.get_cameras();
        auto it = cameras.find(name_token->id);
        if (it != cameras.end() && it->second.camera_view)
        {
            if (!m_scene_manager.register_camera(scene_token, name_token, it->second.camera_view))
            {
                m_scene_view.remove(scene_token, name_token);
                return nullptr;
            }
        }
    }

    return name_token;
}

SceneViewData* EditorScene::get_or_create_scene(pnanovdb_editor_token_t* scene_token, bool create_default_camera)
{
    if (scene_token && m_active_scene_load && m_active_scene_load->cleanup_started_scene &&
        m_active_scene_load->started_scene_token && m_active_scene_load->started_scene_token->id == scene_token->id)
    {
        m_active_scene_load->cleanup_started_scene = false;
    }

    // Create scene if needed (this also creates the initial viewport camera)
    if (!ensure_scene_with_registered_cameras(m_scene_manager, m_scene_view, scene_token, create_default_camera))
    {
        return nullptr;
    }
    return m_scene_view.get_or_create_scene(scene_token, create_default_camera);
}

void EditorScene::initialize_for_startup(bool is_viewer_profile)
{
    m_scene_view.initialize_for_startup(is_viewer_profile);
    for (pnanovdb_editor_token_t* scene_token : m_scene_view.get_all_scene_tokens())
    {
        ensure_scene_with_registered_cameras(m_scene_manager, m_scene_view, scene_token);
    }
    sync_editor_camera_from_scene();
    apply_editor_camera_to_viewport();
}

void EditorScene::sync_restored_viewport_camera()
{
    sync_editor_camera_from_scene();
    apply_editor_camera_to_viewport();
}

const std::map<uint64_t, CameraViewContext>& EditorScene::get_camera_views() const
{
    return m_scene_view.get_cameras();
}

const std::map<uint64_t, NanoVDBContext>& EditorScene::get_nanovdb_views() const
{
    return m_scene_view.get_nanovdbs();
}

const std::map<uint64_t, GaussianDataContext>& EditorScene::get_gaussian_views() const
{
    return m_scene_view.get_gaussians();
}

std::vector<pnanovdb_editor_token_t*> EditorScene::get_ordered_renderable_views(pnanovdb_editor_token_t* scene_token) const
{
    pnanovdb_editor_token_t* token = scene_token ? scene_token : get_current_scene_token();
    return m_scene_view.get_ordered_renderable_views(token);
}

std::vector<pnanovdb_editor_token_t*> EditorScene::get_ordered_nanovdb_views(pnanovdb_editor_token_t* scene_token) const
{
    pnanovdb_editor_token_t* token = scene_token ? scene_token : get_current_scene_token();
    return m_scene_view.get_ordered_nanovdb_views(token);
}

std::vector<pnanovdb_editor_token_t*> EditorScene::get_ordered_gaussian_views(pnanovdb_editor_token_t* scene_token) const
{
    pnanovdb_editor_token_t* token = scene_token ? scene_token : get_current_scene_token();
    return m_scene_view.get_ordered_gaussian_views(token);
}

bool EditorScene::move_renderable_order(pnanovdb_editor_token_t* scene_token,
                                        pnanovdb_editor_token_t* name_token,
                                        int direction)
{
    pnanovdb_editor_token_t* token = scene_token ? scene_token : get_current_scene_token();
    return m_scene_view.move_renderable_order(token, name_token, direction);
}

bool EditorScene::move_renderable_before(pnanovdb_editor_token_t* scene_token,
                                         pnanovdb_editor_token_t* source_name_token,
                                         pnanovdb_editor_token_t* target_name_token)
{
    pnanovdb_editor_token_t* token = scene_token ? scene_token : get_current_scene_token();
    return m_scene_view.move_renderable_before(token, source_name_token, target_name_token);
}

EditorScene::ViewMapVariant EditorScene::get_views(ViewType type) const
{
    switch (type)
    {
    case ViewType::Cameras:
        return &m_scene_view.get_cameras();
    case ViewType::NanoVDBs:
        return &m_scene_view.get_nanovdbs();
    case ViewType::GaussianScenes:
        return &m_scene_view.get_gaussians();
    default:
        return std::monostate{};
    }
}

bool EditorScene::is_selection_valid(const SceneSelection& selection) const
{
    if (!selection.is_valid())
    {
        return false;
    }

    // For cameras, still check SceneView since cameras aren't in SceneManager yet
    if (selection.type == ViewType::Cameras)
    {
        auto map_variant = get_views(selection.type);
        return std::visit(
            [&selection](auto&& map_ptr) -> bool
            {
                using T = std::decay_t<decltype(map_ptr)>;
                if constexpr (std::is_same_v<T, std::monostate>)
                {
                    return false;
                }
                else
                {
                    uint64_t token_id = selection.name_token ? selection.name_token->id : 0;
                    return map_ptr && token_id != 0 && map_ptr->find(token_id) != map_ptr->end();
                }
            },
            map_variant);
    }

    // For NanoVDB and Gaussian data, validate against SceneManager
    pnanovdb_editor_token_t* scene_token = selection.scene_token ? selection.scene_token : get_current_scene_token();
    bool is_valid = false;
    m_scene_manager.with_object(
        scene_token, selection.name_token,
        [&](SceneObject* obj)
        {
            if (!obj)
            {
                return;
            }

            // Check that the object type matches the requested ViewType.
            const auto render_method = pnanovdb_pipeline_get_render_method(obj->render_pipeline());
            switch (selection.type)
            {
            case ViewType::NanoVDBs:
                switch (obj->type)
                {
                case SceneObjectType::NanoVDB:
                    is_valid = true;
                    break;
                case SceneObjectType::GaussianData:
                    is_valid = (obj->nanovdb_array() != nullptr || obj->converted_nanovdb() != nullptr);
                    break;
                case SceneObjectType::Array:
                {
                    const bool data_less = obj->nanovdb_array() == nullptr && obj->converted_nanovdb() == nullptr &&
                                           obj->gaussian_data() == nullptr && obj->named_arrays().empty();
                    is_valid = data_less || (render_method != pnanovdb_pipeline_render_method_gaussian);
                    break;
                }
                default:
                    break;
                }
                break;
            case ViewType::GaussianScenes:
                is_valid = (obj->type == SceneObjectType::GaussianData);
                break;
            default:
                break;
            }
        });
    return is_valid;
}

// ============================================================================
// Scene Selection Management
// ============================================================================

EditorSceneManager* EditorScene::get_scene_manager() const
{
    return &m_scene_manager;
}

uint64_t EditorScene::reserve_async_load_target(pnanovdb_editor_token_t* scene,
                                                pnanovdb_editor_token_t* name,
                                                bool replace_existing)
{
    if (m_async_load_target)
        return 0;
    uint64_t lifetime_id = 0;
    bool replacing = false;
    if (!scene || !name || !m_scene_manager.reserve_load_target(scene, name, &lifetime_id, replace_existing, &replacing))
        return 0;

    uint64_t id = m_next_async_load_id++;
    if (id == 0)
        id = m_next_async_load_id++;
    m_async_load_target = AsyncLoadTarget{ id, scene, name, lifetime_id, replacing };
    return id;
}

bool EditorScene::resolve_async_load_target(uint64_t reservation_id,
                                            pnanovdb_editor_token_t** scene,
                                            pnanovdb_editor_token_t** name,
                                            uint64_t* lifetime_id)
{
    if (!m_async_load_target || m_async_load_target->reservation_id != reservation_id)
        return false;

    bool matches = false;
    m_scene_manager.with_object(m_async_load_target->scene, m_async_load_target->name,
                                [&](SceneObject* obj)
                                {
                                    matches =
                                        obj && obj->lifetime_id == m_async_load_target->lifetime_id &&
                                        (m_async_load_target->replacing || obj->type == SceneObjectType::Uninitialized);
                                });
    if (!matches)
        return false;

    if (scene)
        *scene = m_async_load_target->scene;
    if (name)
        *name = m_async_load_target->name;
    if (lifetime_id)
        *lifetime_id = m_async_load_target->lifetime_id;
    return true;
}

void EditorScene::finish_async_load_target(uint64_t reservation_id, bool consumed)
{
    if (!m_async_load_target || m_async_load_target->reservation_id != reservation_id)
        return;
    if (!consumed)
        m_scene_manager.cancel_load_target(
            m_async_load_target->scene, m_async_load_target->name, m_async_load_target->lifetime_id);
    m_async_load_target.reset();
}

void EditorScene::record_async_load_publish(pnanovdb_editor_token_t* scene,
                                            pnanovdb_editor_token_t* name,
                                            uint64_t reserved_lifetime_id)
{
    if (!m_active_scene_load || !scene || !name || reserved_lifetime_id == 0 ||
        m_active_scene_load->started_lifetime_id != reserved_lifetime_id || !m_active_scene_load->scene_token ||
        !m_active_scene_load->name_token || m_active_scene_load->scene_token->id != scene->id ||
        m_active_scene_load->name_token->id != name->id)
    {
        return;
    }
    m_active_scene_load->published_lifetime_id = m_scene_manager.object_lifetime(scene, name);
}

void EditorScene::apply_editor_camera_to_viewport()
{
    if (!(m_editor && m_editor->impl && m_editor->impl->camera && m_imgui_settings))
    {
        return;
    }
    m_imgui_settings->camera_state = m_editor->impl->camera->state;
    m_imgui_settings->camera_config = m_editor->impl->camera->config;
    m_imgui_settings->sync_camera = PNANOVDB_TRUE;
}

void EditorScene::set_current_scene(pnanovdb_editor_token_t* scene_token)
{
    if (!scene_token)
    {
        return;
    }

    pnanovdb_editor_token_t* old_scene = m_scene_view.get_current_scene_token();

    // Get the currently selected view token before switching (to try to keep same name selected)
    pnanovdb_editor_token_t* previous_view_token = m_scene_view.get_current_view();

    m_scene_view.set_current_scene(scene_token);

    // Ensure the scene is created with the correct up-axis from render settings
    m_scene_view.get_or_create_scene(scene_token);

    // Sync editor's camera from the new scene's viewport camera when switching
    const bool switched = is_switching_scenes(old_scene, scene_token);
    if (switched)
    {
        sync_editor_camera_from_scene();
        // Push the new scene camera into the viewport for one-frame sync
        apply_editor_camera_to_viewport();
    }

    // Handle view selection when switching scenes
    if (switched)
    {
        pnanovdb_editor_token_t* view_to_select = nullptr;

        // Try to keep the same view name selected if it exists in the new scene
        if (previous_view_token)
        {
            // Check if a view with the same name exists in the new scene
            if (m_scene_view.get_nanovdb(scene_token, previous_view_token) ||
                m_scene_view.get_gaussian(scene_token, previous_view_token) ||
                m_scene_view.get_camera(scene_token, previous_view_token))
            {
                view_to_select = previous_view_token;
            }
        }

        // If no matching view found, try the last added view in the new scene
        if (!view_to_select)
        {
            SceneViewData* new_scene_data = m_scene_view.get_or_create_scene(scene_token);
            if (new_scene_data && new_scene_data->last_added_view_token_id != 0)
            {
                view_to_select = EditorToken::getInstance().getTokenById(new_scene_data->last_added_view_token_id);

                // Verify the last_added view actually exists in the scene
                if (view_to_select)
                {
                    ViewType check_type = determine_view_type(view_to_select, scene_token);
                    if (check_type == ViewType::None)
                    {
                        // Last added view doesn't exist anymore, find any available view
                        view_to_select = find_any_view_in_scene(scene_token);
                    }
                }
            }
            else
            {
                // No last_added tracked, find any available view
                view_to_select = find_any_view_in_scene(scene_token);
            }
        }

        // Select the chosen view or clear selection if nothing found
        if (view_to_select)
        {
            m_scene_view.set_current_view(scene_token, view_to_select);

            // Immediately update the editor's selection state
            // This is necessary because sync_selected_view_with_current() won't detect
            // the change if we call it right after setting the view (no before/after difference)
            ViewType view_type = determine_view_type(view_to_select, scene_token);
            if (view_type != ViewType::None)
            {
                set_properties_selection(view_type, view_to_select, scene_token);
                set_render_view(view_type, view_to_select, scene_token);
            }
        }
        else
        {
            // No views in the scene, clear selection
            clear_selection();
        }
    }
}

pnanovdb_editor_token_t* EditorScene::get_current_scene_token() const
{
    return m_scene_view.get_current_scene_token();
}

bool EditorScene::rename_scene(pnanovdb_editor_token_t* old_scene_token, pnanovdb_editor_token_t* new_scene_token)
{
    if (!old_scene_token || !new_scene_token)
    {
        return false;
    }
    if (old_scene_token->id == new_scene_token->id)
    {
        return true;
    }

    bool old_scene_exists = false;
    bool new_scene_exists = false;
    for (pnanovdb_editor_token_t* token : m_scene_view.get_all_scene_tokens())
    {
        if (!token)
        {
            continue;
        }
        if (token->id == old_scene_token->id)
        {
            old_scene_exists = true;
        }
        if (token->id == new_scene_token->id)
        {
            new_scene_exists = true;
        }
    }
    if (!old_scene_exists || new_scene_exists)
    {
        return false;
    }

    struct RenamedProcessTarget
    {
        pnanovdb_editor_token_t* name = nullptr;
        uint64_t lifetime_id = 0;
    };
    std::vector<RenamedProcessTarget> process_targets;
    m_scene_manager.for_each_object(
        [&](SceneObject* obj)
        {
            if (obj && obj->scene_token && obj->scene_token->id == old_scene_token->id && obj->name_token)
                process_targets.push_back({ obj->name_token, obj->lifetime_id });
            return true;
        });

    if (!m_scene_manager.rename_scene(old_scene_token, new_scene_token))
    {
        return false;
    }
    if (!m_scene_view.rename_scene(old_scene_token, new_scene_token))
    {
        return false;
    }

    for (const RenamedProcessTarget& target : process_targets)
    {
        pipeline_retarget_async_process_target(
            old_scene_token, target.name, new_scene_token, target.name, target.lifetime_id);
    }

    if (m_view_selection.scene_token && m_view_selection.scene_token->id == old_scene_token->id)
    {
        m_view_selection.scene_token = new_scene_token;
    }
    if (m_render_view_selection.scene_token && m_render_view_selection.scene_token->id == old_scene_token->id)
    {
        m_render_view_selection.scene_token = new_scene_token;
    }
    if (m_imgui_instance->source_scene_token && m_imgui_instance->source_scene_token->id == old_scene_token->id)
    {
        m_imgui_instance->source_scene_token = new_scene_token;
    }

    if (m_async_load_target && m_async_load_target->scene && m_async_load_target->scene->id == old_scene_token->id)
        m_async_load_target->scene = new_scene_token;

    for (PendingSceneLoad& pending : m_pending_scene_loads)
    {
        if (pending.scene_token && pending.scene_token->id == old_scene_token->id)
        {
            pending.scene_token = new_scene_token;
        }
    }
    if (m_active_scene_load && m_active_scene_load->scene_token &&
        m_active_scene_load->scene_token->id == old_scene_token->id)
    {
        m_active_scene_load->scene_token = new_scene_token;
        if (m_active_scene_load->started_scene_token &&
            m_active_scene_load->started_scene_token->id == old_scene_token->id)
            m_active_scene_load->started_scene_token = new_scene_token;
        m_active_scene_load->cleanup_started_scene = false;
    }
    return true;
}

bool EditorScene::rename_object(pnanovdb_editor_token_t* scene_token,
                                pnanovdb_editor_token_t* old_name_token,
                                pnanovdb_editor_token_t* new_name_token)
{
    if (!scene_token || !old_name_token || !new_name_token)
    {
        return false;
    }
    if (old_name_token->id == new_name_token->id)
    {
        return true;
    }

    // Reject if the destination name is already used by another object in this scene.
    bool name_conflict = false;
    m_scene_manager.with_object(scene_token, new_name_token,
                                [&](SceneObject* obj)
                                {
                                    if (obj)
                                    {
                                        name_conflict = true;
                                    }
                                });
    if (name_conflict)
    {
        return false;
    }

    uint64_t lifetime_id = 0;
    m_scene_manager.with_object(scene_token, old_name_token,
                                [&](SceneObject* obj)
                                {
                                    if (obj)
                                        lifetime_id = obj->lifetime_id;
                                });

    if (!m_scene_manager.rename_object(scene_token, old_name_token, new_name_token))
    {
        return false;
    }
    m_scene_view.rename_object(scene_token, old_name_token, new_name_token);
    pipeline_retarget_async_process_target(scene_token, old_name_token, scene_token, new_name_token, lifetime_id);
    if (m_async_load_target && m_async_load_target->scene && m_async_load_target->name &&
        m_async_load_target->scene->id == scene_token->id && m_async_load_target->name->id == old_name_token->id &&
        m_async_load_target->lifetime_id == lifetime_id)
    {
        m_async_load_target->name = new_name_token;
    }
    if (m_active_scene_load && m_active_scene_load->scene_token && m_active_scene_load->name_token &&
        m_active_scene_load->scene_token->id == scene_token->id &&
        m_active_scene_load->name_token->id == old_name_token->id &&
        m_active_scene_load->started_lifetime_id == lifetime_id)
    {
        m_active_scene_load->name_token = new_name_token;
    }

    auto fix_selection = [&](SceneSelection& sel)
    {
        if (sel.name_token && sel.name_token->id == old_name_token->id &&
            (!sel.scene_token || sel.scene_token->id == scene_token->id))
        {
            sel.name_token = new_name_token;
        }
    };
    fix_selection(m_view_selection);
    fix_selection(m_render_view_selection);
    if (m_imgui_instance->source_scene_token && m_imgui_instance->source_name_token &&
        m_imgui_instance->source_scene_token->id == scene_token->id &&
        m_imgui_instance->source_name_token->id == old_name_token->id)
    {
        m_imgui_instance->source_name_token = new_name_token;
    }
    return true;
}

bool EditorScene::remove_scene(pnanovdb_editor_token_t* scene_token)
{
    if (!scene_token)
    {
        return false;
    }

    if (m_async_load_target && m_async_load_target->scene && m_async_load_target->scene->id == scene_token->id)
    {
        m_scene_manager.cancel_load_target(
            m_async_load_target->scene, m_async_load_target->name, m_async_load_target->lifetime_id);
        m_async_load_target.reset();
    }

    const auto targets_scene = [scene_token](const PendingSceneLoad& pending)
    { return pending.scene_token && pending.scene_token->id == scene_token->id; };
    m_pending_scene_loads.erase(std::remove_if(m_pending_scene_loads.begin(), m_pending_scene_loads.end(), targets_scene),
                                m_pending_scene_loads.end());
    if (m_active_scene_load && targets_scene(*m_active_scene_load))
    {
        m_active_scene_load->discard = true;
        m_active_scene_load->cleanup_started_scene = false;
    }
    if (m_imgui_instance->source_scene_token && m_imgui_instance->source_scene_token->id == scene_token->id)
    {
        m_imgui_instance->source_scene_token = nullptr;
        m_imgui_instance->source_name_token = nullptr;
    }

    std::vector<pnanovdb_editor_token_t*> scene_object_tokens;
    m_scene_manager.for_each_object(
        [&](SceneObject* obj)
        {
            if (obj && obj->scene_token && obj->scene_token->id == scene_token->id && obj->name_token)
            {
                scene_object_tokens.push_back(obj->name_token);
            }
            return true;
        });

    for (pnanovdb_editor_token_t* name_token : scene_object_tokens)
    {
        m_scene_manager.remove(scene_token, name_token);
    }

    const bool removed = m_scene_view.remove_scene(scene_token);
    if (!removed)
    {
        return false;
    }

    if ((m_view_selection.scene_token && m_view_selection.scene_token->id == scene_token->id) ||
        (m_render_view_selection.scene_token && m_render_view_selection.scene_token->id == scene_token->id))
    {
        clear_selection();
    }

    return true;
}

bool EditorScene::discard_untouched_startup_scene()
{
    const std::vector<pnanovdb_editor_token_t*> scenes = m_scene_view.get_all_scene_tokens();
    pnanovdb_editor_token_t* default_scene = EditorToken::getInstance().getToken(DEFAULT_SCENE_NAME);
    pnanovdb_editor_token_t* default_camera = EditorToken::getInstance().getToken("Camera");
    if (scenes.size() != 1 || !scenes.front() || scenes.front()->id != default_scene->id)
        return false;

    const SceneViewData* data = m_scene_view.get_scene_data(default_scene);
    if (!data || !data->nanovdbs.empty() || !data->gaussians.empty() || !data->renderable_order.empty() ||
        data->cameras.size() != 1 || data->cameras.begin()->first != default_camera->id)
    {
        return false;
    }

    size_t object_count = 0;
    bool contains_only_default_camera = true;
    m_scene_manager.for_each_object(
        [&](SceneObject* object)
        {
            ++object_count;
            contains_only_default_camera = object && object->type == SceneObjectType::Camera && object->scene_token &&
                                           object->name_token && object->scene_token->id == default_scene->id &&
                                           object->name_token->id == default_camera->id;
            return contains_only_default_camera;
        });
    return object_count == 1 && contains_only_default_camera && remove_scene(default_scene);
}

std::vector<pnanovdb_editor_token_t*> EditorScene::get_all_scene_tokens() const
{
    return m_scene_view.get_all_scene_tokens();
}

bool EditorScene::has_valid_selection() const
{
    return is_selection_valid(m_view_selection);
}

void EditorScene::clear_selection()
{
    m_view_selection = SceneSelection();
    m_render_view_selection = SceneSelection();
    // Clear displayed data and reset shader_name to default when no object is selected
    clear_editor_view_state();
    m_editor->impl->shader_name = pnanovdb_pipeline_get_shader_name(pnanovdb_pipeline_type_nanovdb_render);
}

void EditorScene::set_properties_selection(ViewType type,
                                           pnanovdb_editor_token_t* name_token,
                                           pnanovdb_editor_token_t* scene_token)
{
    // If no scene token provided, use current scene
    if (!scene_token)
    {
        scene_token = get_current_scene_token();
    }

    // Validate using is_selection_valid which now checks SceneManager
    SceneSelection candidate = { type, name_token, scene_token };
    if (is_selection_valid(candidate))
    {
        m_view_selection = candidate;

        m_scene_manager.with_object(
            scene_token, name_token,
            [&](SceneObject* obj)
            {
                if (obj)
                {
                    Console::getInstance().addLog(Console::LogLevel::Debug,
                                                  "set_properties_selection: object found, shader_name='%s'",
                                                  token_to_string_log(obj->shader_name()));
                }
                else
                {
                    Console::getInstance().addLog(Console::LogLevel::Debug, "set_properties_selection: object is null!");
                }
            });
    }
    else
    {
        m_view_selection.type = ViewType::None;
        m_view_selection.name_token = nullptr;
        m_view_selection.scene_token = nullptr;
    }
}

SceneSelection EditorScene::get_properties_selection() const
{
    return m_view_selection;
}

void EditorScene::set_render_view(ViewType type, pnanovdb_editor_token_t* name_token, pnanovdb_editor_token_t* scene_token)
{
    if (!name_token || !is_selection_valid(SceneSelection{ type, name_token, scene_token }) ||
        (type != ViewType::NanoVDBs && type != ViewType::GaussianScenes))
    {
        clear_editor_view_state();
        m_render_view_selection = SceneSelection();
        return;
    }

    m_render_view_selection = { type, name_token, scene_token };

    m_scene_manager.with_object(scene_token, name_token,
                                [&](SceneObject* scene_obj)
                                {
                                    if (!scene_obj)
                                    {
                                        return;
                                    }

                                    // Load into editor and UI so renderer switches to the requested view
                                    load_view_into_editor_and_ui(scene_obj);

                                    // Ensure default shader params for the selected view are ready even before
                                    // (re)compile.  Use the render_method overload since we're already inside
                                    // with_object() and must not re-acquire the scene manager mutex.
                                    auto rm = pipeline_get_render_method(scene_obj->render_pipeline());
                                    reload_shader_params_for_current_view(rm);
                                });
}

SceneSelection EditorScene::get_render_view_selection() const
{
    return m_render_view_selection;
}

// ============================================================================
// Camera State Management
// ============================================================================

void EditorScene::save_camera_state(pnanovdb_editor_token_t* name_token, const pnanovdb_camera_state_t& state)
{
    if (name_token)
    {
        m_saved_camera_states[name_token->id] = state;
    }
}

const pnanovdb_camera_state_t* EditorScene::get_saved_camera_state(pnanovdb_editor_token_t* name_token) const
{
    if (!name_token)
    {
        return nullptr;
    }
    auto it = m_saved_camera_states.find(name_token->id);
    return (it != m_saved_camera_states.end()) ? &it->second : nullptr;
}

int EditorScene::get_camera_frustum_index(pnanovdb_editor_token_t* camera_name_token) const
{
    if (!camera_name_token)
    {
        return 0;
    }
    auto it = m_camera_frustum_index.find(camera_name_token->id);
    return (it != m_camera_frustum_index.end()) ? it->second : 0;
}

void EditorScene::set_camera_frustum_index(pnanovdb_editor_token_t* camera_name_token, int index)
{
    if (camera_name_token)
    {
        m_camera_frustum_index[camera_name_token->id] = index;
    }
}

// ============================================================================
// Helper Methods (Private)
// ============================================================================

void EditorScene::clear_selection_if_matches(SceneSelection& selection,
                                             const char* name,
                                             pnanovdb_editor_token_t* scene_token,
                                             const char* log_message)
{
    if (selection.name_token && selection.name_token->str && strcmp(selection.name_token->str, name) == 0)
    {
        // Check if selection belongs to the same scene
        if (!selection.scene_token || !scene_token || selection.scene_token->id == scene_token->id)
        {
            selection.type = ViewType::None;
            selection.name_token = nullptr;
            selection.scene_token = nullptr;
            Console::getInstance().addLog("%s", log_message);
        }
    }
}

bool EditorScene::is_switching_scenes(pnanovdb_editor_token_t* from_scene, pnanovdb_editor_token_t* to_scene) const
{
    // Null to null is not a switch
    if (!from_scene && !to_scene)
    {
        return false;
    }

    // Null to non-null or non-null to null is a switch
    if (!from_scene || !to_scene)
    {
        return true;
    }

    return from_scene != to_scene;
}

pnanovdb_editor_token_t* EditorScene::find_next_available_view(pnanovdb_editor_token_t* scene_token) const
{
    const auto& nanovdbs = m_scene_view.get_nanovdbs();
    const auto& gaussians = m_scene_view.get_gaussians();

    if (!nanovdbs.empty())
    {
        pnanovdb_editor_token_t* new_view_token = EditorToken::getInstance().getTokenById(nanovdbs.begin()->first);
        Console::getInstance().addLog("Switched view to '%s'", new_view_token ? new_view_token->str : "unknown");
        return new_view_token;
    }
    else if (!gaussians.empty())
    {
        pnanovdb_editor_token_t* new_view_token = EditorToken::getInstance().getTokenById(gaussians.begin()->first);
        Console::getInstance().addLog("Switched view to '%s'", new_view_token ? new_view_token->str : "unknown");
        return new_view_token;
    }

    Console::getInstance().addLog("No views remaining in scene");
    return nullptr;
}

pnanovdb_editor_token_t* EditorScene::find_any_view_in_scene(pnanovdb_editor_token_t* scene_token) const
{
    if (!scene_token)
    {
        return nullptr;
    }

    // Cast away const since we're not modifying the scene, just reading its contents
    // This is safe because get_or_create_scene only creates if needed, and we're just reading the maps
    SceneView& non_const_view = const_cast<SceneView&>(m_scene_view);
    SceneViewData* scene = non_const_view.get_or_create_scene(scene_token);
    if (!scene)
    {
        return nullptr;
    }

    // Try to find a nanovdb first
    if (!scene->nanovdbs.empty())
    {
        return EditorToken::getInstance().getTokenById(scene->nanovdbs.begin()->first);
    }

    // Then try gaussians
    if (!scene->gaussians.empty())
    {
        return EditorToken::getInstance().getTokenById(scene->gaussians.begin()->first);
    }

    // No content views found
    return nullptr;
}

void* EditorScene::get_view_params_with_fallback(SceneShaderParams& params, void* obj_params) const
{
    if (obj_params)
    {
        return obj_params;
    }

    if (params.default_array && params.default_array->data)
    {
        return params.default_array->data;
    }

    return nullptr;
}

// ============================================================================
// NanoVDB File Operations
// ============================================================================

void EditorScene::load_nanovdb_to_editor()
{
    const char* filepath = m_imgui_instance->nanovdb_filepath.c_str();

    m_editor->impl->nanovdb_array = m_compute->load_nanovdb(filepath);

    handle_nanovdb_data_load(
        get_current_scene_token(), m_editor->impl->nanovdb_array, m_imgui_instance->nanovdb_filepath.c_str());
}

void EditorScene::save_editor_nanovdb()
{
    if (!m_editor || !m_editor->impl || !m_editor->impl->nanovdb_array)
    {
        pnanovdb_editor::Console::getInstance().addLog(Console::LogLevel::Error, "No NanoVDB array to save");
        return;
    }

    const char* filepath = m_imgui_instance->nanovdb_filepath.c_str();

    pnanovdb_bool_t result = m_compute->save_nanovdb(m_editor->impl->nanovdb_array, filepath);
    if (result == PNANOVDB_TRUE)
    {
        pnanovdb_editor::Console::getInstance().addLog("NanoVDB saved to '%s'", filepath);
    }
    else
    {
        pnanovdb_editor::Console::getInstance().addLog("Failed to save NanoVDB to '%s'", filepath);
    }
}

void EditorScene::select_render_view(pnanovdb_editor_token_t* scene, pnanovdb_editor_token_t* name)
{
    if (!scene || !name)
    {
        return;
    }

    // Set the current view in the specified scene
    m_scene_view.set_current_view(scene, name);

    // Sync the selection
    sync_selected_view_with_current();
}

bool EditorScene::load_nanovdb_file(pnanovdb_editor_token_t* scene,
                                    const char* filepath,
                                    pnanovdb_pipeline_type_t render_pipeline,
                                    pnanovdb_editor_token_t* name)
{
    return nanovdb_import::nanovdb(*this, m_compute, scene, filepath, render_pipeline, name);
}

bool EditorScene::load_mesh_file(pnanovdb_editor_token_t* scene,
                                 const char* filepath,
                                 const pnanovdb_editor::mesh_import::Options& options,
                                 pnanovdb_editor_token_t* name)
{
    return mesh_import::mesh(m_compute, scene, filepath, options, name);
}

bool EditorScene::save_nanovdb_file(pnanovdb_editor_token_t* scene, pnanovdb_editor_token_t* name, const char* filepath)
{
    if (!scene || !name || !filepath || !m_compute)
        return false;

    pnanovdb_compute_array_t* array = nullptr;
    m_scene_manager.with_object(scene, name,
                                [&](SceneObject* obj)
                                {
                                    if (obj)
                                    {
                                        // Accept both native NanoVDB objects and GaussianData converted via Raster3D
                                        array = obj->nanovdb_array() ? obj->nanovdb_array() : obj->converted_nanovdb();
                                    }
                                });

    if (!array)
    {
        pnanovdb_editor::Console::getInstance().addLog(
            Console::LogLevel::Error, "No NanoVDB array found for '%s'", name->str);
        return false;
    }

    pnanovdb_bool_t result = m_compute->save_nanovdb(array, filepath);
    if (result == PNANOVDB_TRUE)
    {
        pnanovdb_editor::Console::getInstance().addLog("NanoVDB saved to '%s'", filepath);
        return true;
    }
    else
    {
        pnanovdb_editor::Console::getInstance().addLog(
            Console::LogLevel::Error, "Failed to save NanoVDB to '%s'", filepath);
        return false;
    }
}

bool EditorScene::save_nanovdb_file_stage(pnanovdb_editor_token_t* scene,
                                          pnanovdb_editor_token_t* name,
                                          int step_index,
                                          const char* filepath)
{
    if (!scene || !name || !filepath || !m_compute)
        return false;

    pnanovdb_compute_array_t* array = nullptr;
    m_scene_manager.with_object(
        scene, name,
        [&](SceneObject* obj)
        {
            if (!obj)
                return;
            if (step_index < 0 || (size_t)step_index >= obj->pipeline.process_count())
            {
                array = obj->nanovdb_array() ? obj->nanovdb_array() : obj->converted_nanovdb();
            }
            else
            {
                array = obj->pipeline.process_step((size_t)step_index).output.get_array(k_stage_output_nanovdb);
            }
        });

    if (!array)
    {
        pnanovdb_editor::Console::getInstance().addLog(
            Console::LogLevel::Error, "No NanoVDB output retained for '%s' step %d", name->str, step_index);
        return false;
    }

    pnanovdb_bool_t result = m_compute->save_nanovdb(array, filepath);
    if (result == PNANOVDB_TRUE)
    {
        pnanovdb_editor::Console::getInstance().addLog("NanoVDB (step %d) saved to '%s'", step_index, filepath);
        return true;
    }
    pnanovdb_editor::Console::getInstance().addLog(
        Console::LogLevel::Error, "Failed to save NanoVDB (step %d) to '%s'", step_index, filepath);
    return false;
}

bool EditorScene::load_gaussian_file(pnanovdb_editor_token_t* scene_token,
                                     const char* filepath,
                                     pnanovdb_pipeline_type_t process_pipeline,
                                     pnanovdb_pipeline_type_t render_pipeline,
                                     float voxels_per_unit,
                                     pnanovdb_editor_token_t* name,
                                     bool replace_existing)
{
    if (!scene_token)
    {
        Console::getInstance().addLog(Console::LogLevel::Error, "load_gaussian_file: no target scene");
        return false;
    }

    return gaussian_import::gaussian(*this, m_scene_manager, m_compute, scene_token, filepath, process_pipeline,
                                     render_pipeline, voxels_per_unit, name, replace_existing);
}

std::string EditorScene::get_selected_object_shader_name() const
{
    std::string shader_name;
    auto* scene_token = get_current_scene_token();
    m_scene_manager.with_object(scene_token, m_view_selection.name_token,
                                [&](SceneObject* scene_obj)
                                {
                                    if (scene_obj)
                                    {
                                        shader_name = token_to_string(scene_obj->shader_name());
                                    }
                                });
    return shader_name;
}

void EditorScene::set_selected_object_shader_name(const std::string& shader_name)
{
    auto* scene_token = get_current_scene_token();
    pnanovdb_editor_token_t* new_token = EditorToken::getInstance().getToken(shader_name.c_str());
    m_scene_manager.with_object(scene_token, m_view_selection.name_token,
                                [&](SceneObject* scene_obj)
                                {
                                    if (!scene_obj)
                                    {
                                        return;
                                    }
                                    pnanovdb_editor_token_t* prev_token = scene_obj->shader_name();
                                    scene_obj->shader_name() = new_token;
                                    // Also update the editor->impl->shader_name to mirror it
                                    if (m_editor && m_editor->impl)
                                    {
                                        m_editor->impl->shader_name = shader_name;
                                    }
                                    if (m_compute && !tokens_equal(prev_token, new_token))
                                    {
                                        m_scene_manager.refresh_params_for_object(m_compute, *scene_obj);
                                    }
                                });
}

bool EditorScene::save_scene_file(const std::string& filepath)
{
    if (!m_pending_scene_loads.empty() || m_active_scene_load || m_async_load_target)
    {
        Console::getInstance().addLog(
            Console::LogLevel::Error, "Save scene failed: wait for pending imports/restores to finish");
        return false;
    }
    std::string error;
    if (!save_scenes_to_file(m_scene_manager, m_scene_view, filepath, &error))
    {
        Console::getInstance().addLog(Console::LogLevel::Error, "Save scene failed: %s", error.c_str());
        return false;
    }
    Console::getInstance().addLog("Scene saved to '%s'", filepath.c_str());
    return true;
}

bool EditorScene::load_scene_file(const std::string& filepath)
{
    std::ifstream file(filepath);
    if (!file)
    {
        Console::getInstance().addLog(Console::LogLevel::Error, "Load scene: cannot open '%s'", filepath.c_str());
        return false;
    }

    nlohmann::json doc;
    try
    {
        file >> doc;
    }
    catch (const nlohmann::json::exception& e)
    {
        Console::getInstance().addLog(
            Console::LogLevel::Error, "Load scene: invalid JSON in '%s': %s", filepath.c_str(), e.what());
        return false;
    }

    int serialized_version = k_scene_file_version;
    if (doc.contains("version") && doc["version"].is_number_integer())
    {
        try
        {
            serialized_version = doc["version"].get<int>();
        }
        catch (const nlohmann::json::exception& e)
        {
            Console::getInstance().addLog(Console::LogLevel::Warning, "Load scene: ignoring invalid version in '%s': %s",
                                          filepath.c_str(), e.what());
            serialized_version = k_scene_file_version;
        }
    }
    if (serialized_version != k_scene_file_version)
    {
        Console::getInstance().addLog(Console::LogLevel::Error, "Load scene: unsupported file version %d (expected %d)",
                                      serialized_version, k_scene_file_version);
        return false;
    }

    if (!doc.contains("objects") || !doc["objects"].is_array())
    {
        Console::getInstance().addLog(
            Console::LogLevel::Error, "Load scene: '%s' has no 'objects' array", filepath.c_str());
        return false;
    }

    size_t queued = 0;
    size_t restored = 0;
    size_t restored_scenes = 0;
    std::set<uint64_t> created_object_scene_ids;

    const auto scenes = doc.find("scenes");
    const bool has_serialized_scenes = scenes != doc.end() && scenes->is_array();
    if (has_serialized_scenes)
    {
        for (const auto& scene_json : *scenes)
        {
            try
            {
                if (!scene_json.is_object())
                {
                    continue;
                }
                auto name_it = scene_json.find("name");
                if (name_it == scene_json.end() || !name_it->is_string() ||
                    name_it->get_ref<const std::string&>().empty())
                {
                    Console::getInstance().addLog(
                        Console::LogLevel::Warning, "Load scene: scene record has no valid name; skipping");
                    continue;
                }
                const std::string& scene_name = name_it->get_ref<const std::string&>();
                pnanovdb_editor_token_t* scene_token = EditorToken::getInstance().getToken(scene_name.c_str());
                const bool scene_exists = m_scene_view.get_scene_data(scene_token) != nullptr;
                if (!scene_token || !get_or_create_scene(scene_token, scene_exists))
                {
                    continue;
                }
                ++restored_scenes;

                const auto normalize_viewport = [&]()
                {
                    if (!normalize_scene_viewport_camera(m_scene_manager, m_scene_view, scene_token))
                    {
                        Console::getInstance().addLog(Console::LogLevel::Warning,
                                                      "Load scene: cannot establish a viewport camera for scene '%s'",
                                                      scene_name.c_str());
                    }
                };

                auto cameras = scene_json.find("cameras");
                if (cameras == scene_json.end())
                {
                    normalize_viewport();
                    continue;
                }
                if (!cameras->is_array())
                {
                    Console::getInstance().addLog(Console::LogLevel::Warning,
                                                  "Load scene: cameras for scene '%s' are not an array; skipping",
                                                  scene_name.c_str());
                    normalize_viewport();
                    continue;
                }
                for (const auto& camera_json : *cameras)
                {
                    try
                    {
                        if (!camera_json.is_object())
                        {
                            continue;
                        }
                        auto camera_name_it = camera_json.find("name");
                        if (camera_name_it == camera_json.end() || !camera_name_it->is_string() ||
                            camera_name_it->get_ref<const std::string&>().empty())
                        {
                            Console::getInstance().addLog(Console::LogLevel::Warning,
                                                          "Load scene: camera in scene '%s' has no valid name; skipping",
                                                          scene_name.c_str());
                            continue;
                        }
                        auto entries = camera_json.find("entries");
                        if (entries != camera_json.end() && entries->is_array() &&
                            entries->size() > (size_t)std::numeric_limits<pnanovdb_uint32_t>::max())
                        {
                            Console::getInstance().addLog(
                                Console::LogLevel::Warning,
                                "Load scene: camera in scene '%s' has too many entries; skipping", scene_name.c_str());
                            continue;
                        }
                        const std::string& camera_name = camera_name_it->get_ref<const std::string&>();
                        pnanovdb_editor_token_t* camera_token = EditorToken::getInstance().getToken(camera_name.c_str());
                        if (m_scene_view.get_camera(scene_token, camera_token))
                        {
                            Console::getInstance().addLog(
                                Console::LogLevel::Warning,
                                "Load scene: camera name '%s' already exists in scene '%s'; skipping",
                                camera_name.c_str(), scene_name.c_str());
                            continue;
                        }
                        CameraViewContext context = camera_from_json(
                            camera_json, camera_token, m_imgui_settings ? m_imgui_settings->is_y_up : PNANOVDB_TRUE);
                        if (!m_scene_manager.register_camera(scene_token, camera_token, context.camera_view))
                        {
                            Console::getInstance().addLog(
                                Console::LogLevel::Warning,
                                "Load scene: camera '%s' conflicts with a render object in scene '%s'; skipping",
                                camera_name.c_str(), scene_name.c_str());
                            continue;
                        }
                        m_scene_view.add_camera(scene_token, camera_token, context);
                        auto viewport = camera_json.find("viewport");
                        if (viewport != camera_json.end() && viewport->is_boolean() && viewport->get<bool>())
                        {
                            m_scene_view.set_viewport_camera(scene_token, camera_token);
                        }
                    }
                    catch (const nlohmann::json::exception& e)
                    {
                        Console::getInstance().addLog(Console::LogLevel::Warning,
                                                      "Load scene: invalid camera in scene '%s': %s; skipping",
                                                      scene_name.c_str(), e.what());
                    }
                    catch (const std::exception& e)
                    {
                        Console::getInstance().addLog(Console::LogLevel::Warning,
                                                      "Load scene: cannot restore camera in scene '%s': %s; skipping",
                                                      scene_name.c_str(), e.what());
                    }
                }
                normalize_viewport();
            }
            catch (const nlohmann::json::exception& e)
            {
                Console::getInstance().addLog(
                    Console::LogLevel::Warning, "Load scene: invalid scene record: %s; skipping", e.what());
            }
        }
    }

    for (const auto& obj : doc["objects"])
    {
        try
        {
            std::string scene_name;
            std::string object_name;
            std::string type;
            std::string source;
            std::string validation_error;
            if (!parse_scene_object_record(obj, scene_name, object_name, type, source, &validation_error))
            {
                Console::getInstance().addLog(Console::LogLevel::Warning,
                                              "Load scene: invalid object record (%s); skipping",
                                              validation_error.c_str());
                continue;
            }

            pnanovdb_editor_token_t* scene_token = EditorToken::getInstance().getToken(scene_name.c_str());
            if (!scene_token)
            {
                continue;
            }
            pnanovdb_editor_token_t* name_token = EditorToken::getInstance().getToken(object_name.c_str());

            auto has_target = [&](const PendingSceneLoad& existing)
            {
                return existing.scene_token && existing.name_token && existing.scene_token->id == scene_token->id &&
                       existing.name_token->id == name_token->id;
            };
            auto target_name_in_use = [&]()
            {
                bool in_use = false;
                m_scene_manager.with_object(
                    scene_token, name_token, [&](SceneObject* existing) { in_use = existing != nullptr; });
                return in_use || std::any_of(m_pending_scene_loads.begin(), m_pending_scene_loads.end(), has_target) ||
                       (m_active_scene_load && has_target(*m_active_scene_load));
            };
            auto reject_name_conflict = [&]()
            {
                if (!target_name_in_use())
                    return false;
                Console::getInstance().addLog(Console::LogLevel::Error,
                                              "Load scene: object name '%s' already exists in scene '%s'; skipping",
                                              object_name.c_str(), scene_name.c_str());
                return true;
            };
            if (reject_name_conflict())
            {
                continue;
            }

            pnanovdb_pipeline_type_t render_type = pnanovdb_pipeline_type_nanovdb_render;
            pnanovdb_pipeline_type_t process0_type = pnanovdb_pipeline_type_noop;
            pnanovdb_pipeline_type_t load_type = pnanovdb_pipeline_type_noop;
            float process0_voxels_per_unit = pnanovdb_editor::k_default_voxels_per_unit;
            if (obj.contains("pipeline") && obj["pipeline"].is_object())
            {
                const auto& pj = obj["pipeline"];
                if (pj.contains("load") && pj["load"].is_object())
                {
                    decode_scene_pipeline_type(
                        pj["load"], pnanovdb_pipeline_stage_load, load_type, object_name.c_str(), "load");
                }
                if (pj.contains("render") && pj["render"].is_object())
                {
                    decode_scene_pipeline_type(
                        pj["render"], pnanovdb_pipeline_stage_render, render_type, object_name.c_str(), "render");
                }
                if (pj.contains("process") && pj["process"].is_array() && !pj["process"].empty() &&
                    pj["process"][0].is_object())
                {
                    decode_scene_pipeline_type(pj["process"][0], pnanovdb_pipeline_stage_process, process0_type,
                                               object_name.c_str(), "process");
                    if (process0_type == pnanovdb_pipeline_type_gaussian_voxelize)
                    {
                        pnanovdb_pipeline_params_t params{};
                        pnanovdb_pipeline_get_default_params(process0_type, &params);
                        const auto& stage = pj["process"][0];
                        if (stage.contains("params") && stage["params"].is_object())
                        {
                            json_to_reflect_params(
                                stage["params"], params.type, params.data, static_cast<size_t>(params.size));
                        }
                        process0_voxels_per_unit = pipeline_params_get_voxels_per_unit(&params);
                        pipeline_params_release(&params);
                    }
                }
            }

            PendingSceneLoad pending;
            pending.scene_token = scene_token;
            pending.name_token = name_token;
            pending.process_type = process0_type;
            pending.render_type = render_type;
            pending.voxels_per_unit = process0_voxels_per_unit;
            pending.source_filepath = source;
            pending.object_json = obj.dump();

            const bool load_as_gaussian = load_type == pnanovdb_pipeline_type_gaussian_load;
            const bool load_as_mesh = load_type == pnanovdb_pipeline_type_mesh_load;
            const bool restore_empty = source.empty();

            if (!restore_empty)
            {
                if (!load_as_gaussian && !load_as_mesh && type == "nanovdb")
                {
                    pending.load_type = PendingSceneLoadType::NanoVDB;
                }
                else if (load_as_gaussian || (!load_as_mesh && type == "gaussian"))
                {
                    pending.load_type = PendingSceneLoadType::Gaussian;
                    if (process0_type == pnanovdb_pipeline_type_noop &&
                        render_type == pnanovdb_pipeline_type_nanovdb_render)
                    {
                        pending.render_type = pnanovdb_pipeline_type_gaussian_splat;
                    }
                }
                else
                {
                    pending.load_type = PendingSceneLoadType::Mesh;
                }
            }

            // Only accepted records may create a scene/default camera. Validation
            // above intentionally has no SceneView side effects.
            const bool scene_existed = m_scene_view.get_scene_data(scene_token) != nullptr;
            SceneViewData* target_scene = get_or_create_scene(scene_token);
            if (!scene_existed && target_scene && created_object_scene_ids.insert(scene_token->id).second)
            {
                ++restored_scenes;
                if (!m_scene_view.get_current_scene_token())
                    m_scene_view.set_current_scene(scene_token);
            }
            // A file may omit `scenes`, so creating the target scene above can
            // synthesize and register its default Camera after the first name
            // check. Recheck the unified namespace before restoring this record.
            if (reject_name_conflict())
            {
                continue;
            }
            if (restore_empty)
            {
                create_empty_object(scene_token, name_token, type);
                apply_object_restore(scene_token, name_token, pending.object_json);
                ++restored;
            }
            else
            {
                m_pending_scene_loads.push_back(std::move(pending));
                ++queued;
            }
        }
        catch (const nlohmann::json::exception& e)
        {
            Console::getInstance().addLog(
                Console::LogLevel::Warning, "Load scene: invalid object record: %s; skipping", e.what());
        }
    }

    Console::getInstance().addLog("Loading scene '%s' (%zu scene(s), %zu object(s) queued, %zu restored)",
                                  filepath.c_str(), restored_scenes, queued, restored);
    return restored_scenes + queued + restored > 0;
}

std::vector<std::string> EditorScene::find_conflicting_scene_names(const std::string& filepath) const
{
    std::vector<std::string> conflicts;

    std::ifstream file(filepath);
    if (!file)
    {
        return conflicts;
    }
    nlohmann::json doc;
    try
    {
        file >> doc;
    }
    catch (const nlohmann::json::exception&)
    {
        return conflicts;
    }

    std::set<std::string> existing;
    for (pnanovdb_editor_token_t* token : m_scene_view.get_all_scene_tokens())
    {
        if (token && token->str)
        {
            existing.insert(token->str);
        }
    }

    std::set<std::string> seen;
    const auto consider = [&](const std::string& name)
    {
        if (name.empty() || existing.count(name) == 0 || !seen.insert(name).second)
        {
            return;
        }
        conflicts.push_back(name);
    };

    const auto scenes = doc.find("scenes");
    if (scenes != doc.end() && scenes->is_array())
    {
        for (const auto& scene_json : *scenes)
        {
            if (!scene_json.is_object())
            {
                continue;
            }
            const auto name_it = scene_json.find("name");
            if (name_it != scene_json.end() && name_it->is_string())
            {
                consider(name_it->get<std::string>());
            }
        }
    }

    const auto objects = doc.find("objects");
    if (objects != doc.end() && objects->is_array())
    {
        for (const auto& obj : *objects)
        {
            if (!obj.is_object())
            {
                continue;
            }
            const auto scene_it = obj.find("scene");
            if (scene_it != obj.end() && scene_it->is_string())
            {
                consider(scene_it->get<std::string>());
            }
        }
    }

    return conflicts;
}

void EditorScene::process_pending_restores()
{
    if (!m_active_scene_load && m_pending_scene_loads.empty())
    {
        return;
    }

    constexpr auto k_restore_timeout = std::chrono::seconds(60);
    const bool load_idle = pipeline_load_available();

    // Finish the active asynchronous import before starting the next one
    if (m_active_scene_load)
    {
        PendingSceneLoad& pending = *m_active_scene_load;

        pnanovdb_editor_token_t* import_scene =
            pending.started_scene_token ? pending.started_scene_token : pending.scene_token;
        const bool retargeted = import_scene && pending.scene_token && import_scene->id != pending.scene_token->id;

        const uint64_t imported_lifetime =
            pending.published_lifetime_id ? pending.published_lifetime_id : pending.started_lifetime_id;
        const bool import_exists =
            pending.published_lifetime_id != 0 &&
            m_scene_manager.object_lifetime(import_scene, pending.name_token) == pending.published_lifetime_id;

        if (load_idle)
        {
            if (pending.discard || retargeted)
            {
                const bool removed_exact_generation =
                    m_scene_manager.remove_if_lifetime(import_scene, pending.name_token, imported_lifetime);
                if (removed_exact_generation && pending.name_token && pending.name_token->str)
                {
                    remove_object(import_scene, pending.name_token->str);
                }
                if (pending.cleanup_started_scene)
                {
                    m_scene_view.remove_scene(import_scene);
                }
                if (retargeted && !pending.discard)
                {
                    pending.started_scene_token = nullptr;
                    pending.cleanup_started_scene = false;
                    pending.started_at = {};
                    m_pending_scene_loads.push_front(std::move(pending));
                }
            }
            else if (import_exists)
            {
                apply_object_restore(pending.scene_token, pending.name_token, pending.object_json);
            }
            else
            {
                Console::getInstance().addLog(
                    Console::LogLevel::Error, "Load scene: import failed for '%s' from '%s'; continuing scene restore",
                    pending.name_token && pending.name_token->str ? pending.name_token->str : "?",
                    pending.source_filepath.c_str());
            }
            m_active_scene_load.reset();
        }
        else if (pending.started_at != std::chrono::steady_clock::time_point{} &&
                 std::chrono::steady_clock::now() - pending.started_at >= k_restore_timeout && !pending.discard)
        {
            Console::getInstance().addLog(Console::LogLevel::Warning, "Load scene: timed out restoring state for '%s'",
                                          pending.name_token && pending.name_token->str ? pending.name_token->str : "?");

            const bool same_target = m_async_load_target && m_async_load_target->scene && m_async_load_target->name &&
                                     pending.scene_token && pending.name_token &&
                                     m_async_load_target->scene->id == pending.scene_token->id &&
                                     m_async_load_target->name->id == pending.name_token->id;
            if (same_target)
            {
                m_scene_manager.cancel_load_target(
                    m_async_load_target->scene, m_async_load_target->name, m_async_load_target->lifetime_id);
                m_async_load_target.reset();
            }
            pending.discard = true;
        }
    }

    if (m_active_scene_load || m_pending_scene_loads.empty() || !load_idle)
    {
        return;
    }

    PendingSceneLoad load = std::move(m_pending_scene_loads.front());
    m_pending_scene_loads.pop_front();
    load.started_scene_token = load.scene_token;

    bool name_conflict = false;
    m_scene_manager.with_object(
        load.scene_token, load.name_token, [&](SceneObject* existing) { name_conflict = existing != nullptr; });
    if (name_conflict)
    {
        Console::getInstance().addLog(Console::LogLevel::Error,
                                      "Load scene: object name '%s' is already in use in scene '%s'; skipping",
                                      load.name_token && load.name_token->str ? load.name_token->str : "?",
                                      load.scene_token && load.scene_token->str ? load.scene_token->str : "?");
        return;
    }

    bool kicked_off = false;
    switch (load.load_type)
    {
    case PendingSceneLoadType::NanoVDB:
        kicked_off = load_nanovdb_file(load.scene_token, load.source_filepath.c_str(), load.render_type, load.name_token);
        break;
    case PendingSceneLoadType::Gaussian:
        kicked_off = load_gaussian_file(load.scene_token, load.source_filepath.c_str(), load.process_type,
                                        load.render_type, load.voxels_per_unit, load.name_token);
        break;
    case PendingSceneLoadType::Mesh:
    {
        mesh_import::Options options;
        options.process_pipeline = load.process_type;
        kicked_off = load_mesh_file(load.scene_token, load.source_filepath.c_str(), options, load.name_token);
        break;
    }
    }

    const bool worker_backed =
        load.load_type == PendingSceneLoadType::Mesh ||
        (load.load_type == PendingSceneLoadType::Gaussian && load.process_type != pnanovdb_pipeline_type_voxelbvh_build);
    if (kicked_off && worker_backed)
    {
        load.started_at = std::chrono::steady_clock::now();
        if (m_async_load_target && m_async_load_target->scene && m_async_load_target->name &&
            m_async_load_target->scene->id == load.scene_token->id &&
            m_async_load_target->name->id == load.name_token->id)
        {
            load.started_lifetime_id = m_async_load_target->lifetime_id;
        }
        m_active_scene_load = std::move(load);
    }
    else if (kicked_off)
    {
        apply_object_restore(load.scene_token, load.name_token, load.object_json);
    }
    else
    {
        Console::getInstance().addLog(
            Console::LogLevel::Error, "Load scene: failed to start queued import for '%s' from '%s'",
            load.name_token && load.name_token->str ? load.name_token->str : "?", load.source_filepath.c_str());
    }
}

void EditorScene::apply_object_restore(pnanovdb_editor_token_t* scene_token,
                                       pnanovdb_editor_token_t* name_token,
                                       const std::string& object_json)
{
    if (!m_editor || !scene_token || !name_token)
    {
        return;
    }

    nlohmann::json obj;
    try
    {
        obj = nlohmann::json::parse(object_json);
    }
    catch (const nlohmann::json::exception& e)
    {
        Console::getInstance().addLog(Console::LogLevel::Warning, "Load scene: cannot parse restore state for '%s': %s",
                                      name_token->str ? name_token->str : "?", e.what());
        return;
    }

    try
    {
        auto apply_stage_params =
            [](pnanovdb_pipeline_params_t* params, pnanovdb_pipeline_type_t type, const nlohmann::json& stage_json)
        {
            if (!params || !params->data || !stage_json.contains("params") || !stage_json["params"].is_object())
            {
                return;
            }
            const pnanovdb_reflect_data_type_t* dt = params->type;
            if (!dt)
            {
                const pnanovdb_pipeline_descriptor_t* desc = pnanovdb_pipeline_get_descriptor(type);
                dt = desc ? desc->params_data_type : nullptr;
            }
            json_to_reflect_params(stage_json["params"], dt, params->data, (size_t)params->size);
            if (type == pnanovdb_pipeline_type_gaussian_voxelize)
            {
                pipeline_params_set_voxels_per_unit(params, pipeline_params_get_voxels_per_unit(params));
            }
        };

        if (obj.contains("pipeline") && obj["pipeline"].is_object())
        {
            const auto& pj = obj["pipeline"];

            if (pj.contains("load") && pj["load"].is_object())
            {
                pnanovdb_pipeline_type_t t = pnanovdb_pipeline_type_noop;
                if (decode_scene_pipeline_type(pj["load"], pnanovdb_pipeline_stage_load, t, name_token->str, "load"))
                {
                    m_editor->set_pipeline(m_editor, scene_token, name_token, pnanovdb_pipeline_stage_load, t);
                    pnanovdb_pipeline_params_t* p =
                        m_editor->map_pipeline_params(m_editor, scene_token, name_token, pnanovdb_pipeline_stage_load);
                    apply_stage_params(p, t, pj["load"]);
                    m_editor->unmap_pipeline_params(m_editor, scene_token, name_token, pnanovdb_pipeline_stage_load);
                }
            }

            if (pj.contains("render") && pj["render"].is_object())
            {
                pnanovdb_pipeline_type_t t = pnanovdb_pipeline_type_noop;
                if (decode_scene_pipeline_type(pj["render"], pnanovdb_pipeline_stage_render, t, name_token->str, "render"))
                {
                    m_editor->set_pipeline(m_editor, scene_token, name_token, pnanovdb_pipeline_stage_render, t);
                    pnanovdb_pipeline_params_t* p =
                        m_editor->map_pipeline_params(m_editor, scene_token, name_token, pnanovdb_pipeline_stage_render);
                    apply_stage_params(p, t, pj["render"]);
                    m_editor->unmap_pipeline_params(m_editor, scene_token, name_token, pnanovdb_pipeline_stage_render);
                }
            }

            if (pj.contains("process") && pj["process"].is_array())
            {
                const auto& steps = pj["process"];
                for (pnanovdb_uint32_t i = 0; i < steps.size(); ++i)
                {
                    if (!steps[i].is_object())
                    {
                        continue;
                    }
                    pnanovdb_pipeline_type_t t = pnanovdb_pipeline_type_noop;
                    if (decode_scene_pipeline_type(
                            steps[i], pnanovdb_pipeline_stage_process, t, name_token->str, "process"))
                    {
                        m_editor->set_process_step(m_editor, scene_token, name_token, i, t);
                        pnanovdb_pipeline_params_t* p =
                            m_editor->map_process_step_params(m_editor, scene_token, name_token, i);
                        apply_stage_params(p, t, steps[i]);
                        m_editor->unmap_process_step_params(m_editor, scene_token, name_token, i);
                    }
                }
            }

            if (pj.contains("drop_intermediate") && pj["drop_intermediate"].is_boolean())
            {
                const bool drop = pj["drop_intermediate"].get<bool>();
                m_scene_manager.with_object(scene_token, name_token,
                                            [drop](SceneObject* o)
                                            {
                                                if (o)
                                                    o->pipeline.drop_intermediate = drop;
                                            });
            }
        }

        const std::string shader_name = obj.value("shader_name", std::string());
        if (!shader_name.empty())
        {
            pnanovdb_editor_shader_name_t* mapped = (pnanovdb_editor_shader_name_t*)m_editor->map_params(
                m_editor, scene_token, name_token, PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_editor_shader_name_t));
            if (mapped)
            {
                mapped->shader_name = m_editor->get_token(shader_name.c_str());
                m_editor->unmap_params(m_editor, scene_token, name_token);
            }
        }

        if (!shader_name.empty() && obj.contains("shader_params") && obj["shader_params"].is_object() && m_compute)
        {
            std::vector<unsigned char> bytes;
            if (json_to_shader_params(m_scene_manager.shader_params, shader_name, obj["shader_params"], bytes) &&
                !bytes.empty())
            {
                pnanovdb_compute_array_t* array =
                    m_compute->create_array(sizeof(char), (pnanovdb_uint64_t)bytes.size(), bytes.data());
                if (array)
                {
                    m_scene_manager.set_params_array(scene_token, name_token, array, m_compute);
                }
            }
        }

        if (obj.contains("visible") && obj["visible"].is_boolean())
        {
            m_editor->set_visible(
                m_editor, scene_token, name_token, obj["visible"].get<bool>() ? PNANOVDB_TRUE : PNANOVDB_FALSE);
        }

        m_editor->mark_pipeline_dirty(m_editor, scene_token, name_token);
    }
    catch (const nlohmann::json::exception& e)
    {
        Console::getInstance().addLog(Console::LogLevel::Warning,
                                      "Load scene: invalid restore state for object '%s': %s; skipping remainder",
                                      name_token->str ? name_token->str : "?", e.what());
    }
}

} // namespace pnanovdb_editor
