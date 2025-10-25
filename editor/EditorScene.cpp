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
#include "EditorSceneManager.h"
#include "EditorToken.h"
#include "ShaderMonitor.h"
#include "ShaderCompileUtils.h"
#include "Console.h"

#include "raster/Raster.h"

#include <nanovdb/io/IO.h>

#include <cstddef>
#include <filesystem>

namespace pnanovdb_editor
{

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
        m_raster2d_params.shader_name = pnanovdb_editor::s_raster2d_shader_group;
        m_raster2d_params.size = m_raster_shader_params_data_type->element_size;
        m_raster2d_params.default_array = m_scene_manager.create_initialized_shader_params(
            m_compute, nullptr, pnanovdb_editor::s_raster2d_shader_group,
            m_raster_shader_params_data_type->element_size, m_raster_shader_params_data_type);
    }

    // Setup shader monitoring
    ShaderCallback callback =
        pnanovdb_editor::get_shader_recompile_callback(m_imgui_instance, m_editor->impl->compiler, config.compiler_inst);
    monitor_shader_dir(pnanovdb_shader::getShaderDir().c_str(), callback);
}

EditorScene::~EditorScene()
{
    // Destroy shader params arrays (destroy_array handles nullptr safely)
    // Note: current_array might be the same as default_array, avoid double-free
    if (m_nanovdb_params.current_array && m_nanovdb_params.current_array != m_nanovdb_params.default_array)
    {
        m_compute->destroy_array(m_nanovdb_params.current_array);
    }
    m_compute->destroy_array(m_nanovdb_params.default_array);

    if (m_raster2d_params.current_array && m_raster2d_params.current_array != m_raster2d_params.default_array)
    {
        m_compute->destroy_array(m_raster2d_params.current_array);
    }
    m_compute->destroy_array(m_raster2d_params.default_array);
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
    SceneObjectType obj_type = SceneObjectType::NanoVDB;
    void* obj_shader_params = nullptr;
    std::string obj_shader_name;

    m_scene_manager.for_each_object(
        [&](SceneObject* scene_obj)
        {
            if (!scene_obj)
                return true;

            // Check if this is the render view object
            if (!m_render_view_selection.is_valid())
                return false; // Stop iteration

            pnanovdb_editor_token_t* scene_token = get_current_scene_token();
            if (scene_obj->scene_token == scene_token && scene_obj->name_token == m_render_view_selection.name_token)
            {
                // Found it - copy the fields we need
                obj_type = scene_obj->type;
                obj_shader_params = scene_obj->shader_params;
                obj_shader_name = scene_obj->shader_name;
                return false; // Stop iteration
            }
            return true; // Continue iteration
        });

    // Now process with the copied data (no longer holding mutex)
    if (obj_shader_params || !obj_shader_name.empty())
    {
        copy_shader_params(obj_type, obj_shader_params, obj_shader_name, sync_direction);
    }
}

void EditorScene::clear_editor_view_state()
{
    // Clear all view-related pointers before switching to a new view
    m_editor->impl->nanovdb_array = nullptr;
    m_editor->impl->gaussian_data = nullptr;
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
    if (!params || !view_params)
    {
        return;
    }

    auto* old_array = params->current_array;
    pnanovdb_compute_array_t* new_array =
        m_scene_manager.shader_params.get_compute_array_for_shader(params->shader_name, m_compute);

    if (!new_array || !new_array->data)
    {
        return;
    }

    // If view_params points to the same buffer as the old array, update in place
    if (old_array && view_params == old_array->data)
    {
        std::memcpy(old_array->data, new_array->data, params->size);
        m_compute->destroy_array(new_array);
        return;
    }

    // Otherwise, switch to the new array and copy to the external target
    auto* to_destroy = old_array;
    params->current_array = new_array;
    std::memcpy(view_params, new_array->data, params->size);

    if (to_destroy && to_destroy != params->default_array)
    {
        m_compute->destroy_array(to_destroy);
        // Note: to_destroy is a copy of old_array, so we don't need to set it to nullptr
    }
}

void EditorScene::copy_ui_shader_params_from_to_editor(SceneShaderParams* params)
{
    if (params && params->current_array)
    {
        std::memcpy(m_editor->impl->shader_params, params->current_array->data, params->size);
    }
}

