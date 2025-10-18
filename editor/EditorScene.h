// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/editor/EditorScene.h

    \author Petra Hapalova

    \brief  Handles all view switching and state management
*/

#pragma once

#include "ImguiInstance.h"
#include "EditorView.h"
#include "nanovdb_editor/putil/Editor.h"

#include <string>
#include <map>
#include <filesystem>
#include <variant>
#include <memory>

namespace pnanovdb_editor
{
enum class ViewType
{
    Root,
    Cameras,
    GaussianScenes,
    NanoVDBs,
    None
};

// Loaded context for Viewer data
class UnifiedViewContext
{
public:
    using ContextVariant = std::variant<std::monostate, GaussianDataContext*, NanoVDBContext*>;

    UnifiedViewContext() : m_view_type(ViewType::None), m_view_name(""), m_context(std::monostate{})
    {
    }

    UnifiedViewContext(const std::string& name, GaussianDataContext* ctx)
        : m_view_type(ViewType::GaussianScenes), m_view_name(name), m_context(ctx)
    {
    }

    UnifiedViewContext(const std::string& name, NanoVDBContext* ctx)
        : m_view_type(ViewType::NanoVDBs), m_view_name(name), m_context(ctx)
    {
    }

    bool is_valid() const
    {
        if (m_view_type == ViewType::None || m_view_name.empty())
        {
            return false;
        }

        return !std::holds_alternative<std::monostate>(m_context);
    }

    ViewType get_view_type() const
    {
        return m_view_type;
    }
    const std::string& get_view_name() const
    {
        return m_view_name;
    }

    GaussianDataContext* get_gaussian_context() const
    {
        if (auto* ctx = std::get_if<GaussianDataContext*>(&m_context))
        {
            return *ctx;
        }
        return nullptr;
    }

    NanoVDBContext* get_nanovdb_context() const
    {
        if (auto* ctx = std::get_if<NanoVDBContext*>(&m_context))
        {
            return *ctx;
        }
        return nullptr;
    }

private:
    ViewType m_view_type;
    std::string m_view_name;
    ContextVariant m_context;
};

// Loaded context for Gaussian data (with ownership via shared_ptr)
struct GaussianDataLoadedContext
{
    std::string name;
    pnanovdb_raster_context_t* raster_ctx = nullptr;
    std::shared_ptr<pnanovdb_raster_gaussian_data_t> gaussian_data;
    pnanovdb_raster_shader_params_t* shader_params = nullptr;
};

// Loaded context for NanoVDB data (with ownership via shared_ptr)
struct NanoVDBLoadedContext
{
    std::string name;
    std::shared_ptr<pnanovdb_compute_array_t> nanovdb_array;
    pnanovdb_compute_array_t* shader_params_array = nullptr;
};

// Loaded scene data managed by EditorScene
struct EditorSceneData
{
    std::vector<NanoVDBLoadedContext> nanovdb_arrays;
    std::vector<GaussianDataLoadedContext> gaussian_views;
};

// Configuration structure for EditorScene
struct EditorSceneConfig
{
    imgui_instance_user::Instance* imgui_instance;
    pnanovdb_editor_t* editor;
    pnanovdb_imgui_settings_render_t* imgui_settings;
    pnanovdb_imgui_window_interface_t* window_iface;
    pnanovdb_imgui_window_t* window;
    pnanovdb_compute_queue_t* device_queue;
    pnanovdb_compiler_instance_t* compiler_inst;
    const char* default_shader_name;
};

// Selection management - ensures type-safe selection across view types
struct SceneSelection
{
    ViewType type;
    std::string name;

    SceneSelection() : type(ViewType::None), name("")
    {
    }
    SceneSelection(ViewType t, const std::string& n) : type(t), name(n)
    {
    }

