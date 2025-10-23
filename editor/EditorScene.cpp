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
const char* s_raster2d_shader_name = "raster/gaussian_rasterize_2d.slang";

EditorScene::EditorScene(const EditorSceneConfig& config)
    : m_imgui_instance(config.imgui_instance),
      m_editor(config.editor),
      m_views(config.editor->impl->views),
      m_compute(config.editor->impl->compute),
      m_imgui_settings(config.imgui_settings),
      m_device_queue(config.device_queue)
{
    // Setup views UI - ImguiInstance accesses views through EditorScene
    m_imgui_instance->editor_scene = this;

    // Initialize view registry (maps ViewsTypes to actual map pointers)
    initialize_view_registry();

    // Initialize NanoVDB shader params arrays with defaults
    {
        m_nanovdb_params.shader_name = config.default_shader_name;
        m_nanovdb_params.size = PNANOVDB_COMPUTE_CONSTANT_BUFFER_MAX_SIZE;
        m_nanovdb_params.default_array = get_scene_manager()->create_initialized_shader_params(
            m_compute, config.default_shader_name, nullptr, PNANOVDB_COMPUTE_CONSTANT_BUFFER_MAX_SIZE);
    }

    // Initialize raster shader params arrays with defaults
    {
        m_raster_shader_params_data_type = PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_raster_shader_params_t);
        m_raster2d_params.shader_name = s_raster2d_shader_name;
        m_raster2d_params.size = m_raster_shader_params_data_type->element_size;
        m_raster2d_params.default_array = get_scene_manager()->create_initialized_shader_params(
            m_compute, nullptr, imgui_instance_user::s_raster2d_shader_group,
            m_raster_shader_params_data_type->element_size, m_raster_shader_params_data_type);
    }

    // Initialize default camera view that syncs with viewport
    if (m_editor->impl->camera)
    {
        m_default_camera_view_config = m_editor->impl->camera->config;
        m_default_camera_view_state = m_editor->impl->camera->state;
    }
    else
    {
        pnanovdb_camera_config_default(&m_default_camera_view_config);
        pnanovdb_bool_t is_y_up = (m_imgui_settings && m_imgui_settings->is_y_up) ? PNANOVDB_TRUE : PNANOVDB_FALSE;
        pnanovdb_camera_state_default(&m_default_camera_view_state, is_y_up);
    }

    pnanovdb_camera_view_default(&m_imgui_instance->default_camera_view);
    m_imgui_instance->default_camera_view.name =
        EditorToken::getInstance().getToken(imgui_instance_user::VIEWPORT_CAMERA);
    m_imgui_instance->default_camera_view.configs = &m_default_camera_view_config;
    m_imgui_instance->default_camera_view.states = &m_default_camera_view_state;
    m_imgui_instance->default_camera_view.num_cameras = 1;
    m_imgui_instance->default_camera_view.is_visible = PNANOVDB_FALSE;

    // Add default viewport camera
    m_views->add_camera(imgui_instance_user::VIEWPORT_CAMERA, &m_imgui_instance->default_camera_view);

    // Setup shader monitoring
    ShaderCallback callback =
        pnanovdb_editor::get_shader_recompile_callback(m_imgui_instance, m_editor->impl->compiler, config.compiler_inst);
    monitor_shader_dir(pnanovdb_shader::getShaderDir().c_str(), callback);
}

EditorScene::~EditorScene()
{
    // Destroy shader params arrays
    m_compute->destroy_array(m_nanovdb_params.current_array);
    m_compute->destroy_array(m_nanovdb_params.default_array);

    m_compute->destroy_array(m_raster2d_params.current_array);
    m_compute->destroy_array(m_raster2d_params.default_array);
}

SceneObject* EditorScene::get_scene_object(pnanovdb_editor_token_t* view_name_token, ViewType view_type) const
{
    if (!view_name_token)
    {
        return nullptr;
    }

    auto* scene_manager = get_scene_manager();
    if (!scene_manager)
    {
        return nullptr;
    }

    pnanovdb_editor_token_t* scene_token = get_current_scene_token();
    return scene_manager->get(scene_token, view_name_token);
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
    auto* scene_obj = get_render_scene_object();
    if (!scene_obj)
    {
        return;
    }

    copy_shader_params(scene_obj, sync_direction);
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
    if (auto* scene_manager = get_scene_manager())
    {
        scene_manager->shader_params.set_compute_array_for_shader(params->shader_name, params->current_array);
    }
}

