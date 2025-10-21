// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/editor/EditorView.h

    \author Petra Hapalova

    \brief  Views in the editor
*/

#pragma once

#include "nanovdb_editor/putil/Editor.h"
#include "nanovdb_editor/putil/Raster.h"
#include "imgui/ImguiWindow.h"

#include <string>
#include <map>
#include <atomic>
#include <vector>

namespace pnanovdb_editor
{

// Default scene name used when no scene is specified
static constexpr const char* DEFAULT_SCENE_NAME = "default";

// Context data for a NanoVDB view
struct NanoVDBContext
{
    pnanovdb_compute_array_t* nanovdb_array = nullptr;
    void* shader_params = nullptr;
};

// Context data for a Gaussian splatting view
struct GaussianDataContext
{
    pnanovdb_raster_context_t* raster_ctx = nullptr;
    pnanovdb_raster_gaussian_data_t* gaussian_data = nullptr;
    pnanovdb_raster_shader_params_t* shader_params = nullptr;
};

// Scene view data for a specific scene ID - holds all views in that scene
struct SceneViewData
{
    std::map<std::string, pnanovdb_camera_view_t*> cameras;
    std::map<std::string, GaussianDataContext> gaussians;
    std::map<std::string, NanoVDBContext> nanovdbs;
    std::string current_view; // Current view selected in this scene
    std::atomic<uint64_t> current_view_epoch{ 0 };
    int unnamed_counter = 0;
};

// Views representing loaded scenes via editor's API, does not own the data
// Manages multiple scene view data (one per scene token)
class EditorView
{
public:
    EditorView() = default;

    // Scene management
    SceneViewData* get_or_create_scene(pnanovdb_editor_token_t* scene_token);
    SceneViewData* get_scene(pnanovdb_editor_token_t* scene_token);
    const SceneViewData* get_scene(pnanovdb_editor_token_t* scene_token) const;

    // Get current scene (defaults to default scene if none set)
    SceneViewData* get_current_scene();
    const SceneViewData* get_current_scene() const;

    // Set which scene is current
    void set_current_scene(pnanovdb_editor_token_t* scene_token);
    pnanovdb_editor_token_t* get_current_scene_token() const
    {
        return m_current_scene_token;
    }

    // Get all scene tokens
    std::vector<pnanovdb_editor_token_t*> get_all_scene_tokens() const;

    // Cameras (in current scene)
    void add_camera(const std::string& name, pnanovdb_camera_view_t* camera);
    void add_camera(pnanovdb_editor_token_t* scene_token, const std::string& name, pnanovdb_camera_view_t* camera);
    pnanovdb_camera_view_t* get_camera(const std::string& name) const;
    pnanovdb_camera_view_t* get_camera(pnanovdb_editor_token_t* scene_token, const std::string& name) const;
    std::map<std::string, pnanovdb_camera_view_t*>& get_cameras();
    const std::map<std::string, pnanovdb_camera_view_t*>& get_cameras() const;

    // Current view selection (in current scene)
    void set_current_view(const std::string& view_name);
    void set_current_view(pnanovdb_editor_token_t* scene_token, const std::string& view_name);
    const std::string& get_current_view() const;
    const std::string& get_current_view(pnanovdb_editor_token_t* scene_token) const;
    uint64_t get_current_view_epoch() const;
    uint64_t get_current_view_epoch(pnanovdb_editor_token_t* scene_token) const;

    // Gaussians (in current scene)
    void add_gaussian(const std::string& name, const GaussianDataContext& ctx);
    void add_gaussian(pnanovdb_editor_token_t* scene_token, const std::string& name, const GaussianDataContext& ctx);
    GaussianDataContext* get_gaussian(const std::string& name);
    GaussianDataContext* get_gaussian(pnanovdb_editor_token_t* scene_token, const std::string& name);
    const GaussianDataContext* get_gaussian(const std::string& name) const;
    const GaussianDataContext* get_gaussian(pnanovdb_editor_token_t* scene_token, const std::string& name) const;
    std::map<std::string, GaussianDataContext>& get_gaussians();
    const std::map<std::string, GaussianDataContext>& get_gaussians() const;

    // NanoVDBs (in current scene)
    void add_nanovdb(const std::string& name, const NanoVDBContext& ctx);
    void add_nanovdb(pnanovdb_editor_token_t* scene_token, const std::string& name, const NanoVDBContext& ctx);
    NanoVDBContext* get_nanovdb(const std::string& name);
    NanoVDBContext* get_nanovdb(pnanovdb_editor_token_t* scene_token, const std::string& name);
    const NanoVDBContext* get_nanovdb(const std::string& name) const;
    const NanoVDBContext* get_nanovdb(pnanovdb_editor_token_t* scene_token, const std::string& name) const;
    std::map<std::string, NanoVDBContext>& get_nanovdbs();
    const std::map<std::string, NanoVDBContext>& get_nanovdbs() const;

    std::string add_nanovdb_view(pnanovdb_compute_array_t* nanovdb_array, void* shader_params);
    std::string add_gaussian_view(pnanovdb_raster_context_t* raster_ctx,
                                  pnanovdb_raster_gaussian_data_t* gaussian_data,
                                  pnanovdb_raster_shader_params_t* shader_params);

    // Remove views
    bool remove_camera(const std::string& name);
    bool remove_camera(pnanovdb_editor_token_t* scene_token, const std::string& name);
    bool remove_nanovdb(const std::string& name);
    bool remove_nanovdb(pnanovdb_editor_token_t* scene_token, const std::string& name);
    bool remove_gaussian(const std::string& name);
    bool remove_gaussian(pnanovdb_editor_token_t* scene_token, const std::string& name);

private:
    template <typename MapType>
    bool remove_from_map(MapType SceneViewData::*map_member, const std::string& name);

    template <typename MapType>
    bool remove_from_map(MapType SceneViewData::*map_member, pnanovdb_editor_token_t* scene_token, const std::string& name);

    // Map of scene ID -> SceneViewData
    std::map<uint64_t, SceneViewData> m_scene_view_data;
    pnanovdb_editor_token_t* m_current_scene_token = nullptr; // Currently active scene
    pnanovdb_editor_token_t* m_default_scene_token = nullptr; // Default scene for backward compatibility
};

} // namespace pnanovdb_editor
