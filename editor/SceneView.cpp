// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/editor/SceneView.cpp

    \author Petra Hapalova

    \brief  Views in the editor - now with multi-scene support
*/

#include "SceneView.h"
#include "EditorToken.h"
#include "ImguiInstance.h"

#include <filesystem>

namespace pnanovdb_editor
{

SceneView::SceneView()
{
}

template <typename MapType>
static auto find_in_map(MapType& map, uint64_t token_id) -> decltype(&map.begin()->second)
{
    auto it = map.find(token_id);
    return (it != map.end()) ? &it->second : nullptr;
}

// Scene management
SceneViewData* SceneView::get_or_create_scene(pnanovdb_editor_token_t* scene_token)
{
    if (!scene_token)
    {
        // When in viewer profile, do not auto-create a default scene
        if (m_imgui_settings && m_imgui_settings->ui_profile_name &&
            strcmp(m_imgui_settings->ui_profile_name, imgui_instance_user::s_viewer_profile_name) == 0)
        {
            return nullptr;
        }

        // Use default scene if no token provided
        if (!m_default_scene_token)
        {
            m_default_scene_token = EditorToken::getInstance().getToken(DEFAULT_SCENE_NAME);
        }
        scene_token = m_default_scene_token;
    }

    auto it = m_scene_view_data.find(scene_token->id);
    if (it != m_scene_view_data.end())
    {
        return &it->second; // Scene already exists
    }

    // New scene being created - initialize it with a default camera
    SceneViewData& new_scene = m_scene_view_data[scene_token->id];

    // Create the initial camera and mark it as viewport
    pnanovdb_editor_token_t* camera_token = add_new_camera(scene_token, nullptr);
    if (camera_token)
    {
        new_scene.viewport_camera_token_id = camera_token->id;
    }

    return &new_scene;
}

SceneViewData* SceneView::get_scene(pnanovdb_editor_token_t* scene_token)
{
    if (!scene_token)
        return nullptr;

    auto it = m_scene_view_data.find(scene_token->id);
    return (it != m_scene_view_data.end()) ? &it->second : nullptr;
}

const SceneViewData* SceneView::get_scene(pnanovdb_editor_token_t* scene_token) const
{
    if (!scene_token)
        return nullptr;

    auto it = m_scene_view_data.find(scene_token->id);
    return (it != m_scene_view_data.end()) ? &it->second : nullptr;
}

SceneViewData* SceneView::get_current_scene()
{
    return get_scene(m_current_scene_token);
}

const SceneViewData* SceneView::get_current_scene() const
{
    pnanovdb_editor_token_t* token = m_current_scene_token ? m_current_scene_token : m_default_scene_token;
    return get_scene(token);
}

void SceneView::set_current_scene(pnanovdb_editor_token_t* scene_token)
{
    m_current_scene_token = scene_token;
}

std::vector<pnanovdb_editor_token_t*> SceneView::get_all_scene_tokens() const
{
    std::vector<pnanovdb_editor_token_t*> tokens;
    for (const auto& pair : m_scene_view_data)
    {
        // Retrieve token by ID from EditorToken singleton
        pnanovdb_editor_token_t* token = EditorToken::getInstance().getTokenById(pair.first);
        if (token)
        {
            tokens.push_back(token);
        }
    }
    return tokens;
}

// Cameras
void SceneView::add_camera(pnanovdb_editor_token_t* name_token, const CameraViewContext& camera)
{
    if (!name_token)
        return;
    SceneViewData* scene = get_current_scene();
    if (scene)
    {
        scene->cameras[name_token->id] = camera;
    }
}

void SceneView::add_camera(pnanovdb_editor_token_t* scene_token,
                           pnanovdb_editor_token_t* name_token,
                           const CameraViewContext& camera)
{
    if (!name_token)
        return;
    SceneViewData* scene = get_or_create_scene(scene_token);
    if (scene)
    {
        scene->cameras[name_token->id] = camera;
        set_current_scene(scene_token);
    }
}

pnanovdb_camera_view_t* SceneView::get_camera(pnanovdb_editor_token_t* name_token) const
{
    if (!name_token)
        return nullptr;
    const SceneViewData* scene = get_current_scene();
    if (!scene)
        return nullptr;
    auto it = scene->cameras.find(name_token->id);
    return (it != scene->cameras.end()) ? it->second.camera_view.get() : nullptr;
}

pnanovdb_camera_view_t* SceneView::get_camera(pnanovdb_editor_token_t* scene_token,
                                              pnanovdb_editor_token_t* name_token) const
{
    if (!name_token)
        return nullptr;
    const SceneViewData* scene = get_scene(scene_token);
    if (!scene)
        return nullptr;
    auto it = scene->cameras.find(name_token->id);
    return (it != scene->cameras.end()) ? it->second.camera_view.get() : nullptr;
}

const std::map<uint64_t, CameraViewContext>& SceneView::get_cameras() const
{
    const SceneViewData* scene = get_current_scene();
    static const std::map<uint64_t, CameraViewContext> empty_map;
    return scene ? scene->cameras : empty_map;
}

pnanovdb_editor_token_t* SceneView::add_new_camera(pnanovdb_editor_token_t* scene_token, const char* name)
{
    SceneViewData* scene = scene_token ? get_or_create_scene(scene_token) : get_current_scene();
    if (!scene)
        return nullptr;

    // Generate unique name if not provided
    std::string camera_name = name ? name : "";
    if (camera_name.empty())
    {
        int counter = scene->unnamed_counter++;
        // First camera is "Camera", subsequent are "Camera 1", "Camera 2", etc.
        camera_name = (counter == 0) ? "Camera" : "Camera " + std::to_string(counter);
    }

    // Create token for the name
    pnanovdb_editor_token_t* name_token = EditorToken::getInstance().getToken(camera_name.c_str());
    if (!name_token)
        return nullptr;

    // Check if name already exists, append number to make unique
    if (scene->cameras.find(name_token->id) != scene->cameras.end())
    {
        static constexpr int kMaxSuffixAttempts = 10000;
        int suffix = 1;
        while (suffix <= kMaxSuffixAttempts)
        {
            std::string new_name = camera_name + " " + std::to_string(suffix++);
            name_token = EditorToken::getInstance().getToken(new_name.c_str());
            if (!name_token)
                return nullptr; // Token generation failed
            if (scene->cameras.find(name_token->id) == scene->cameras.end())
            {
                break;
            }
        }
        if (suffix > kMaxSuffixAttempts)
            return nullptr; // Could not generate unique name within limit
    }

    // Create camera context with its own config/state storage
    CameraViewContext camera_ctx;
    camera_ctx.camera_config = std::make_shared<pnanovdb_camera_config_t>();
    pnanovdb_camera_config_default(camera_ctx.camera_config.get());
    camera_ctx.camera_config->far_plane = 100.0f; // Non-viewport cameras use bounded far plane for viz

    const pnanovdb_bool_t is_y_up = (m_imgui_settings ? m_imgui_settings->is_y_up : PNANOVDB_TRUE);
    camera_ctx.camera_state = std::make_shared<pnanovdb_camera_state_t>();
    pnanovdb_camera_state_default(camera_ctx.camera_state.get(), is_y_up);

    camera_ctx.camera_view = std::make_shared<pnanovdb_camera_view_t>();
    pnanovdb_camera_view_default(camera_ctx.camera_view.get());
    camera_ctx.camera_view->name = name_token;
    camera_ctx.camera_view->configs = camera_ctx.camera_config.get();
    camera_ctx.camera_view->states = camera_ctx.camera_state.get();
    camera_ctx.camera_view->num_cameras = 1;
    camera_ctx.camera_view->is_visible = PNANOVDB_FALSE; // New cameras are invisible by default

    scene->cameras[name_token->id] = std::move(camera_ctx);

    return name_token;
}

// Viewport camera methods
pnanovdb_editor_token_t* SceneView::get_viewport_camera_token() const
{
    const SceneViewData* scene = get_current_scene();
    if (!scene || scene->viewport_camera_token_id == 0)
        return nullptr;
    return EditorToken::getInstance().getTokenById(scene->viewport_camera_token_id);
}

pnanovdb_editor_token_t* SceneView::get_viewport_camera_token(pnanovdb_editor_token_t* scene_token) const
{
    const SceneViewData* scene = get_scene(scene_token);
    if (!scene || scene->viewport_camera_token_id == 0)
        return nullptr;
    return EditorToken::getInstance().getTokenById(scene->viewport_camera_token_id);
}

void SceneView::set_viewport_camera(pnanovdb_editor_token_t* camera_token)
{
    SceneViewData* scene = get_current_scene();
    if (scene && camera_token)
    {
        // Verify camera exists in this scene
        if (scene->cameras.find(camera_token->id) != scene->cameras.end())
        {
            scene->viewport_camera_token_id = camera_token->id;
        }
    }
}

void SceneView::set_viewport_camera(pnanovdb_editor_token_t* scene_token, pnanovdb_editor_token_t* camera_token)
{
    SceneViewData* scene = get_scene(scene_token);
    if (scene && camera_token)
    {
        // Verify camera exists in this scene
        if (scene->cameras.find(camera_token->id) != scene->cameras.end())
        {
            scene->viewport_camera_token_id = camera_token->id;
        }
    }
}

bool SceneView::is_viewport_camera(pnanovdb_editor_token_t* camera_token) const
{
    const SceneViewData* scene = get_current_scene();
    if (!scene || !camera_token)
        return false;
    return scene->viewport_camera_token_id == camera_token->id;
}

bool SceneView::is_viewport_camera(pnanovdb_editor_token_t* scene_token, pnanovdb_editor_token_t* camera_token) const
{
    const SceneViewData* scene = get_scene(scene_token);
    if (!scene || !camera_token)
        return false;
    return scene->viewport_camera_token_id == camera_token->id;
}

void SceneView::set_view(SceneViewData* scene, pnanovdb_editor_token_t* view_token)
{
    if (scene)
    {
        scene->current_view_token_id = view_token ? view_token->id : 0;
        scene->current_view_epoch.fetch_add(1, std::memory_order_relaxed);
    }
}

// Current view selection
void SceneView::set_current_view(pnanovdb_editor_token_t* view_token)
{
    SceneViewData* scene = get_current_scene();
    set_view(scene, view_token);
}

void SceneView::set_current_view(pnanovdb_editor_token_t* scene_token, pnanovdb_editor_token_t* view_token)
{
    SceneViewData* scene = get_or_create_scene(scene_token);
    set_view(scene, view_token);
}

pnanovdb_editor_token_t* SceneView::get_current_view() const
{
    const SceneViewData* scene = get_current_scene();
    if (!scene || scene->current_view_token_id == 0)
        return nullptr;
    return EditorToken::getInstance().getTokenById(scene->current_view_token_id);
}

pnanovdb_editor_token_t* SceneView::get_current_view(pnanovdb_editor_token_t* scene_token) const
{
    const SceneViewData* scene = get_scene(scene_token);
    if (!scene || scene->current_view_token_id == 0)
        return nullptr;
    return EditorToken::getInstance().getTokenById(scene->current_view_token_id);
}

uint64_t SceneView::get_current_view_epoch() const
{
    const SceneViewData* scene = get_current_scene();
    return scene ? scene->current_view_epoch.load(std::memory_order_relaxed) : 0;
}

uint64_t SceneView::get_current_view_epoch(pnanovdb_editor_token_t* scene_token) const
{
    const SceneViewData* scene = get_scene(scene_token);
    return scene ? scene->current_view_epoch.load(std::memory_order_relaxed) : 0;
}

// Gaussians
void SceneView::add_gaussian(pnanovdb_editor_token_t* name_token, const GaussianDataContext& ctx)
{
    if (!name_token)
        return;
    SceneViewData* scene = get_current_scene();
    if (scene)
    {
        scene->gaussians[name_token->id] = ctx;
    }
}

void SceneView::add_gaussian(pnanovdb_editor_token_t* scene_token,
                             pnanovdb_editor_token_t* name_token,
                             const GaussianDataContext& ctx)
{
    if (!name_token)
        return;
    SceneViewData* scene = get_or_create_scene(scene_token);
    if (scene)
    {
        scene->gaussians[name_token->id] = ctx;
        scene->last_added_view_token_id = name_token->id;
        set_current_scene(scene_token);
    }
}

GaussianDataContext* SceneView::get_gaussian(pnanovdb_editor_token_t* scene_token, pnanovdb_editor_token_t* name_token)
{
    if (!name_token)
        return nullptr;
    SceneViewData* scene = get_scene(scene_token);
    return scene ? find_in_map(scene->gaussians, name_token->id) : nullptr;
}

const GaussianDataContext* SceneView::get_gaussian(pnanovdb_editor_token_t* scene_token,
                                                   pnanovdb_editor_token_t* name_token) const
{
    if (!name_token)
        return nullptr;
    const SceneViewData* scene = get_scene(scene_token);
    return scene ? find_in_map(scene->gaussians, name_token->id) : nullptr;
}

std::map<uint64_t, GaussianDataContext>& SceneView::get_gaussians()
{
    SceneViewData* scene = get_current_scene();
    static std::map<uint64_t, GaussianDataContext> empty_map;
    return scene ? scene->gaussians : empty_map;
}

const std::map<uint64_t, GaussianDataContext>& SceneView::get_gaussians() const
{
    const SceneViewData* scene = get_current_scene();
    static const std::map<uint64_t, GaussianDataContext> empty_map;
    return scene ? scene->gaussians : empty_map;
}

// NanoVDBs
void SceneView::add_nanovdb(pnanovdb_editor_token_t* name_token, const NanoVDBContext& ctx)
{
    if (!name_token)
        return;
    SceneViewData* scene = get_current_scene();
    if (scene)
    {
        scene->nanovdbs[name_token->id] = ctx;
    }
}

void SceneView::add_nanovdb(pnanovdb_editor_token_t* scene_token,
                            pnanovdb_editor_token_t* name_token,
                            const NanoVDBContext& ctx)
{
    if (!name_token)
        return;
    SceneViewData* scene = get_or_create_scene(scene_token);
    if (scene)
    {
        scene->nanovdbs[name_token->id] = ctx;
        scene->last_added_view_token_id = name_token->id;
        set_current_scene(scene_token);
    }
}

NanoVDBContext* SceneView::get_nanovdb(pnanovdb_editor_token_t* scene_token, pnanovdb_editor_token_t* name_token)
{
    if (!name_token)
        return nullptr;
    SceneViewData* scene = get_scene(scene_token);
    return scene ? find_in_map(scene->nanovdbs, name_token->id) : nullptr;
}

const NanoVDBContext* SceneView::get_nanovdb(pnanovdb_editor_token_t* scene_token, pnanovdb_editor_token_t* name_token) const
{
    if (!name_token)
        return nullptr;
    const SceneViewData* scene = get_scene(scene_token);
    return scene ? find_in_map(scene->nanovdbs, name_token->id) : nullptr;
}

std::map<uint64_t, NanoVDBContext>& SceneView::get_nanovdbs()
{
    SceneViewData* scene = get_current_scene();
    static std::map<uint64_t, NanoVDBContext> empty_map;
    return scene ? scene->nanovdbs : empty_map;
}

const std::map<uint64_t, NanoVDBContext>& SceneView::get_nanovdbs() const
{
    const SceneViewData* scene = get_current_scene();
    static const std::map<uint64_t, NanoVDBContext> empty_map;
    return scene ? scene->nanovdbs : empty_map;
}

template <typename MapType, typename ContextType, typename AddFunc>
static std::string add_view(MapType& map,
                            const std::string& prefix,
                            const std::string& input_name,
                            ContextType&& context,
                            AddFunc&& add_func,
                            int& unnamed_counter)
{
    // Generate view name or create unnamed
    std::string view_name = input_name;
    if (view_name.empty())
    {
        view_name = prefix + std::to_string(unnamed_counter++);
    }

    // Get or create token for the name
    pnanovdb_editor_token_t* name_token = EditorToken::getInstance().getToken(view_name.c_str());
    if (!name_token)
    {
        return "";
    }

    // Replace if already exists
    auto it = map.find(name_token->id);
    if (it != map.end())
    {
        map.erase(it);
    }

    // Add new view
    add_func(name_token, std::forward<ContextType>(context));

    return view_name;
}

std::string SceneView::add_nanovdb_view(pnanovdb_compute_array_t* nanovdb_array, void* shader_params)
{
    if (!nanovdb_array)
    {
        return "";
    }

    std::string view_name;
    if (nanovdb_array->filepath)
    {
        std::string filepath_stem = std::filesystem::path(nanovdb_array->filepath).stem().string();
        view_name = filepath_stem.c_str();
    }

    SceneViewData* scene = get_current_scene();
    if (!scene)
        return "";

    // Note: This function shouldn't be called with raw pointers - needs refactoring
    // For now, wrap in shared_ptr with no-op deleter since we don't own it
    view_name = add_view(
        scene->nanovdbs, "nanovdb_", view_name,
        NanoVDBContext{ std::shared_ptr<pnanovdb_compute_array_t>(nanovdb_array, [](pnanovdb_compute_array_t*) {}),
                        shader_params, nullptr },
        [this](pnanovdb_editor_token_t* name_token, NanoVDBContext&& ctx) { add_nanovdb(name_token, ctx); },
        scene->unnamed_counter);

    pnanovdb_editor_token_t* name_token = EditorToken::getInstance().getToken(view_name.c_str());
    if (name_token)
    {
        set_current_view(name_token);
    }

    return view_name;
}

void SceneView::add_nanovdb_to_scene(pnanovdb_editor_token_t* scene_token,
                                     pnanovdb_editor_token_t* name_token,
                                     pnanovdb_compute_array_t* nanovdb_array,
                                     void* shader_params)
{
    if (!scene_token || !name_token || !nanovdb_array)
    {
        return;
    }

    // Note: This function takes raw pointers but context needs shared_ptr
    // Wrap with no-op deleter since we don't own these
    NanoVDBContext context;
    context.nanovdb_array = std::shared_ptr<pnanovdb_compute_array_t>(nanovdb_array, [](pnanovdb_compute_array_t*) {});
    context.shader_params = shader_params;

    add_nanovdb(scene_token, name_token, context);
    set_current_view(scene_token, name_token);
}

void SceneView::add_gaussian_to_scene(pnanovdb_editor_token_t* scene_token,
                                      pnanovdb_editor_token_t* name_token,
                                      pnanovdb_raster_gaussian_data_t* gaussian_data,
                                      pnanovdb_raster_shader_params_t* shader_params)
{
    if (!scene_token || !name_token || !gaussian_data)
    {
        return;
    }

    // Note: This function takes raw pointers but context needs shared_ptr
    // Wrap with no-op deleter since we don't own these
    GaussianDataContext context;
    context.gaussian_data =
        std::shared_ptr<pnanovdb_raster_gaussian_data_t>(gaussian_data, [](pnanovdb_raster_gaussian_data_t*) {});
    context.shader_params = shader_params;

    add_gaussian(scene_token, name_token, context);
    set_current_view(scene_token, name_token);
}

template <typename MapType>
bool SceneView::remove_from_map(MapType SceneViewData::*map_member,
                                pnanovdb_editor_token_t* scene_token,
                                pnanovdb_editor_token_t* name_token)
{
    if (!name_token)
        return false;

    // If no scene_token provided, use current scene
    SceneViewData* scene = scene_token ? get_or_create_scene(scene_token) : get_current_scene();
    if (!scene)
        return false;

    return (scene->*map_member).erase(name_token->id) > 0;
}

// Remove view by name (tries all types: camera, nanovdb, gaussian)
bool SceneView::remove(pnanovdb_editor_token_t* scene_token, pnanovdb_editor_token_t* name_token)
{
    // Try to remove from all view types, return true if any succeeded
    bool removed = remove_from_map(&SceneViewData::cameras, scene_token, name_token);
    removed = remove_from_map(&SceneViewData::nanovdbs, scene_token, name_token) || removed;
    removed = remove_from_map(&SceneViewData::gaussians, scene_token, name_token) || removed;
    return removed;
}

pnanovdb_editor_token_t* SceneView::find_next_available_view(pnanovdb_editor_token_t* scene_token) const
{
    // Temporarily switch context to the target scene to reuse getters
    const SceneViewData* scene = get_scene(scene_token);
    if (!scene)
    {
        return nullptr;
    }

    // Prefer NanoVDBs, then Gaussians
    if (!scene->nanovdbs.empty())
    {
        return EditorToken::getInstance().getTokenById(scene->nanovdbs.begin()->first);
    }
    if (!scene->gaussians.empty())
    {
        return EditorToken::getInstance().getTokenById(scene->gaussians.begin()->first);
    }
    return nullptr;
}

bool SceneView::remove_and_fix_current(pnanovdb_editor_token_t* scene_token,
                                       pnanovdb_editor_token_t* name_token,
                                       pnanovdb_editor_token_t** out_new_view)
{
    if (!name_token)
        return false;

    bool removed = remove(scene_token, name_token);
    if (!removed)
        return false;

    // If the removed was current, select a fallback
    pnanovdb_editor_token_t* current = get_current_view(scene_token);
    if (current && current->id == name_token->id)
    {
        pnanovdb_editor_token_t* next = find_next_available_view(scene_token);
        set_current_view(scene_token, next);
        if (out_new_view)
        {
            *out_new_view = next;
        }
    }
    else if (out_new_view)
    {
        *out_new_view = nullptr;
    }
    return true;
}

bool SceneView::remove_scene(pnanovdb_editor_token_t* scene_token)
{
    if (!scene_token)
        return false;

    auto it = m_scene_view_data.find(scene_token->id);
    if (it == m_scene_view_data.end())
        return false; // Scene not found

    SceneViewData& scene = it->second;

    // Note: We don't explicitly clear the maps here - let the destructor handle it
    // The maps contain raw pointers (not owned data), so clearing them manually
    // shouldn't be necessary and might interfere with proper destruction order

    // If this is the current scene, switch to another scene or set to nullptr
    if (m_current_scene_token && m_current_scene_token->id == scene_token->id)
    {
        // Try to switch to another scene
        if (m_scene_view_data.size() > 1)
        {
            // Find first scene that isn't the one being removed
            for (const auto& pair : m_scene_view_data)
            {
                if (pair.first != scene_token->id)
                {
                    m_current_scene_token = EditorToken::getInstance().getTokenById(pair.first);
                    break;
                }
            }
        }
        else
        {
            // No other scenes, set to default
            m_current_scene_token = m_default_scene_token;
        }
    }

    // Remove the scene from the map (this destroys the SceneViewData)
    m_scene_view_data.erase(it);
    return true;
}

} // namespace pnanovdb_editor