void EditorScene::copy_shader_params(SceneObjectType obj_type,
                                     void* obj_shader_params,
                                     const std::string& obj_shader_name,
                                     SyncDirection sync_direction,
                                     void** view_params_out)
{
    SceneShaderParams* params = nullptr;
    void* view_params = nullptr;

    if (obj_type == SceneObjectType::GaussianData)
    {
        params = &m_raster2d_params;
        view_params = get_view_params_with_fallback(m_raster2d_params, obj_shader_params);
    }
    else if (obj_type == SceneObjectType::NanoVDB)
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
    SceneObjectType obj_type = scene_obj->type;
    void* obj_shader_params = scene_obj->shader_params;
    std::string obj_shader_name = scene_obj->shader_name;

    if (obj_type == SceneObjectType::GaussianData)
    {
        m_editor->impl->gaussian_data = scene_obj->gaussian_data;
        m_editor->impl->shader_params = obj_shader_params;
        m_editor->impl->shader_params_data_type = m_raster_shader_params_data_type;
    }
    else if (obj_type == SceneObjectType::NanoVDB)
    {
        m_editor->impl->nanovdb_array = scene_obj->nanovdb_array;
        m_editor->impl->shader_params = obj_shader_params;
        m_editor->impl->shader_params_data_type = nullptr;
    }

    copy_shader_params(obj_type, obj_shader_params, obj_shader_name, SyncDirection::EditorToUI);

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
        std::lock_guard<std::mutex> lock(m_editor->impl->editor_worker->shader_params_mutex);
        void* old_shader_params = nullptr;
        worker->pending_shader_params.process_pending(m_editor->impl->shader_params, old_shader_params);

        const pnanovdb_reflect_data_type_t* old_shader_params_data_type = nullptr;
        worker->pending_shader_params_data_type.process_pending(
            m_editor->impl->shader_params_data_type, old_shader_params_data_type);
    }

    // Process pending removals (queued by worker thread)
    // This is done on the render thread to ensure views/renderer aren't using the data
    {
        std::vector<pnanovdb_editor::PendingRemoval> removals_to_process;
        {
            std::lock_guard<std::mutex> lock(worker->pending_removals_mutex);
            removals_to_process = std::move(worker->pending_removals);
            worker->pending_removals.clear();
        }

        for (const auto& removal : removals_to_process)
        {
            if (!removal.scene)
            {
                Console::getInstance().addLog("[ERROR] Invalid removal request: scene is nullptr!");
                continue;
            }

            if (removal.name)
            {
                // Execute object removal on render thread (safe - no UAF)
                Console::getInstance().addLog("[Removal] Processing object removal: scene='%s', name='%s'",
                                              removal.scene->str, removal.name->str);
                execute_removal(m_editor, removal.scene, removal.name);
            }
            else
            {
                // name == nullptr signals full scene removal (after all objects were removed)
                Console::getInstance().addLog("[Removal] Processing scene removal: scene='%s'", removal.scene->str);
                if (m_editor->impl->scene_view)
                {
                    bool scene_removed = m_editor->impl->scene_view->remove_scene(removal.scene);
                    if (scene_removed)
                    {
                        Console::getInstance().addLog(
                            "[API] Removed scene '%s' from SceneView on render thread", removal.scene->str);
                    }
                    else
                    {
                        Console::getInstance().addLog("[API] Scene '%s' was not found in SceneView", removal.scene->str);
                    }
                }
            }
        }
    }

    // Sync views from scene_manager if signaled (worker thread modified scene_manager)
    if (worker->views_need_sync.exchange(false, std::memory_order_acq_rel))
    {
        sync_views_from_scene_manager();
    }
}