    bool is_valid() const
    {
        return type != ViewType::None && !name.empty();
    }
    bool operator==(const SceneSelection& other) const
    {
        return type == other.type && name == other.name;
    }
    bool operator!=(const SceneSelection& other) const
    {
        return !(*this == other);
    }
};

struct ShaderParams
{
    std::string shader_name;
    size_t size = 0u;
    pnanovdb_compute_array_t* default_array = nullptr;
    pnanovdb_compute_array_t* current_array = nullptr;
};

class EditorScene
{
public:
    explicit EditorScene(const EditorSceneConfig& config);
    ~EditorScene();

    // Get unified view context for a specific view
    UnifiedViewContext get_view_context(const std::string& view_name, ViewType view_type) const;

    // Get current view context
    UnifiedViewContext get_current_view_context() const;

    // Save current view state before switching
    void save_current_view_state();

    void load_view_into_editor_and_ui(const UnifiedViewContext& view_ctx);
    void clear_editor_view_state();
    void sync_selected_view_with_current();

    // Handle selection changes in Scene tree
    void handle_pending_view_changes();

    // Scene selection management
    void set_selection(ViewType type, const std::string& name);
    SceneSelection get_selection() const;
    bool has_valid_selection() const;
    void clear_selection();

    // Camera state management
    void save_camera_state(const std::string& name, const pnanovdb_camera_state_t& state);
    const pnanovdb_camera_state_t* get_saved_camera_state(const std::string& name) const;
    void update_view_camera_state(const std::string& view_name, const pnanovdb_camera_state_t& state);
    const std::map<std::string, pnanovdb_camera_state_t>& get_views_camera_states() const
    {
        return m_views_camera_state;
    }
    std::map<std::string, pnanovdb_camera_state_t>& get_views_camera_states_mutable()
    {
        return m_views_camera_state;
    }

    // Camera frustum index management
    int get_camera_frustum_index(const std::string& camera_name) const;
    void set_camera_frustum_index(const std::string& camera_name, int index);
    const std::map<std::string, int>& get_camera_frustum_indices() const
    {
        return m_camera_frustum_index;
    }
    std::map<std::string, int>& get_camera_frustum_indices_mutable()
    {
        return m_camera_frustum_index;
    }

    // NanoVDB file operations
    void load_nanovdb_to_editor();
    void save_editor_nanovdb();

    // Update shader if needed
    void update_viewport_shader(const char* new_shader);

    // Copy shader params from editor to UI
    void copy_editor_shader_params_to_ui(ShaderParams* params);

    // Load UI params to the view
    void copy_shader_params_from_ui_to_view(ShaderParams* params, void* view_params);

    // Copy shader params from UI to editor
    void copy_shader_params_to_editor(ShaderParams* params);

    // Handle NanoVDB load completion
    void handle_nanovdb_data_load(pnanovdb_compute_array_t* nanovdb_array, const std::string& filename);

    // Handle Gaussian data load completion
    void handle_gaussian_data_load(pnanovdb_raster_context_t* raster_ctx,
                                   pnanovdb_raster_gaussian_data_t* gaussian_data,
                                   pnanovdb_raster_shader_params_t* raster_params,
                                   const std::string& filename,
                                   pnanovdb_raster_t* raster,
                                   std::shared_ptr<pnanovdb_raster_gaussian_data_t>& old_gaussian_data_ptr);

    // Copy current editor shader params from UI
    void get_editor_params_for_current_view(void* shader_params_mapped);

    // Add NanoVDB to loaded arrays
    void add_nanovdb_to_scene_data(pnanovdb_compute_array_t* nanovdb_array,
                                   const char* shader_name,
                                   const std::string& view_name);

    // Add Gaussian data to loaded arrays
    void add_gaussian_to_scene_data(pnanovdb_raster_context_t* raster_ctx,
                                    pnanovdb_raster_gaussian_data_t* gaussian_data,
                                    pnanovdb_raster_shader_params_t* raster_params,
                                    const std::string& view_name,
                                    pnanovdb_raster_t* raster,
                                    std::shared_ptr<pnanovdb_raster_gaussian_data_t>& old_gaussian_data_ptr);

