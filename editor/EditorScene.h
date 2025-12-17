// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/editor/EditorScene.h

    \author Petra Hapalova

    \brief  Handles all view switching and state management
*/

#pragma once

#include "SceneView.h"
#include "nanovdb_editor/putil/Editor.h"
#include "imgui/ImguiWindow.h"

#include <string>
#include <map>
#include <filesystem>
#include <variant>
#include <memory>

namespace imgui_instance_user
{
struct Instance;
}

namespace pnanovdb_editor
{
class EditorSceneManager;
struct SceneObject;
enum class SceneObjectType;

enum class ViewType
{
    Root,
    Cameras,
    GaussianScenes,
    NanoVDBs,
    None
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
    bool operator==(const SceneSelection& other) const;
    bool operator!=(const SceneSelection& other) const;
};

struct SceneShaderParams
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

// This class handles scene management and synchronization between the editor and the UI
class EditorScene
{
private:
    void copy_editor_shader_params_to_ui(SceneShaderParams* params);
    void copy_shader_params_from_ui_to_view(SceneShaderParams* params, void* view_params);
    void copy_ui_shader_params_from_to_editor(SceneShaderParams* params);

    // Helper to determine view type from tokens
    ViewType determine_view_type(pnanovdb_editor_token_t* view_token, pnanovdb_editor_token_t* scene_token) const;

public:
    explicit EditorScene(const EditorSceneConfig& config);
    ~EditorScene();

    SceneObject* get_scene_object(pnanovdb_editor_token_t* view_name_token, ViewType view_type) const;
    uint64_t get_current_view_epoch() const
    {
        return m_scene_view.get_current_view_epoch();
    }

    // Editor per update sync
    void process_pending_editor_changes();
    void process_pending_ui_changes();

    void sync_selected_view_with_current();
    void sync_shader_params_from_editor();
    void sync_views_from_scene_manager(); // Sync views from scene manager (for worker thread safety)

    // To refresh shader params after shader compile
    void reload_shader_params_for_current_view();

    // Copy current editor shader params from UI
    void get_shader_params_for_current_view(void* shader_params_data);

    // Scene management
    EditorSceneManager* get_scene_manager() const;
    void set_current_scene(pnanovdb_editor_token_t* scene_token);
    pnanovdb_editor_token_t* get_current_scene_token() const;

    // Get the viewport camera token for the current scene
    pnanovdb_editor_token_t* get_viewport_camera_token() const
    {
        return m_scene_view.get_viewport_camera_token();
    }

    // Get the viewport camera token for a specific scene
    pnanovdb_editor_token_t* get_viewport_camera_token(pnanovdb_editor_token_t* scene_token) const
    {
        return m_scene_view.get_viewport_camera_token(scene_token);
    }

    // Check if a camera is the viewport camera (in current scene)
    bool is_viewport_camera(pnanovdb_editor_token_t* camera_token) const
    {
        return m_scene_view.is_viewport_camera(camera_token);
    }

    // Set which camera is the viewport camera (in current scene)
    void set_viewport_camera(pnanovdb_editor_token_t* camera_token)
    {
        m_scene_view.set_viewport_camera(camera_token);
    }

    // Create a new camera with default settings in the current scene
    // Returns the name of the created camera, or empty string on failure
    std::string add_new_camera(const char* name = nullptr)
    {
        return m_scene_view.add_new_camera(get_current_scene_token(), name);
    }

    // Get a camera by token (in current scene)
    pnanovdb_camera_view_t* get_camera(pnanovdb_editor_token_t* camera_token) const
    {
        return m_scene_view.get_camera(camera_token);
    }

    // Get the shader name from the currently selected object (for Properties window)
    std::string get_selected_object_shader_name() const;

    // Set the shader name for the currently selected object
    void set_selected_object_shader_name(const std::string& shader_name);

    // Ensure scene exists (creates with default viewport camera if needed)
    SceneViewData* get_or_create_scene(pnanovdb_editor_token_t* scene_token)
    {
        return m_scene_view.get_or_create_scene(scene_token);
    }

    std::vector<pnanovdb_editor_token_t*> get_all_scene_tokens() const;

    // Scene selection management
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
    const std::map<uint64_t, pnanovdb_camera_state_t>& get_views_camera_states() const
    {
        return m_scene_view_camera_state;
    }
    std::map<uint64_t, pnanovdb_camera_state_t>& get_views_camera_states_mutable()
    {
        return m_scene_view_camera_state;
    }