void EditorScene::process_pending_ui_changes()
{
    // update pending GUI states
    if (m_imgui_instance->pending.load_nvdb)
    {
        m_imgui_instance->pending.load_nvdb = false;
        load_nanovdb_to_editor();
    }
    if (m_imgui_instance->pending.save_nanovdb)
    {
        m_imgui_instance->pending.save_nanovdb = false;
        save_editor_nanovdb();
    }
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
    const uint64_t prev_epoch = get_current_view_epoch();
    bool has_pending_request = handle_pending_view_changes();


    pnanovdb_editor_token_t* view_token = m_scene_view.get_current_view();

    // If a camera is selected and no explicit request was made, keep camera selection ONLY
    // when the content view hasn't changed (by token ID and by epoch) this frame. This ensures
    // views created earlier (via API/worker) can still grab selection.
    uint64_t prev_id = prev_view_token ? prev_view_token->id : 0;
    uint64_t curr_id = view_token ? view_token->id : 0;
    if (!has_pending_request && m_view_selection.type == ViewType::Cameras && prev_id == curr_id &&
        prev_epoch == get_current_view_epoch())
    {
        return;
    }

    // Check if we have a valid view token
    if (!view_token)
    {
        // No views in scene - clear the display
        clear_selection();
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
        prev_epoch == get_current_view_epoch())
    {
        return;
    }

    set_properties_selection(new_view_type, view_token, current_scene_token);
    set_render_view(new_view_type, view_token, current_scene_token);
}

void EditorScene::sync_shader_params_from_editor()
{
    if (m_editor->impl->editor_worker)
    {
        // Only lock if params are dirty
        if (m_editor->impl->editor_worker->params_dirty.load())
        {
            std::lock_guard<std::mutex> lock(m_editor->impl->editor_worker->shader_params_mutex);

            // Check again after acquiring lock (double-check pattern)
            if (m_editor->impl->editor_worker->params_dirty.exchange(false))
            {
                // Sync editor params to UI
                sync_current_view_state(SyncDirection::EditorToUI);
            }
        }
    }
    else
    {
        sync_current_view_state(SyncDirection::UiToView);

        // Clear editor params pointer to mark UI as source of truth
        m_editor->impl->shader_params = nullptr;
        m_editor->impl->shader_params_data_type = nullptr;
    }
}

void EditorScene::sync_views_from_scene_manager()
{
    // This function syncs SceneView from EditorSceneManager state
    // Called from render thread when worker thread signals views_need_sync
    // Ensures thread-safe updates by only touching views from render thread

    if (!m_editor->impl->scene_manager)
    {
        return;
    }

    // Get the last added view tokens if we're in worker mode
    pnanovdb_editor_token_t* last_added_scene_token = nullptr;
    pnanovdb_editor_token_t* last_added_name_token = nullptr;

    if (m_editor->impl->editor_worker)
    {
        uint64_t scene_id = m_editor->impl->editor_worker->last_added_scene_token_id.load(std::memory_order_relaxed);
        uint64_t name_id = m_editor->impl->editor_worker->last_added_name_token_id.load(std::memory_order_relaxed);

        if (scene_id != 0 && name_id != 0)
        {
            last_added_scene_token = EditorToken::getInstance().getTokenById(scene_id);
            last_added_name_token = EditorToken::getInstance().getTokenById(name_id);
        }
    }

    // Iterate over all objects while holding the scene_manager mutex
    // This prevents race conditions where objects might be removed by worker thread during iteration
    m_scene_manager.for_each_object(
        [this](SceneObject* obj)
        {
            if (!obj || !obj->scene_token || !obj->name_token || !obj->name_token->str)
            {
                return true; // Continue iteration
            }

            switch (obj->type)
            {
            case SceneObjectType::NanoVDB:
                if (obj->nanovdb_array)
                {
                    NanoVDBContext ctx{ obj->nanovdb_array, obj->shader_params };
                    m_scene_view.add_nanovdb(obj->scene_token, obj->name_token, ctx);
                }
                break;

            case SceneObjectType::GaussianData:
                if (obj->gaussian_data && obj->shader_params)
                {
                    GaussianDataContext ctx{ obj->gaussian_data, (pnanovdb_raster_shader_params_t*)obj->shader_params };
                    m_scene_view.add_gaussian(obj->scene_token, obj->name_token, ctx);
                }
                break;

            case SceneObjectType::Camera:
                if (obj->camera_view)
                {
                    m_scene_view.add_camera(obj->scene_token, obj->name_token, obj->camera_view);
                }
                break;

            default:
                break;
            }

            return true; // Continue iteration
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

        // Clear the token IDs so we don't re-select on future syncs
        if (m_editor->impl->editor_worker)
        {
            m_editor->impl->editor_worker->last_added_scene_token_id.store(0, std::memory_order_relaxed);
            m_editor->impl->editor_worker->last_added_name_token_id.store(0, std::memory_order_relaxed);
        }
    }
}

void EditorScene::reload_shader_params_for_current_view()
{
    if (m_nanovdb_params.default_array)
    {
        m_compute->destroy_array(m_nanovdb_params.default_array);
        m_nanovdb_params.default_array = nullptr; // Prevent double-free
    }

    m_nanovdb_params.shader_name = m_imgui_instance->shader_name;
    m_nanovdb_params.default_array = m_scene_manager.create_initialized_shader_params(
        m_compute, m_nanovdb_params.shader_name.c_str(), nullptr, PNANOVDB_COMPUTE_CONSTANT_BUFFER_MAX_SIZE);
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

    pnanovdb_editor_token_t* viewport_token = m_scene_view.get_viewport_camera_token();
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
        if (!m_editor->impl->camera)
        {
            m_editor->impl->camera = new pnanovdb_camera_t();
            pnanovdb_camera_init(m_editor->impl->camera);
        }

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

    pnanovdb_editor_token_t* viewport_token = m_scene_view.get_viewport_camera_token();
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
    if (!shader_params_data)
    {
        return;
    }

    // Use with_object() to safely copy fields while holding mutex
    if (!m_render_view_selection.is_valid() || !m_editor->impl->scene_manager)
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

            // Copy fields while holding mutex to avoid UAF
            SceneObjectType obj_type = scene_obj->type;
            void* obj_shader_params = scene_obj->shader_params;
            std::string obj_shader_name = scene_obj->shader_name;

            void* view_params = nullptr;
            size_t copy_size =
                (obj_type == SceneObjectType::GaussianData) ? m_raster2d_params.size : m_nanovdb_params.size;
            copy_shader_params(obj_type, obj_shader_params, obj_shader_name, SyncDirection::UiToView, &view_params);

            if (view_params && copy_size > 0)
            {
                std::memcpy(shader_params_data, view_params, copy_size);
            }
        });
}

