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

// Forward declarations
namespace pnanovdb_editor
{
class EditorSceneManager;
}

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
    pnanovdb_compute_queue_t* device_queue;
    pnanovdb_compiler_instance_t* compiler_inst;
    const char* default_shader_name;
};

// Selection management - ensures type-safe selection across view types
struct SceneSelection
{
    ViewType type;
    pnanovdb_editor_token_t* name_token; // Object name as token (replaces string)
    pnanovdb_editor_token_t* scene_token; // Which scene this selection is in

    SceneSelection() : type(ViewType::None), name_token(nullptr), scene_token(nullptr)
    {
    }
    SceneSelection(ViewType t, pnanovdb_editor_token_t* name, pnanovdb_editor_token_t* scene = nullptr)
        : type(t), name_token(name), scene_token(scene)
    {
    }

    bool is_valid() const
    {
        return type != ViewType::None && name_token != nullptr;
    }
    bool operator==(const SceneSelection& other) const
    {
        // Compare token IDs for efficiency
        uint64_t this_name_id = name_token ? name_token->id : 0;
        uint64_t other_name_id = other.name_token ? other.name_token->id : 0;
        uint64_t this_scene_id = scene_token ? scene_token->id : 0;
        uint64_t other_scene_id = other.scene_token ? other.scene_token->id : 0;

        return type == other.type && this_name_id == other_name_id && this_scene_id == other_scene_id;
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

enum class SyncDirection
{
    UIToEditor,
    EditorToUI,
    UiToView,
};

class EditorScene
{
private:
    void copy_editor_shader_params_to_ui(ShaderParams* params);
    void copy_shader_params_from_ui_to_view(ShaderParams* params, void* view_params);
    void copy_ui_shader_params_from_to_editor(ShaderParams* params);

public:
    explicit EditorScene(const EditorSceneConfig& config);
    ~EditorScene();

    UnifiedViewContext get_view_context(pnanovdb_editor_token_t* view_name_token, ViewType view_type) const;
    UnifiedViewContext get_current_view_context() const;
    UnifiedViewContext get_render_view_context() const;
    uint64_t get_current_view_epoch() const
    {
        return m_views ? m_views->get_current_view_epoch() : 0;
    }

    // Editor per update sync
    void process_pending_editor_changes();
    void process_pending_ui_changes();
    void sync_selected_view_with_current();
    void sync_shader_params_from_editor();

    // Copy current editor shader params from UI
    void get_shader_params_for_current_view(void* shader_params_data);

    // Scene management
    EditorSceneManager* get_scene_manager() const;
    void set_current_scene(pnanovdb_editor_token_t* scene_token);
    pnanovdb_editor_token_t* get_current_scene_token() const;
    std::vector<pnanovdb_editor_token_t*> get_all_scene_tokens() const;

    // Scene selection management
    bool has_valid_selection() const;
    void clear_selection();

    void set_properties_selection(ViewType type,
                                  pnanovdb_editor_token_t* name_token,
                                  pnanovdb_editor_token_t* scene_token = nullptr);
    SceneSelection get_properties_selection() const;

    void set_render_view(ViewType type,
                         pnanovdb_editor_token_t* name_token,
                         pnanovdb_editor_token_t* scene_token = nullptr);
    SceneSelection get_render_view_selection() const;

    // Camera state management
    void save_camera_state(pnanovdb_editor_token_t* name_token, const pnanovdb_camera_state_t& state);
    const pnanovdb_camera_state_t* get_saved_camera_state(pnanovdb_editor_token_t* name_token) const;
    void update_view_camera_state(pnanovdb_editor_token_t* view_name_token, const pnanovdb_camera_state_t& state);
    const std::map<uint64_t, pnanovdb_camera_state_t>& get_views_camera_states() const
    {
        return m_views_camera_state;
    }
    std::map<uint64_t, pnanovdb_camera_state_t>& get_views_camera_states_mutable()
    {
        return m_views_camera_state;
    }

    // Camera frustum index management
    int get_camera_frustum_index(pnanovdb_editor_token_t* camera_name_token) const;
    void set_camera_frustum_index(pnanovdb_editor_token_t* camera_name_token, int index);

    // Update shader if needed
    void update_viewport_shader(const char* new_shader);

    // Handle NanoVDB load completion
    void handle_nanovdb_data_load(pnanovdb_compute_array_t* nanovdb_array, const char* filename);

    // Handle Gaussian data load completion
    void handle_gaussian_data_load(pnanovdb_raster_context_t* raster_ctx,
                                   pnanovdb_raster_gaussian_data_t* gaussian_data,
                                   pnanovdb_raster_shader_params_t* raster_params,
                                   const char* filename,
                                   pnanovdb_raster_t* raster,
                                   std::shared_ptr<pnanovdb_raster_gaussian_data_t>& old_gaussian_data_ptr);

    // Add NanoVDB to loaded arrays
    void add_nanovdb_to_scene_data(pnanovdb_compute_array_t* nanovdb_array, const char* shader_name, const char* view_name);

    // Add Gaussian data to loaded arrays
    void add_gaussian_to_scene_data(pnanovdb_raster_context_t* raster_ctx,
                                    pnanovdb_raster_gaussian_data_t* gaussian_data,
                                    pnanovdb_raster_shader_params_t* raster_params,
                                    const char* view_name,
                                    pnanovdb_raster_t* raster,
                                    std::shared_ptr<pnanovdb_raster_gaussian_data_t>& old_gaussian_data_ptr);

    // Remove object from scene (UI, loaded data, renderer state, selection)
    bool remove_object(pnanovdb_editor_token_t* scene_token, const char* name);

    // Sync default camera view with current viewport camera
    void sync_default_camera_view();

    const pnanovdb_reflect_data_type_t* get_raster_shader_params_data_type() const
    {
        return m_raster_shader_params_data_type;
    }

    const EditorSceneData& get_editor_data() const
    {
        return m_scene_data;
    }

    pnanovdb_editor_t* get_editor() const
    {
        return m_editor;
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
    void copy_shader_params(const UnifiedViewContext& view_ctx,
                            SyncDirection sync_direction,
                            void** view_params_out = nullptr);
    void sync_current_view_state(SyncDirection sync_direction);
    void clear_editor_view_state();
    void load_view_into_editor_and_ui(const UnifiedViewContext& view_ctx);
    bool handle_pending_view_changes();
    void initialize_view_registry();

    // NanoVDB file operations
    void load_nanovdb_to_editor();
    void save_editor_nanovdb();

    std::map<ViewType, ViewMapVariant> m_view_registry;

    imgui_instance_user::Instance* m_imgui_instance;
    pnanovdb_editor_t* m_editor;
    EditorView* m_views;
    const pnanovdb_compute_t* m_compute;
    pnanovdb_imgui_settings_render_t* m_imgui_settings;
    pnanovdb_compute_queue_t* m_device_queue;

    SceneSelection m_view_selection; // what UI shows
    SceneSelection m_render_view_selection; // what renderer uses

    std::map<uint64_t, pnanovdb_camera_state_t> m_saved_camera_states; // Indexed by token ID
    std::map<uint64_t, pnanovdb_camera_state_t> m_views_camera_state; // Indexed by token ID
    std::map<uint64_t, int> m_camera_frustum_index; // Indexed by token ID

    const pnanovdb_reflect_data_type_t* m_raster_shader_params_data_type;

    ShaderParams m_nanovdb_params;
    ShaderParams m_raster2d_params;

    pnanovdb_camera_config_t m_default_camera_view_config;
    pnanovdb_camera_state_t m_default_camera_view_state;

    EditorSceneData m_scene_data;
};

} // namespace pnanovdb_editor