    // Camera frustum index management
    int get_camera_frustum_index(pnanovdb_editor_token_t* camera_name_token) const;
    void set_camera_frustum_index(pnanovdb_editor_token_t* camera_name_token, int index);

    // Handle NanoVDB load completion
    void handle_nanovdb_data_load(pnanovdb_compute_array_t* nanovdb_array, const char* filename);

    // Handle Gaussian data load completion
    void handle_gaussian_data_load(pnanovdb_raster_gaussian_data_t* gaussian_data,
                                   pnanovdb_raster_shader_params_t* raster_params,
                                   const char* filename,
                                   std::shared_ptr<pnanovdb_raster_gaussian_data_t>& old_gaussian_data_ptr);

    // Remove object from scene (UI, loaded data, renderer state, selection)
    bool remove_object(pnanovdb_editor_token_t* scene_token, const char* name);

    const pnanovdb_reflect_data_type_t* get_raster_shader_params_data_type() const
    {
        return m_raster_shader_params_data_type;
    }

    pnanovdb_editor_t* get_editor() const
    {
        return m_editor;
    }

    const std::map<uint64_t, CameraViewContext>& get_camera_views() const;
    const std::map<uint64_t, NanoVDBContext>& get_nanovdb_views() const;
    const std::map<uint64_t, GaussianDataContext>& get_gaussian_views() const;

    // Generic map access by type (returns variant of possible map types)
    using ViewMapVariant = std::variant<std::monostate,
                                        const std::map<uint64_t, CameraViewContext>*,
                                        const std::map<uint64_t, NanoVDBContext>*,
                                        const std::map<uint64_t, GaussianDataContext>*>;
    ViewMapVariant get_views(ViewType type) const;

    // Check if a selection is valid (item exists in the appropriate view map)
    bool is_selection_valid(const SceneSelection& selection) const;

    // Sync current scene's viewport camera from editor's camera (for properties display)
    void sync_scene_camera_from_editor();

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
    void copy_shader_params(SceneObjectType obj_type,
                            void* obj_shader_params,
                            const std::string& obj_shader_name,
                            SyncDirection sync_direction,
                            void** view_params_out = nullptr);
    void sync_current_view_state(SyncDirection sync_direction);
    void clear_editor_view_state();
    void load_view_into_editor_and_ui(SceneObject* scene_obj);
    bool handle_pending_view_changes();

    // Sync editor's camera from current scene's viewport camera
    void sync_editor_camera_from_scene();

    // NanoVDB file operations
    void load_nanovdb_to_editor();
    void save_editor_nanovdb();

    // Helper methods for internal use
    SceneObject* get_current_scene_object() const;
    SceneObject* get_render_scene_object() const;
    bool has_valid_selection() const;
    void clear_selection_if_matches(SceneSelection& selection,
                                    const char* name,
                                    pnanovdb_editor_token_t* scene_token,
                                    const char* log_message);
    bool is_switching_scenes(pnanovdb_editor_token_t* from_scene, pnanovdb_editor_token_t* to_scene) const;
    void apply_editor_camera_to_viewport();
    pnanovdb_editor_token_t* find_next_available_view(pnanovdb_editor_token_t* scene_token) const;
    pnanovdb_editor_token_t* find_any_view_in_scene(pnanovdb_editor_token_t* scene_token) const;
    void* get_view_params_with_fallback(SceneShaderParams& params, void* obj_params) const;

    imgui_instance_user::Instance* m_imgui_instance;
    pnanovdb_editor_t* m_editor;
    EditorSceneManager& m_scene_manager;
    SceneView& m_scene_view;
    const pnanovdb_compute_t* m_compute;
    pnanovdb_imgui_settings_render_t* m_imgui_settings;
    pnanovdb_compute_queue_t* m_device_queue;

    SceneSelection m_view_selection; // what UI shows
    SceneSelection m_render_view_selection; // what renderer uses

    std::map<uint64_t, pnanovdb_camera_state_t> m_saved_camera_states; // Indexed by token ID
    std::map<uint64_t, pnanovdb_camera_state_t> m_scene_view_camera_state; // Indexed by token ID

    uint64_t m_last_synced_epoch = 0; // Track last processed epoch to detect changes
    std::map<uint64_t, int> m_camera_frustum_index; // Indexed by token ID

    const pnanovdb_reflect_data_type_t* m_raster_shader_params_data_type;

    SceneShaderParams m_nanovdb_params;
    SceneShaderParams m_raster2d_params;
};

} // namespace pnanovdb_editor