void EditorScene::copy_shader_params_from_ui_to_view(SceneShaderParams* params, void* view_params)
{
    if (!params || !view_params)
    {
        return;
    }

    auto* old_array = params->current_array;
    pnanovdb_compute_array_t* new_array = nullptr;
    if (auto* scene_manager = get_scene_manager())
    {
        new_array = scene_manager->shader_params.get_compute_array_for_shader(params->shader_name, m_compute);
    }

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
    }
}

void EditorScene::copy_ui_shader_params_from_to_editor(SceneShaderParams* params)
{
    if (params && params->current_array)
    {
        std::memcpy(m_editor->impl->shader_params, params->current_array->data, params->size);
    }
}

void EditorScene::copy_shader_params(SceneObject* scene_obj, SyncDirection sync_direction, void** view_params_out)
{
    if (!scene_obj)
    {
        return;
    }

    SceneShaderParams* params = nullptr;
    void* view_params = nullptr;

    if (scene_obj->type == SceneObjectType::GaussianData)
    {
        params = &m_raster2d_params;
        if (!scene_obj->shader_params)
        {
            scene_obj->shader_params = m_raster2d_params.default_array ?
                                           (pnanovdb_raster_shader_params_t*)m_raster2d_params.default_array->data :
                                           nullptr;
        }
        view_params = scene_obj->shader_params;
    }
    else if (scene_obj->type == SceneObjectType::NanoVDB)
    {
        params = &m_nanovdb_params;
        if (!scene_obj->shader_params)
        {
            scene_obj->shader_params = m_nanovdb_params.default_array ? m_nanovdb_params.default_array->data : nullptr;
        }
        view_params = scene_obj->shader_params;
    }

    if (params && view_params)
    {
        params->shader_name = scene_obj->shader_name;

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
    if (scene_obj->type == SceneObjectType::GaussianData)
    {
        m_editor->impl->gaussian_data = scene_obj->gaussian_data;
        m_editor->impl->shader_params = scene_obj->shader_params;
        m_editor->impl->shader_params_data_type = m_raster_shader_params_data_type;
    }
    else if (scene_obj->type == SceneObjectType::NanoVDB)
    {
        m_editor->impl->nanovdb_array = scene_obj->nanovdb_array;
        m_editor->impl->shader_params = scene_obj->shader_params;
        m_editor->impl->shader_params_data_type = nullptr;
    }

    copy_shader_params(scene_obj, SyncDirection::EditorToUI);
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
        m_views->set_current_view(pending_view_name);
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

    pnanovdb_compute_array_t* old_nanovdb_array = nullptr;
    updated = worker->pending_nanovdb.process_pending(m_editor->impl->nanovdb_array, old_nanovdb_array);

    pnanovdb_compute_array_t* old_array = nullptr;
    worker->pending_data_array.process_pending(m_editor->impl->data_array, old_array);

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

    void* old_shader_params = nullptr;
    worker->pending_shader_params.process_pending(m_editor->impl->shader_params, old_shader_params);

    // Lock mutex when updating shader_params pointer to protect from concurrent viewer access
    {
        std::lock_guard<std::mutex> lock(m_editor->impl->editor_worker->shader_params_mutex);
        const pnanovdb_reflect_data_type_t* old_shader_params_data_type = nullptr;
        worker->pending_shader_params_data_type.process_pending(
            m_editor->impl->shader_params_data_type, old_shader_params_data_type);
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

void EditorScene::sync_selected_view_with_current()
{
    const std::string prev_view = m_views->get_current_view();
    const uint64_t prev_epoch = get_current_view_epoch();

    bool has_pending_request = handle_pending_view_changes();

    const std::string& view_current = m_views->get_current_view();

    // If a camera is selected and no explicit request was made, keep camera selection ONLY
    // when the content view hasn't changed (by name and by epoch) this frame. This ensures
    // views created earlier (via API/worker) can still grab selection.
    if (!has_pending_request && m_view_selection.type == ViewType::Cameras && view_current == prev_view &&
        prev_epoch == get_current_view_epoch())
    {
        return;
    }

    // Convert view_current string to token for comparison
    if (view_current.empty())
    {
        return;
    }

    pnanovdb_editor_token_t* view_token = pnanovdb_editor::EditorToken::getInstance().getToken(view_current.c_str());

    // Compare tokens directly (more efficient than string comparison)
    if (m_view_selection.name_token && m_view_selection.name_token->id == view_token->id)
    {
        return;
    }

    // Get current scene token for validation
    pnanovdb_editor_token_t* current_scene_token = get_current_scene_token();

    ViewType new_view_type = ViewType::None;
    if (is_selection_valid(SceneSelection{ ViewType::NanoVDBs, view_token, current_scene_token }))
    {
        new_view_type = ViewType::NanoVDBs;
    }
    else if (is_selection_valid(SceneSelection{ ViewType::GaussianScenes, view_token, current_scene_token }))
    {
        new_view_type = ViewType::GaussianScenes;
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

void EditorScene::reload_shader_params_for_current_view()
{
    if (m_nanovdb_params.default_array)
    {
        m_compute->destroy_array(m_nanovdb_params.default_array);
    }

    m_nanovdb_params.shader_name = m_imgui_instance->shader_name;
    m_nanovdb_params.default_array = get_scene_manager()->create_initialized_shader_params(
        m_compute, m_nanovdb_params.shader_name.c_str(), nullptr, PNANOVDB_COMPUTE_CONSTANT_BUFFER_MAX_SIZE);
}

void EditorScene::get_shader_params_for_current_view(void* shader_params_data)
{
    if (!shader_params_data)
    {
        return;
    }

    auto* scene_obj = get_render_scene_object();
    if (!scene_obj)
    {
        return;
    }

    void* view_params = nullptr;
    size_t copy_size =
        (scene_obj->type == SceneObjectType::GaussianData) ? m_raster2d_params.size : m_nanovdb_params.size;
    copy_shader_params(scene_obj, SyncDirection::UiToView, &view_params);

    if (view_params && copy_size > 0)
    {
        std::memcpy(shader_params_data, view_params, copy_size);
    }
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
    std::string view_name_str = fsPath.stem().string();
    const char* view_name = view_name_str.c_str();

    // Store in SceneManager as the single source of truth
    if (auto* scene_manager = get_scene_manager())
    {
        pnanovdb_editor_token_t* scene_token = get_current_scene_token();
        pnanovdb_editor_token_t* name_token = EditorToken::getInstance().getToken(view_name);

        // Create params array initialized from JSON (like m_nanovdb_params.default_array)
        pnanovdb_compute_array_t* params_array = scene_manager->create_initialized_shader_params(
            m_compute, m_nanovdb_params.shader_name.c_str(), nullptr, PNANOVDB_COMPUTE_CONSTANT_BUFFER_MAX_SIZE);

        scene_manager->add_nanovdb(
            scene_token, name_token, nanovdb_array, params_array, m_compute, m_nanovdb_params.shader_name.c_str());

        // Register in SceneView (for scene tree display) using the SceneObject's params pointer
        if (auto* obj = scene_manager->get(scene_token, name_token))
        {
            m_views->add_nanovdb_view(nanovdb_array, obj->shader_params);
        }
    }

    // Setting current view will trigger sync_selected_view_with_current to load it
    m_views->set_current_view(view_name);
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
    std::string view_name_str = fsPath.stem().string();
    const char* view_name = view_name_str.c_str();

    // Store in SceneManager as the single source of truth
    if (auto* scene_manager = get_scene_manager())
    {
        pnanovdb_editor_token_t* scene_token = get_current_scene_token();
        pnanovdb_editor_token_t* name_token = EditorToken::getInstance().getToken(view_name);

        // Create params array initialized from JSON (like m_raster2d_params.default_array)
        pnanovdb_compute_array_t* params_array = scene_manager->create_initialized_shader_params(
            m_compute, nullptr, imgui_instance_user::s_raster2d_shader_group, 0, m_raster_shader_params_data_type);

        scene_manager->add_gaussian_data(
            scene_token, name_token, gaussian_data, params_array, m_raster_shader_params_data_type, m_compute);

        // Register in SceneView (for scene tree display) using the SceneObject's params pointer
        if (auto* obj = scene_manager->get(scene_token, name_token))
        {
            m_views->add_gaussian_view(gaussian_data, static_cast<pnanovdb_raster_shader_params_t*>(obj->shader_params));
        }
    }

    // Setting current view will trigger sync_selected_view_with_current to load it
    m_views->set_current_view(view_name);
}


bool EditorScene::remove_object(pnanovdb_editor_token_t* scene_token, const char* name)
{
    if (!name || !m_views)
    {
        return false;
    }

    bool removed = false;
    std::string name_str(name);

    // Try to remove from SceneView (this handles UI scene tree)
    if (m_views->remove_camera(scene_token, name_str))
    {
        Console::getInstance().addLog("Removed camera '%s' from scene", name);
        removed = true;
    }
    else if (m_views->remove_nanovdb(scene_token, name_str))
    {
        Console::getInstance().addLog("Removed NanoVDB '%s' from scene", name);
        removed = true;
    }
    else if (m_views->remove_gaussian(scene_token, name_str))
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
        const std::string& current_view = m_views->get_current_view(scene_token);
        if (current_view == name_str)
        {
            // Try to find another view to switch to
            const auto& nanovdbs = m_views->get_nanovdbs();
            const auto& gaussians = m_views->get_gaussians();

            if (!nanovdbs.empty())
            {
                m_views->set_current_view(scene_token, nanovdbs.begin()->first);
                Console::getInstance().addLog("Switched view to '%s'", nanovdbs.begin()->first.c_str());
            }
            else if (!gaussians.empty())
            {
                m_views->set_current_view(scene_token, gaussians.begin()->first);
                Console::getInstance().addLog("Switched view to '%s'", gaussians.begin()->first.c_str());
            }
            else
            {
                m_views->set_current_view(scene_token, "");
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

void EditorScene::initialize_view_registry()
{
    if (m_views)
    {
        m_view_registry[ViewType::Cameras] = &m_views->get_cameras();
        m_view_registry[ViewType::NanoVDBs] = &m_views->get_nanovdbs();
        m_view_registry[ViewType::GaussianScenes] = &m_views->get_gaussians();
    }
}

const std::map<std::string, pnanovdb_camera_view_t*>& EditorScene::get_camera_views() const
{
    return m_views->get_cameras();
}

const std::map<std::string, NanoVDBContext>& EditorScene::get_nanovdb_views() const
{
    return m_views->get_nanovdbs();
}

const std::map<std::string, GaussianDataContext>& EditorScene::get_gaussian_views() const
{
    return m_views->get_gaussians();
}

EditorScene::ViewMapVariant EditorScene::get_views(ViewType type) const
{
    auto it = m_view_registry.find(type);
    if (it != m_view_registry.end())
    {
        return it->second;
    }
    return std::monostate{};
}

bool EditorScene::is_selection_valid(const SceneSelection& selection) const
{
    if (!selection.is_valid())
    {
        return false;
    }

    // Validate against SceneManager (single source of truth)
    auto* scene_manager = get_scene_manager();
    if (!scene_manager)
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
                    const char* name = selection.name_token ? selection.name_token->str : nullptr;
                    return map_ptr && name && map_ptr->find(name) != map_ptr->end();
                }
            },
            map_variant);
    }

    // For NanoVDB and Gaussian data, validate against SceneManager
    pnanovdb_editor_token_t* scene_token = selection.scene_token ? selection.scene_token : get_current_scene_token();
    return scene_manager->get(scene_token, selection.name_token) != nullptr;
}

// ============================================================================
// Scene Selection Management
// ============================================================================

// update_selection() removed; logic migrated into set_properties_selection()

// Scene management
EditorSceneManager* EditorScene::get_scene_manager() const
{
    return m_editor->impl->scene_manager;
}

void EditorScene::set_current_scene(pnanovdb_editor_token_t* scene_token)
{
    if (m_views)
    {
        pnanovdb_editor_token_t* old_scene = m_views->get_current_scene_token();
        m_views->set_current_scene(scene_token);

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
}

pnanovdb_editor_token_t* EditorScene::get_current_scene_token() const
{
    return m_views ? m_views->get_current_scene_token() : nullptr;
}

std::vector<pnanovdb_editor_token_t*> EditorScene::get_all_scene_tokens() const
{
    return m_views ? m_views->get_all_scene_tokens() : std::vector<pnanovdb_editor_token_t*>();
}

bool EditorScene::has_valid_selection() const
{
    return is_selection_valid(m_view_selection);
}

void EditorScene::clear_selection()
{
    m_view_selection = SceneSelection();
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
    if (!name_token)
    {
        return;
    }

    if (!is_selection_valid(SceneSelection{ type, name_token, scene_token }))
    {
        return;
    }
    if (type != ViewType::NanoVDBs && type != ViewType::GaussianScenes)
    {
        return;
    }

    m_render_view_selection = { type, name_token, scene_token };

    // Load into editor and UI so renderer switches to the requested view
    auto* scene_obj = get_scene_object(name_token, type);
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
        m_views_camera_state[view_name_token->id] = state;
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