void EditorScene::handle_nanovdb_data_load(pnanovdb_compute_array_t* nanovdb_array, const char* filename)
{
    if (!filename)
    {
        return;
    }

    nanovdb_array = m_editor->impl->compute->load_nanovdb(filename);
    if (!nanovdb_array)
    {
        return;
    }

    std::filesystem::path fsPath(filename);
    std::string view_name = fsPath.stem().string();

    // Store in SceneManager as the single source of truth
    pnanovdb_editor_token_t* scene_token = get_current_scene_token();
    pnanovdb_editor_token_t* name_token = EditorToken::getInstance().getToken(view_name.c_str());

    // Create params array initialized from JSON (like m_nanovdb_params.default_array)
    pnanovdb_compute_array_t* params_array = m_scene_manager.create_initialized_shader_params(
        m_compute, m_nanovdb_params.shader_name.c_str(), nullptr, PNANOVDB_COMPUTE_CONSTANT_BUFFER_MAX_SIZE);

    m_scene_manager.add_nanovdb(
        scene_token, name_token, nanovdb_array, params_array, m_compute, m_nanovdb_params.shader_name.c_str());

    // Register in SceneView (for scene tree display)
    m_scene_view.add_nanovdb_to_scene(
        scene_token, name_token, nanovdb_array, params_array ? params_array->data : nullptr);

    // Select the newly loaded rasterized view
    m_scene_view.set_current_view(scene_token, name_token);
}

