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
        // Use scene object's params if available, otherwise fall back to default
        view_params = obj_shader_params ? obj_shader_params :
                                          (m_raster2d_params.default_array ?
                                               (pnanovdb_raster_shader_params_t*)m_raster2d_params.default_array->data :
                                               nullptr);
    }
    else if (obj_type == SceneObjectType::NanoVDB)
    {
        params = &m_nanovdb_params;
        // Use scene object's params if available, otherwise fall back to default
        view_params = obj_shader_params ?
                          obj_shader_params :
                          (m_nanovdb_params.default_array ? m_nanovdb_params.default_array->data : nullptr);
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

    // Sync the editor's camera from the current scene's viewport camera
    sync_editor_camera_from_scene();
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
                extern void execute_removal(pnanovdb_editor_t*, pnanovdb_editor_token_t*, pnanovdb_editor_token_t*);
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
                            "[API] Successfully removed scene '%s' from SceneView on render thread", removal.scene->str);
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

void EditorScene::init_default_camera_for_scene(pnanovdb_editor_token_t* scene_token)
{
    if (!scene_token)
    {
        return;
    }

    // Get or create the scene - this will set up the default camera automatically
    SceneViewData* scene = m_scene_view.get_or_create_scene(scene_token);
    if (!scene)
    {
        return;
    }

    // The scene already has default_camera_view set up in get_or_create_scene()
    // which points to scene->default_camera_config and scene->default_camera_state
    // These pointers are stable since they're embedded in the SceneViewData object
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
    if (m_scene_view.remove_camera(scene_token, name_token))
    {
        Console::getInstance().addLog("Removed camera '%s' from scene", name);
        removed = true;
    }
    else if (m_scene_view.remove_nanovdb(scene_token, name_token))
    {
        Console::getInstance().addLog("Removed NanoVDB '%s' from scene", name);
        removed = true;
    }
    else if (m_scene_view.remove_gaussian(scene_token, name_token))
    {
        Console::getInstance().addLog("Removed Gaussian data '%s' from scene", name);
        removed = true;
    }

    if (removed)
    {
        // Clear selection if the removed object is currently selected
        if (m_view_selection.name_token && m_view_selection.name_token->str &&
            strcmp(m_view_selection.name_token->str, name) == 0)
        {
            // Check if selection belongs to the same scene
            if (!m_view_selection.scene_token || !scene_token || m_view_selection.scene_token->id == scene_token->id)
            {
                m_view_selection.type = ViewType::None;
                m_view_selection.name_token = nullptr;
                m_view_selection.scene_token = nullptr;
                Console::getInstance().addLog("Cleared selection (removed object was selected)");
            }
        }

        // Clear render view if the removed object is currently rendered
        if (m_render_view_selection.name_token && m_render_view_selection.name_token->str &&
            strcmp(m_render_view_selection.name_token->str, name) == 0)
        {
            if (!m_render_view_selection.scene_token || !scene_token ||
                m_render_view_selection.scene_token->id == scene_token->id)
            {
                m_render_view_selection.type = ViewType::None;
                m_render_view_selection.name_token = nullptr;
                m_render_view_selection.scene_token = nullptr;

                // Note: Renderer state will be cleared automatically on next frame
                // when sync_selected_view_with_current() runs
                Console::getInstance().addLog("Cleared render view selection (renderer will update next frame)");
            }
        }

        // If current view was removed, try to switch to another view
        pnanovdb_editor_token_t* current_view_token = m_scene_view.get_current_view(scene_token);
        if (current_view_token && current_view_token->id == name_token->id)
        {
            // Try to find another view to switch to
            const auto& nanovdbs = m_scene_view.get_nanovdbs();
            const auto& gaussians = m_scene_view.get_gaussians();

            if (!nanovdbs.empty())
            {
                pnanovdb_editor_token_t* new_view_token =
                    EditorToken::getInstance().getTokenById(nanovdbs.begin()->first);
                m_scene_view.set_current_view(scene_token, new_view_token);
                Console::getInstance().addLog("Switched view to '%s'", new_view_token ? new_view_token->str : "unknown");
            }
            else if (!gaussians.empty())
            {
                pnanovdb_editor_token_t* new_view_token =
                    EditorToken::getInstance().getTokenById(gaussians.begin()->first);
                m_scene_view.set_current_view(scene_token, new_view_token);
                Console::getInstance().addLog("Switched view to '%s'", new_view_token ? new_view_token->str : "unknown");
            }
            else
            {
                m_scene_view.set_current_view(scene_token, nullptr);
                Console::getInstance().addLog("No views remaining in scene");
            }
        }
    }

    return removed;
}

void EditorScene::sync_default_camera_view()
{
    // Update default camera view to sync with viewport camera to preserve changes when switching views
    m_default_camera_view_state = m_imgui_settings->camera_state;
    m_default_camera_view_config = m_imgui_settings->camera_config;
    m_imgui_instance->default_camera_view.states = &m_default_camera_view_state;
    m_imgui_instance->default_camera_view.configs = &m_default_camera_view_config;
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

void EditorScene::set_current_scene(pnanovdb_editor_token_t* scene_token)
{
    pnanovdb_editor_token_t* old_scene = m_scene_view.get_current_scene_token();
    m_scene_view.set_current_scene(scene_token);

    // Ensure this scene has a default camera
    if (scene_token)
    {
        init_default_camera_for_scene(scene_token);
    }

    // Clear selection if it belongs to a different scene
    if (m_view_selection.scene_token)
    {
        // Compare scene tokens to see if we're switching to a different scene
        bool switching_scenes = false;

        if (!scene_token && old_scene)
        {
            // Switching from a scene to null (shouldn't happen normally)
            switching_scenes = (m_view_selection.scene_token->id != 0);
        }
        else if (scene_token && !old_scene)
        {
            // Switching from null to a scene
            switching_scenes = true;
        }
        else if (scene_token && old_scene && scene_token->id != old_scene->id)
        {
            // Switching between different scenes
            switching_scenes = true;
        }

        // If switching to a different scene and selection belongs to the old scene, clear it
        if (switching_scenes && m_view_selection.scene_token->id != (scene_token ? scene_token->id : 0))
        {
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
        pnanovdb_editor_token_t* scene_token = get_current_scene_token();
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
    // Handle empty scene - clear the display
    if (!name_token)
    {
        clear_editor_view_state();
        m_render_view_selection = SceneSelection();
        return;
    }

    if (!is_selection_valid(SceneSelection{ type, name_token, scene_token }))
    {
        clear_editor_view_state();
        m_render_view_selection = SceneSelection();
        return;
    }
    if (type != ViewType::NanoVDBs && type != ViewType::GaussianScenes)
    {
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

void EditorScene::update_view_camera_state(pnanovdb_editor_token_t* view_name_token, const pnanovdb_camera_state_t& state)
{
    if (view_name_token)
    {
        m_scene_view_camera_state[view_name_token->id] = state;
    }
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
