// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/editor/SceneView.h

    \author Petra Hapalova

    \brief  Views in the editor
*/

#pragma once

#include "nanovdb_editor/putil/Editor.h"
#include "nanovdb_editor/putil/Raster.h"

#include <string>
#include <map>
#include <atomic>
#include <vector>

struct pnanovdb_imgui_settings_render_t;

namespace pnanovdb_editor
{

// Default scene name used when no scene is specified
static constexpr const char* DEFAULT_SCENE_NAME = "<default>";

// Context data for a NanoVDB view
struct NanoVDBContext
{
    pnanovdb_compute_array_t* nanovdb_array = nullptr;
    void* shader_params = nullptr;
};

// Context data for a Gaussian splatting view
struct GaussianDataContext
{
    pnanovdb_raster_gaussian_data_t* gaussian_data = nullptr;
    pnanovdb_raster_shader_params_t* shader_params = nullptr;
};

// Scene view data for a specific scene ID - holds all views in that scene
struct SceneViewData
{
    std::map<uint64_t, pnanovdb_camera_view_t*> cameras; // Key = token ID
    std::map<uint64_t, GaussianDataContext> gaussians; // Key = token ID
    std::map<uint64_t, NanoVDBContext> nanovdbs; // Key = token ID
    uint64_t last_added_view_token_id = 0; // Track last added view for auto-selection on scene switch
    uint64_t current_view_token_id = 0; // Current view token ID selected in this scene
    std::atomic<uint64_t> current_view_epoch{ 0 };
    int unnamed_counter = 0;

    // Per-scene default camera config and state (initialized on creation)
    pnanovdb_camera_config_t default_camera_config;
    pnanovdb_camera_state_t default_camera_state;
    pnanovdb_camera_view_t default_camera_view;
};

// Views representing loaded scenes via editor's API, does not own the data
// Manages multiple scene view data (one per scene token)
class SceneView
{
public:
    SceneView();
    ~SceneView() = default;

    // Scene management
    SceneViewData* get_or_create_scene(pnanovdb_editor_token_t* scene_token);

    // Get current scene (defaults to default scene if none set)
    SceneViewData* get_current_scene();
    const SceneViewData* get_current_scene() const;

    // Get cached viewport camera token (initialized once in constructor)
    pnanovdb_editor_token_t* get_viewport_camera_token() const
    {
        return m_viewport_camera_token;
    }

    // Set which scene is current
    void set_current_scene(pnanovdb_editor_token_t* scene_token);
    pnanovdb_editor_token_t* get_current_scene_token() const
    {
        return m_current_scene_token;
    }

    // Get all scene tokens
    std::vector<pnanovdb_editor_token_t*> get_all_scene_tokens() const;

    // Check if any scenes exist
    bool has_scenes() const
    {
        return !m_scene_view_data.empty();
    }

    // Cameras (in current scene)
    void add_camera(pnanovdb_editor_token_t* name_token, pnanovdb_camera_view_t* camera);
    void add_camera(pnanovdb_editor_token_t* scene_token,
                    pnanovdb_editor_token_t* name_token,
                    pnanovdb_camera_view_t* camera);
    pnanovdb_camera_view_t* get_camera(pnanovdb_editor_token_t* name_token) const;
    pnanovdb_camera_view_t* get_camera(pnanovdb_editor_token_t* scene_token, pnanovdb_editor_token_t* name_token) const;
    std::map<uint64_t, pnanovdb_camera_view_t*>& get_cameras();
    const std::map<uint64_t, pnanovdb_camera_view_t*>& get_cameras() const;

    // Current view selection (in current scene)
    void set_current_view(pnanovdb_editor_token_t* view_token);
    void set_current_view(pnanovdb_editor_token_t* scene_token, pnanovdb_editor_token_t* view_token);
    pnanovdb_editor_token_t* get_current_view() const;
    pnanovdb_editor_token_t* get_current_view(pnanovdb_editor_token_t* scene_token) const;
    uint64_t get_current_view_epoch() const;
    uint64_t get_current_view_epoch(pnanovdb_editor_token_t* scene_token) const;

