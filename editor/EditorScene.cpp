// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/editor/EditorScene.cpp

    \author Petra Hapalova

    \brief  Handles all view switching and state management
*/

#include "EditorScene.h"
#include "EditorView.h"
#include "Editor.h"
#include "ShaderMonitor.h"
#include "ShaderCompileUtils.h"
#include "Console.h"

#include <filesystem>

namespace pnanovdb_editor
{
const char* s_raster2d_shader_name = "raster/gaussian_rasterize_2d.slang";

EditorScene::EditorScene(const EditorSceneConfig& config)
    : m_imgui_instance(config.imgui_instance),
      m_editor(config.editor),
      m_views(static_cast<EditorView*>(config.editor->impl->views)),
      m_compute(config.editor->impl->compute),
      m_imgui_settings(config.imgui_settings),
      m_window_iface(config.window_iface),
      m_window(config.window),
      m_device_queue(config.device_queue)
{
    // Setup views UI - ImguiInstance accesses views through EditorScene
    m_imgui_instance->editor_scene = this;
    m_imgui_instance->set_default_shader(config.default_shader_name);

    // Initialize view registry (maps ViewsTypes to actual map pointers)
    initialize_view_registry();

    // Initialize NanoVDB shader params arrays with defaults
    {
        m_imgui_instance->shader_params.load(m_imgui_instance->shader_name, true);

        m_nanovdb_params.shader_name = config.default_shader_name;
        m_nanovdb_params.size = PNANOVDB_COMPUTE_CONSTANT_BUFFER_MAX_SIZE;
        m_nanovdb_params.default_array = m_imgui_instance->shader_params.get_compute_array_for_shader(
            m_imgui_instance->shader_name.c_str(), m_compute);
    }

    // Initialize raster shader params arrays with defaults
    {
        m_raster_shader_params_data_type = PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_raster_shader_params_t);
        const pnanovdb_raster_shader_params_t* default_raster_shader_params =
            (const pnanovdb_raster_shader_params_t*)m_raster_shader_params_data_type->default_value;

        m_imgui_instance->shader_params.loadGroup(imgui_instance_user::s_raster2d_shader_group, true);

        m_raster2d_params.shader_name = s_raster2d_shader_name;
        m_raster2d_params.size = m_raster_shader_params_data_type->element_size;
        m_raster2d_params.default_array =
            m_compute->create_array(m_raster_shader_params_data_type->element_size, 1u, default_raster_shader_params);
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
    m_imgui_instance->default_camera_view.name = imgui_instance_user::VIEWPORT_CAMERA;
    m_imgui_instance->default_camera_view.configs = &m_default_camera_view_config;
    m_imgui_instance->default_camera_view.states = &m_default_camera_view_state;
    m_imgui_instance->default_camera_view.num_cameras = 1;
    m_imgui_instance->default_camera_view.is_visible = PNANOVDB_FALSE;

    // Add default viewport camera
    add_camera_view(imgui_instance_user::VIEWPORT_CAMERA, &m_imgui_instance->default_camera_view);

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

    // Clean up loaded nanovdb arrays
    for (auto& it : m_scene_data.nanovdb_arrays)
    {
        if (it.shader_params_array)
        {
            m_compute->destroy_array(it.shader_params_array);
        }
    }

    // Clear loaded views
    m_scene_data.nanovdb_arrays.clear();
    m_scene_data.gaussian_views.clear();
}

UnifiedViewContext EditorScene::get_view_context(const std::string& view_name, ViewType view_type) const
{
    if (view_type == ViewType::GaussianScenes)
    {
        auto it = m_views->get_gaussians().find(view_name);
        if (it != m_views->get_gaussians().end())
        {
            return UnifiedViewContext(view_name, const_cast<GaussianDataContext*>(&it->second));
        }
    }
    else if (view_type == ViewType::NanoVDBs)
    {
        auto it = m_views->get_nanovdbs().find(view_name);
        if (it != m_views->get_nanovdbs().end())
        {
            return UnifiedViewContext(view_name, const_cast<NanoVDBContext*>(&it->second));
        }
    }
    return UnifiedViewContext();
}

UnifiedViewContext EditorScene::get_current_view_context() const
{
    if (m_view_selection.name.empty())
    {
        return UnifiedViewContext();
    }

    return get_view_context(m_view_selection.name, m_view_selection.type);
}

void EditorScene::save_current_view_state()
{
    std::string current_scene_item = m_view_selection.name;
    ViewType current_view_type = m_view_selection.type;

    if (current_scene_item.empty())
    {
        return;
    }

    auto view_ctx = get_view_context(current_scene_item, current_view_type);
    if (!view_ctx.is_valid())
    {
        return;
    }

    // Save shader params from UI back to the view
    if (view_ctx.get_view_type() == ViewType::GaussianScenes)
    {
        auto* gaussian_ctx = view_ctx.get_gaussian_context();
        if (m_imgui_instance->viewport_option == imgui_instance_user::ViewportOption::Raster2D && gaussian_ctx &&
            gaussian_ctx->shader_params)
        {
            copy_shader_params_from_ui_to_view(&m_raster2d_params, (void*)&gaussian_ctx->shader_params);
        }
    }
    else if (view_ctx.get_view_type() == ViewType::NanoVDBs)
    {
        auto* nanovdb_ctx = view_ctx.get_nanovdb_context();
        if (m_imgui_instance->viewport_option == imgui_instance_user::ViewportOption::NanoVDB && nanovdb_ctx &&
            nanovdb_ctx->shader_params)
        {
            copy_shader_params_from_ui_to_view(&m_nanovdb_params, nanovdb_ctx->shader_params);
        }
    }
}

void EditorScene::load_view_into_editor_and_ui(const UnifiedViewContext& view_ctx)
{
    if (!view_ctx.is_valid())
    {
        return;
    }

    clear_editor_view_state();

    if (view_ctx.get_view_type() == ViewType::GaussianScenes)
    {
        auto* gaussian_ctx = view_ctx.get_gaussian_context();
        if (gaussian_ctx)
        {
            m_editor->impl->gaussian_data = gaussian_ctx->gaussian_data;
            m_editor->impl->raster_ctx = gaussian_ctx->raster_ctx;
            m_editor->impl->shader_params = gaussian_ctx->shader_params;
            m_editor->impl->shader_params_data_type = m_raster_shader_params_data_type;

            copy_editor_shader_params_to_ui(&m_raster2d_params);
        }
    }
    else if (view_ctx.get_view_type() == ViewType::NanoVDBs)
    {
        auto* nanovdb_ctx = view_ctx.get_nanovdb_context();
        if (nanovdb_ctx)
        {
            m_editor->impl->nanovdb_array = nanovdb_ctx->nanovdb_array;
            m_editor->impl->shader_params = nanovdb_ctx->shader_params;

            copy_editor_shader_params_to_ui(&m_nanovdb_params);
        }
    }
}

void EditorScene::clear_editor_view_state()
{
    // Clear all view-related pointers before switching to a new view
    m_editor->impl->nanovdb_array = nullptr;
    m_editor->impl->gaussian_data = nullptr;
    m_editor->impl->raster_ctx = nullptr;
}

void EditorScene::sync_selected_view_with_current()
{
    const std::string& view_current = m_views->get_current_view();
    if (view_current.empty() || m_view_selection.name == view_current)
    {
        return;
    }

    // View changed - save current view state before switching
    save_current_view_state();

    // Determine the type of the new view
    ViewType new_view_type = ViewType::None;
    if (m_views->get_nanovdbs().find(view_current) != m_views->get_nanovdbs().end())
    {
        new_view_type = ViewType::NanoVDBs;
    }
    else if (m_views->get_gaussians().find(view_current) != m_views->get_gaussians().end())
    {
        new_view_type = ViewType::GaussianScenes;
    }

    if (new_view_type == ViewType::None)
    {
        clear_selection();
        clear_editor_view_state();
        return;
    }

    // Update selection
    set_selection(new_view_type, view_current);

    // Get the view context from EditorView
    auto view_ctx = get_view_context(view_current, new_view_type);
    if (!view_ctx.is_valid())
    {
        return;
    }

    // Load the view into editor and UI (this updates editor->impl and UI params)
    load_view_into_editor_and_ui(view_ctx);

    // Update viewport shader if switching view types
    if (new_view_type == ViewType::NanoVDBs)
    {
        m_imgui_instance->viewport_option = imgui_instance_user::ViewportOption::NanoVDB;
    }
    else if (new_view_type == ViewType::GaussianScenes)
    {
        m_imgui_instance->viewport_option = imgui_instance_user::ViewportOption::Raster2D;
    }
}

void EditorScene::handle_pending_view_changes()
{
    // Handle UI-triggered view changes (from scene tree)
    // These set the current view in EditorView, which sync_selected_view_with_current will detect

    if (!m_imgui_instance->pending.viewport_gaussian_view.empty() &&
        m_imgui_instance->pending.viewport_gaussian_view != m_view_selection.name)
    {
        std::string pending_view_name = m_imgui_instance->pending.viewport_gaussian_view;
        m_imgui_instance->pending.viewport_gaussian_view.clear();

        // Set as current view - sync_selected_view_with_current will handle the rest
        m_views->set_current_view(pending_view_name);
    }
    else if (!m_imgui_instance->pending.viewport_nanovdb_array.empty() &&
             m_imgui_instance->pending.viewport_nanovdb_array != m_view_selection.name)
    {
        std::string pending_view_name = m_imgui_instance->pending.viewport_nanovdb_array;
        m_imgui_instance->pending.viewport_nanovdb_array.clear();

        // Set as current view - sync_selected_view_with_current will handle the rest
        m_views->set_current_view(pending_view_name);
    }
}

void EditorScene::update_viewport_shader(const char* new_shader)
{
    if (m_imgui_instance->shader_name != new_shader)
    {
        m_imgui_instance->shader_name = new_shader;
        m_imgui_instance->pending.viewport_shader_name = new_shader;
        m_imgui_instance->pending.update_shader = true;
    }
}

void EditorScene::copy_editor_shader_params_to_ui(ShaderParams* params)
{
    if (!params || !m_editor->impl->shader_params)
    {
        return;
    }

    params->current_array = m_compute->create_array(params->size, 1u, m_editor->impl->shader_params);
    m_imgui_instance->shader_params.set_compute_array_for_shader(params->shader_name, params->current_array);
}

void EditorScene::copy_shader_params_from_ui_to_view(ShaderParams* params, void* view_params)
{
    if (!params || !view_params)
    {
        return;
    }

    m_compute->destroy_array(params->current_array);
    params->current_array = m_imgui_instance->shader_params.get_compute_array_for_shader(params->shader_name, m_compute);
    std::memcpy(view_params, params->current_array->data, params->size);
}

void EditorScene::copy_shader_params_to_editor(ShaderParams* params)
{
    if (params && params->current_array)
    {
        std::memcpy(m_editor->impl->shader_params, params->current_array->data, params->size);
    }
}

void EditorScene::handle_nanovdb_data_load(pnanovdb_compute_array_t* nanovdb_array, const std::string& filename)
{
    nanovdb_array = m_editor->impl->compute->load_nanovdb(filename.c_str());
    if (!nanovdb_array)
    {
        return;
    }

    std::filesystem::path fsPath(filename);
    std::string view_name = fsPath.stem().string();

    // Add to scene data for ownership management
    add_nanovdb_to_scene_data(nanovdb_array, m_imgui_instance->shader_name.c_str(), view_name);

    // Get the shader params we just created
    void* shader_params = nullptr;
    for (auto& ctx : m_scene_data.nanovdb_arrays)
    {
        if (ctx.name == view_name && ctx.shader_params_array)
        {
            shader_params = ctx.shader_params_array->data;
            break;
        }
    }

    // Register in EditorView (so it appears in scene tree)
    m_views->add_nanovdb_view(nanovdb_array, shader_params);

    // Setting current view will trigger sync_selected_view_with_current to load it
    m_views->set_current_view(view_name);
}

void EditorScene::handle_gaussian_data_load(pnanovdb_raster_context_t* raster_ctx,
                                            pnanovdb_raster_gaussian_data_t* gaussian_data,
                                            pnanovdb_raster_shader_params_t* raster_params,
                                            const std::string& filename,
                                            pnanovdb_raster_t* raster,
                                            std::shared_ptr<pnanovdb_raster_gaussian_data_t>& old_gaussian_data_ptr)
{
    std::filesystem::path fsPath(filename);
    std::string view_name = fsPath.stem().string();

    // Add to scene data for ownership management
    add_gaussian_to_scene_data(raster_ctx, gaussian_data, raster_params, view_name, raster, old_gaussian_data_ptr);

    // Register in EditorView (so it appears in scene tree)
    m_views->add_gaussian_view(raster_ctx, gaussian_data, raster_params);

    // Setting current view will trigger sync_selected_view_with_current to load it
    m_views->set_current_view(view_name);
}

void EditorScene::get_editor_params_for_current_view(void* shader_params_mapped)
{
    if (!shader_params_mapped || m_view_selection.type != ViewType::NanoVDBs || m_view_selection.name.empty())
    {
        return;
    }

    auto view_it = m_views->get_nanovdbs().find(m_view_selection.name);
    if (view_it != m_views->get_nanovdbs().end())
    {
        if (!view_it->second.shader_params)
        {
            view_it->second.shader_params = m_nanovdb_params.default_array->data;
        }

        copy_shader_params_from_ui_to_view(&m_nanovdb_params, view_it->second.shader_params);
        std::memcpy(shader_params_mapped, view_it->second.shader_params, PNANOVDB_COMPUTE_CONSTANT_BUFFER_MAX_SIZE);
        const char* data = static_cast<const char*>(view_it->second.shader_params);
    }
}

void EditorScene::add_nanovdb_to_scene_data(pnanovdb_compute_array_t* nanovdb_array,
                                            const char* shader_name,
                                            const std::string& view_name)
{
    // Find and remove existing view with same name
    for (auto itPrev = m_scene_data.nanovdb_arrays.begin(); itPrev != m_scene_data.nanovdb_arrays.end(); ++itPrev)
    {
        if (itPrev->name == view_name)
        {
            if (itPrev->shader_params_array)
            {
                m_compute->destroy_array(itPrev->shader_params_array);
            }
            m_scene_data.nanovdb_arrays.erase(itPrev);
            break;
        }
    }

    auto& loaded_ctx = m_scene_data.nanovdb_arrays.emplace_back();
    loaded_ctx.name = view_name;
    loaded_ctx.nanovdb_array = std::shared_ptr<pnanovdb_compute_array_t>(
        nanovdb_array, [destroy_fn = m_compute->destroy_array](pnanovdb_compute_array_t* ptr) { destroy_fn(ptr); });
    loaded_ctx.shader_params_array = m_imgui_instance->shader_params.get_compute_array_for_shader(shader_name, m_compute);
}

void EditorScene::add_gaussian_to_scene_data(pnanovdb_raster_context_t* raster_ctx,
                                             pnanovdb_raster_gaussian_data_t* gaussian_data,
                                             pnanovdb_raster_shader_params_t* raster_params,
                                             const std::string& view_name,
                                             pnanovdb_raster_t* raster,
                                             std::shared_ptr<pnanovdb_raster_gaussian_data_t>& old_gaussian_data_ptr)
{
    // Find and remove existing view with same name
    for (auto itPrev = m_scene_data.gaussian_views.begin(); itPrev != m_scene_data.gaussian_views.end(); ++itPrev)
    {
        if (itPrev->name == view_name)
        {
            old_gaussian_data_ptr = itPrev->gaussian_data;
            m_scene_data.gaussian_views.erase(itPrev);
            break;
        }
    }

    auto& loaded_view = m_scene_data.gaussian_views.emplace_back();
    loaded_view.name = view_name;
    loaded_view.raster_ctx = raster_ctx;
    loaded_view.gaussian_data = std::shared_ptr<pnanovdb_raster_gaussian_data_t>(
        gaussian_data, [destroy_fn = raster->destroy_gaussian_data, compute = raster->compute, queue = m_device_queue](
                           pnanovdb_raster_gaussian_data_t* ptr) { destroy_fn(compute, queue, ptr); });
    loaded_view.shader_params = raster_params;
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

void EditorScene::add_camera_view(const std::string& name, pnanovdb_camera_view_t* camera)
{
    m_views->add_camera(name, camera);
}

bool EditorScene::is_selection_valid(const SceneSelection& selection) const
{
    if (!selection.is_valid())
    {
        return false;
    }

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
                return map_ptr && map_ptr->find(selection.name) != map_ptr->end();
            }
        },
        map_variant);
}