    // Sync default camera view with current viewport camera
    void sync_default_camera_view();

    const pnanovdb_raster_shader_params_t* get_raster2d_shader_params() const
    {
        const pnanovdb_raster_shader_params_t* raster_shader_params =
            (pnanovdb_raster_shader_params_t*)m_raster2d_params.current_array->data;
        return raster_shader_params;
    }

    const pnanovdb_reflect_data_type_t* get_raster_shader_params_data_type() const
    {
        return m_raster_shader_params_data_type;
    }

    void copy_editor_raster_params_to_ui(const void* editor_params)
    {
        copy_editor_shader_params_to_ui(&m_raster2d_params);
    }

    void copy_raster_params_from_ui()
    {
        copy_shader_params_from_ui_to_view(&m_raster2d_params, m_raster2d_params.current_array->data);
    }

    void copy_raster_params_to_editor()
    {
        copy_shader_params_to_editor(&m_raster2d_params);
    }

    EditorSceneData& get_editor_data()
    {
        return m_scene_data;
    }
    const EditorSceneData& get_editor_data() const
    {
        return m_scene_data;
    }

    const std::map<std::string, pnanovdb_camera_view_t*>& get_camera_views() const;
    const std::map<std::string, NanoVDBContext>& get_nanovdb_views() const;
    const std::map<std::string, GaussianDataContext>& get_gaussian_views() const;

    // Generic map access by type (returns variant of possible map types)
    using ViewMapVariant = std::variant<std::monostate,
                                        const std::map<std::string, pnanovdb_camera_view_t*>*,
                                        const std::map<std::string, NanoVDBContext>*,
                                        const std::map<std::string, GaussianDataContext>*>;
    ViewMapVariant get_views(ViewType type) const;

    // Specific operations for managing views
    void add_camera_view(const std::string& name, pnanovdb_camera_view_t* camera);

    // Check if a selection is valid (item exists in the appropriate view map)
    bool is_selection_valid(const SceneSelection& selection) const;

    // Generic iteration over views of any type
    // Callback signature: void(const std::string& name, const auto& view_data)
    template <typename Callback>
    void for_each_view(ViewType type, Callback&& callback) const
    {
        auto map_variant = get_views(type);
        std::visit(
            [&callback](auto&& map_ptr)
            {
                using T = std::decay_t<decltype(map_ptr)>;
                if constexpr (!std::is_same_v<T, std::monostate>)
                {
                    if (map_ptr)
                    {
                        for (const auto& pair : *map_ptr)
                            callback(pair.first, pair.second);
                    }
                }
            },
            map_variant);
    }

private:
    void initialize_view_registry();
    std::map<ViewType, ViewMapVariant> m_view_registry;

    imgui_instance_user::Instance* m_imgui_instance;
    pnanovdb_editor_t* m_editor;
    EditorView* m_views;
    const pnanovdb_compute_t* m_compute;
    pnanovdb_imgui_settings_render_t* m_imgui_settings;
    pnanovdb_imgui_window_interface_t* m_window_iface;
    pnanovdb_imgui_window_t* m_window;
    pnanovdb_compute_queue_t* m_device_queue;

    SceneSelection m_view_selection;

    std::map<std::string, pnanovdb_camera_state_t> m_saved_camera_states;
    std::map<std::string, pnanovdb_camera_state_t> m_views_camera_state;
    std::map<std::string, int> m_camera_frustum_index;

    const pnanovdb_reflect_data_type_t* m_raster_shader_params_data_type;

    ShaderParams m_nanovdb_params;
    ShaderParams m_raster2d_params;

    pnanovdb_camera_config_t m_default_camera_view_config;
    pnanovdb_camera_state_t m_default_camera_view_state;

    EditorSceneData m_scene_data;
};

} // namespace pnanovdb_editor