void EditorScene::handle_gaussian_data_load(pnanovdb_raster_gaussian_data_t* gaussian_data,
                                            pnanovdb_raster_shader_params_t* raster_params,
                                            const char* filename,
                                            std::shared_ptr<pnanovdb_raster_gaussian_data_t>& old_gaussian_data_ptr)
{
    if (!filename)
    {
        return;
    }

    std::filesystem::path fsPath(filename);
    std::string view_name = fsPath.stem().string();

    // Store in SceneManager as the single source of truth
    pnanovdb_editor_token_t* scene_token = get_current_scene_token();
    pnanovdb_editor_token_t* name_token = EditorToken::getInstance().getToken(view_name.c_str());

    // Create params array initialized from JSON (like m_raster2d_params.default_array)
    pnanovdb_compute_array_t* params_array = m_scene_manager.create_initialized_shader_params(
        m_compute, nullptr, pnanovdb_editor::s_raster2d_shader_group, 0, m_raster_shader_params_data_type);

    // Add with deferred destruction handling
    std::shared_ptr<pnanovdb_raster_gaussian_data_t> old_owner;
    m_scene_manager.add_gaussian_data(scene_token, name_token, gaussian_data, params_array,
                                      m_raster_shader_params_data_type, m_compute, m_editor->impl->raster,
                                      m_device_queue, pnanovdb_editor::s_raster2d_gaussian_shader, &old_owner);

    // Chain old data through gaussian_data_old for deferred destruction
    if (old_owner)
    {
        if (m_editor->impl->gaussian_data_old)
        {
            // If gaussian_data_old already has data, add it to pending queue first
            m_editor->impl->gaussian_data_destruction_queue_pending.push_back(
                std::move(m_editor->impl->gaussian_data_old));
        }
        m_editor->impl->gaussian_data_old = std::move(old_owner);
    }

    // Register in SceneView (for scene tree display)
    m_scene_view.add_gaussian_to_scene(
        scene_token, name_token, gaussian_data,
        static_cast<pnanovdb_raster_shader_params_t*>(params_array ? params_array->data : nullptr));

    // Select the newly loaded rasterized view
    m_scene_view.set_current_view(scene_token, name_token);
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


const std::map<uint64_t, pnanovdb_camera_view_t*>& EditorScene::get_camera_views() const
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
    // Check both existence AND that the object type matches the ViewType
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

            // Check that the object type matches the requested ViewType
            if (selection.type == ViewType::NanoVDBs && obj->type == SceneObjectType::NanoVDB)
            {
                is_valid = true;
            }
            else if (selection.type == ViewType::GaussianScenes && obj->type == SceneObjectType::GaussianData)
            {
                is_valid = true;
            }
        });
    return is_valid;
}

// ============================================================================
// Scene Selection Management
// ============================================================================

// update_selection() removed; logic migrated into set_properties_selection()

// Scene management
EditorSceneManager* EditorScene::get_scene_manager() const
{
    return &m_scene_manager;
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
    m_editor->impl->shader_name = s_default_editor_shader;
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

        // Update shader_name to reflect the selected object's shader
        // Use with_object() to safely access shader_name while holding mutex
        m_scene_manager.with_object(scene_token, name_token,
                                    [&](SceneObject* obj)
                                    {
                                        if (obj && !obj->shader_name.empty())
                                        {
                                            m_editor->impl->shader_name = obj->shader_name;
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
    // Validate selection (handles null name_token, invalid selection, and type checking)
    if (!name_token || !is_selection_valid(SceneSelection{ type, name_token, scene_token }) ||
        (type != ViewType::NanoVDBs && type != ViewType::GaussianScenes))
    {
        clear_editor_view_state();
        m_render_view_selection = SceneSelection();
        return;
    }

    m_render_view_selection = { type, name_token, scene_token };

    // Load into editor and UI so renderer switches to the requested view
    // Use with_object() to safely copy data while holding mutex
    m_scene_manager.with_object(
        scene_token, name_token,
        [&](SceneObject* scene_obj)
        {
            if (scene_obj)
            {
                load_view_into_editor_and_ui(scene_obj);
                if (type == ViewType::NanoVDBs)
                {
                    m_imgui_instance->viewport_option = imgui_instance_user::ViewportOption::NanoVDB;
                }
                else if (type == ViewType::GaussianScenes)
                {
                    m_imgui_instance->viewport_option = imgui_instance_user::ViewportOption::Raster2D;
                }
            }
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
    handle_nanovdb_data_load(m_editor->impl->nanovdb_array, m_imgui_instance->nanovdb_filepath.c_str());
}

void EditorScene::save_editor_nanovdb()
{
    if (!m_editor || !m_editor->impl || !m_editor->impl->nanovdb_array)
    {
        pnanovdb_editor::Console::getInstance().addLog("Error: No NanoVDB array to save");
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

} // namespace pnanovdb_editor