// ============================================================================
// Scene Selection Management
// ============================================================================

void EditorScene::set_selection(ViewType type, const std::string& name)
{
    // Update our selection with type info
    m_view_selection = SceneSelection(type, name);

    // Keep EditorView in sync (it's the source of truth for the view name)
    m_views->set_current_view(name);
}

SceneSelection EditorScene::get_selection() const
{
    return m_view_selection;
}

bool EditorScene::has_valid_selection() const
{
    return is_selection_valid(m_view_selection);
}

void EditorScene::clear_selection()
{
    m_view_selection = SceneSelection();
}

// ============================================================================
// Camera State Management
// ============================================================================

void EditorScene::save_camera_state(const std::string& name, const pnanovdb_camera_state_t& state)
{
    m_saved_camera_states[name] = state;
}

const pnanovdb_camera_state_t* EditorScene::get_saved_camera_state(const std::string& name) const
{
    auto it = m_saved_camera_states.find(name);
    return (it != m_saved_camera_states.end()) ? &it->second : nullptr;
}

void EditorScene::update_view_camera_state(const std::string& view_name, const pnanovdb_camera_state_t& state)
{
    m_views_camera_state[view_name] = state;
}

int EditorScene::get_camera_frustum_index(const std::string& camera_name) const
{
    auto it = m_camera_frustum_index.find(camera_name);
    return (it != m_camera_frustum_index.end()) ? it->second : -1;
}

void EditorScene::set_camera_frustum_index(const std::string& camera_name, int index)
{
    m_camera_frustum_index[camera_name] = index;
}

// ============================================================================
// NanoVDB File Operations
// ============================================================================

void EditorScene::load_nanovdb_to_editor()
{
    handle_nanovdb_data_load(m_editor->impl->nanovdb_array, m_imgui_instance->nanovdb_filepath);
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