    // Gaussians (in current scene)
    void add_gaussian(pnanovdb_editor_token_t* name_token, const GaussianDataContext& ctx);
    void add_gaussian(pnanovdb_editor_token_t* scene_token,
                      pnanovdb_editor_token_t* name_token,
                      const GaussianDataContext& ctx);
    GaussianDataContext* get_gaussian(pnanovdb_editor_token_t* scene_token, pnanovdb_editor_token_t* name_token);
    const GaussianDataContext* get_gaussian(pnanovdb_editor_token_t* scene_token,
                                            pnanovdb_editor_token_t* name_token) const;
    std::map<uint64_t, GaussianDataContext>& get_gaussians();
    const std::map<uint64_t, GaussianDataContext>& get_gaussians() const;

    // NanoVDBs (in current scene)
    void add_nanovdb(pnanovdb_editor_token_t* name_token, const NanoVDBContext& ctx);
    void add_nanovdb(pnanovdb_editor_token_t* scene_token, pnanovdb_editor_token_t* name_token, const NanoVDBContext& ctx);
    NanoVDBContext* get_nanovdb(pnanovdb_editor_token_t* scene_token, pnanovdb_editor_token_t* name_token);
    const NanoVDBContext* get_nanovdb(pnanovdb_editor_token_t* scene_token, pnanovdb_editor_token_t* name_token) const;
    std::map<uint64_t, NanoVDBContext>& get_nanovdbs();
    const std::map<uint64_t, NanoVDBContext>& get_nanovdbs() const;

    std::string add_nanovdb_view(pnanovdb_compute_array_t* nanovdb_array, void* shader_params);

    // Scene-scoped helpers to add views and set current view for that scene
    void add_nanovdb_to_scene(pnanovdb_editor_token_t* scene_token,
                              pnanovdb_editor_token_t* name_token,
                              pnanovdb_compute_array_t* nanovdb_array,
                              void* shader_params);
    void add_gaussian_to_scene(pnanovdb_editor_token_t* scene_token,
                               pnanovdb_editor_token_t* name_token,
                               pnanovdb_raster_gaussian_data_t* gaussian_data,
                               pnanovdb_raster_shader_params_t* shader_params);

    // Remove view by name (tries all types: camera, nanovdb, gaussian)
    // scene_token = nullptr uses current scene
    bool remove(pnanovdb_editor_token_t* scene_token, pnanovdb_editor_token_t* name_token);

    // Remove entire scene
    bool remove_scene(pnanovdb_editor_token_t* scene_token);

    // Set current render settings so SceneView can derive is_y_up at scene creation time
    void set_render_settings(pnanovdb_imgui_settings_render_t* settings)
    {
        m_imgui_settings = settings;
    }

private:
    // Internal helper methods
    SceneViewData* get_scene(pnanovdb_editor_token_t* scene_token);
    const SceneViewData* get_scene(pnanovdb_editor_token_t* scene_token) const;

    template <typename MapType>
    bool remove_from_map(MapType SceneViewData::*map_member,
                         pnanovdb_editor_token_t* scene_token,
                         pnanovdb_editor_token_t* name_token);

    void set_view(SceneViewData* scene, pnanovdb_editor_token_t* view_token);

    // Non-owning pointer to current render settings (provided by EditorScene)
    pnanovdb_imgui_settings_render_t* m_imgui_settings = nullptr;

    // Map of scene ID -> SceneViewData
    std::map<uint64_t, SceneViewData> m_scene_view_data;
    pnanovdb_editor_token_t* m_current_scene_token = nullptr; // Currently active scene
    pnanovdb_editor_token_t* m_default_scene_token = nullptr; // Default scene for backward compatibility
    pnanovdb_editor_token_t* m_viewport_camera_token = nullptr; // Cached token for viewport camera (initialized once)
};

} // namespace pnanovdb_editor
