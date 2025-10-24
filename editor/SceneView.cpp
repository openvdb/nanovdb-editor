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
    // Initialize cached viewport camera token once
    m_viewport_camera_token = EditorToken::getInstance().getToken(imgui_instance_user::VIEWPORT_CAMERA);
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

    // Initialize default camera config and state with Y-up orientation
    pnanovdb_camera_config_default(&new_scene.default_camera_config);
    pnanovdb_camera_state_default(&new_scene.default_camera_state, PNANOVDB_TRUE);

    // Create viewport camera view pointing to this scene's camera
    pnanovdb_camera_view_default(&new_scene.default_camera_view);
    new_scene.default_camera_view.name = m_viewport_camera_token;
    new_scene.default_camera_view.configs = &new_scene.default_camera_config;
    new_scene.default_camera_view.states = &new_scene.default_camera_state;
    new_scene.default_camera_view.num_cameras = 1;
    new_scene.default_camera_view.is_visible = PNANOVDB_FALSE;

    // Add the default viewport camera to this scene
    new_scene.cameras[m_viewport_camera_token->id] = &new_scene.default_camera_view;

    return &new_scene;
}

void SceneView::init_scene_viewport_camera(pnanovdb_editor_token_t* scene_token, pnanovdb_camera_view_t* viewport_camera)
{
    SceneViewData* scene = get_scene(scene_token);
    if (scene)
    {
        scene->cameras[m_viewport_camera_token->id] = viewport_camera;
    }
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
    return get_or_create_scene(m_current_scene_token);
}

const SceneViewData* SceneView::get_current_scene() const
{
    if (!m_current_scene_token)
    {
        if (!m_default_scene_token)
        {
            return nullptr;
        }
        auto it = m_scene_view_data.find(m_default_scene_token->id);
        return (it != m_scene_view_data.end()) ? &it->second : nullptr;
    }

    auto it = m_scene_view_data.find(m_current_scene_token->id);
    return (it != m_scene_view_data.end()) ? &it->second : nullptr;
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
void SceneView::add_camera(pnanovdb_editor_token_t* name_token, pnanovdb_camera_view_t* camera)
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
                           pnanovdb_camera_view_t* camera)
{
    if (!name_token)
        return;
    SceneViewData* scene = get_or_create_scene(scene_token);
    if (scene)
    {
        scene->cameras[name_token->id] = camera;
        // Automatically select the scene when a camera is added to it
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
    return (it != scene->cameras.end()) ? it->second : nullptr;
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
    return (it != scene->cameras.end()) ? it->second : nullptr;
}

std::map<uint64_t, pnanovdb_camera_view_t*>& SceneView::get_cameras()
{
    SceneViewData* scene = get_current_scene();
    static std::map<uint64_t, pnanovdb_camera_view_t*> empty_map;
    return scene ? scene->cameras : empty_map;
}

const std::map<uint64_t, pnanovdb_camera_view_t*>& SceneView::get_cameras() const
{
    const SceneViewData* scene = get_current_scene();
    static const std::map<uint64_t, pnanovdb_camera_view_t*> empty_map;
    return scene ? scene->cameras : empty_map;
}

// Current view selection
void SceneView::set_current_view(pnanovdb_editor_token_t* view_token)
{
    SceneViewData* scene = get_current_scene();
    if (scene)
    {
        scene->current_view_token_id = view_token ? view_token->id : 0;
        scene->current_view_epoch.fetch_add(1, std::memory_order_relaxed);
    }
}

void SceneView::set_current_view(pnanovdb_editor_token_t* scene_token, pnanovdb_editor_token_t* view_token)
{
    SceneViewData* scene = get_or_create_scene(scene_token);
    if (scene)
    {
        scene->current_view_token_id = view_token ? view_token->id : 0;
        scene->current_view_epoch.fetch_add(1, std::memory_order_relaxed);
    }
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
        // Automatically select the scene when an object is added to it
        set_current_scene(scene_token);
    }
}

GaussianDataContext* SceneView::get_gaussian(pnanovdb_editor_token_t* name_token)
{
    if (!name_token)
        return nullptr;
    SceneViewData* scene = get_current_scene();
    return scene ? find_in_map(scene->gaussians, name_token->id) : nullptr;
}

GaussianDataContext* SceneView::get_gaussian(pnanovdb_editor_token_t* scene_token, pnanovdb_editor_token_t* name_token)
{
    if (!name_token)
        return nullptr;
    SceneViewData* scene = get_scene(scene_token);
    return scene ? find_in_map(scene->gaussians, name_token->id) : nullptr;
}

const GaussianDataContext* SceneView::get_gaussian(pnanovdb_editor_token_t* name_token) const
{
    if (!name_token)
        return nullptr;
    const SceneViewData* scene = get_current_scene();
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
        // Automatically select the scene when an object is added to it
        set_current_scene(scene_token);
    }
}

NanoVDBContext* SceneView::get_nanovdb(pnanovdb_editor_token_t* name_token)
{
    if (!name_token)
        return nullptr;
    SceneViewData* scene = get_current_scene();
    return scene ? find_in_map(scene->nanovdbs, name_token->id) : nullptr;
}

NanoVDBContext* SceneView::get_nanovdb(pnanovdb_editor_token_t* scene_token, pnanovdb_editor_token_t* name_token)
{
    if (!name_token)
        return nullptr;
    SceneViewData* scene = get_scene(scene_token);
    return scene ? find_in_map(scene->nanovdbs, name_token->id) : nullptr;
}

const NanoVDBContext* SceneView::get_nanovdb(pnanovdb_editor_token_t* name_token) const
{
    if (!name_token)
        return nullptr;
    const SceneViewData* scene = get_current_scene();
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

    view_name = add_view(
        scene->nanovdbs, "nanovdb_", view_name, NanoVDBContext{ nanovdb_array, shader_params },
        [this](pnanovdb_editor_token_t* name_token, NanoVDBContext&& ctx) { add_nanovdb(name_token, ctx); },
        scene->unnamed_counter);

    pnanovdb_editor_token_t* name_token = EditorToken::getInstance().getToken(view_name.c_str());
    if (name_token)
    {
        set_current_view(name_token);
    }

    return view_name;
}

std::string SceneView::add_gaussian_view(pnanovdb_raster_gaussian_data_t* gaussian_data,
                                         pnanovdb_raster_shader_params_t* shader_params)
{
    if (!gaussian_data || !shader_params)
    {
        return "";
    }

    std::string view_name;
    if (shader_params->name)
    {
        view_name = shader_params->name;
    }

    SceneViewData* scene = get_current_scene();
    if (!scene)
        return "";

    view_name = add_view(
        scene->gaussians, "gaussian_", view_name, GaussianDataContext{ gaussian_data, shader_params },
        [this](pnanovdb_editor_token_t* name_token, GaussianDataContext&& ctx) { add_gaussian(name_token, ctx); },
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

    NanoVDBContext context;
    context.nanovdb_array = nanovdb_array;
    context.shader_params = shader_params;

    add_nanovdb(scene_token, name_token, context);
    set_current_view(scene_token, name_token);
}

void SceneView::add_gaussian_to_scene(pnanovdb_editor_token_t* scene_token,
                                      pnanovdb_editor_token_t* name_token,
                                      pnanovdb_raster_gaussian_data_t* gaussian_data,
                                      pnanovdb_raster_shader_params_t* shader_params)
{
    if (!scene_token || !gaussian_data || !shader_params)
    {
        return;
    }

    GaussianDataContext context;
    context.gaussian_data = gaussian_data;
    context.shader_params = shader_params;

    add_gaussian(scene_token, name_token, context);
    set_current_view(scene_token, name_token);
}

template <typename MapType>
bool SceneView::remove_from_map(MapType SceneViewData::*map_member, pnanovdb_editor_token_t* name_token)
{
    if (!name_token)
        return false;
    SceneViewData* scene = get_current_scene();
    if (!scene)
        return false;
    return (scene->*map_member).erase(name_token->id) > 0;
}

template <typename MapType>
bool SceneView::remove_from_map(MapType SceneViewData::*map_member,
                                pnanovdb_editor_token_t* scene_token,
                                pnanovdb_editor_token_t* name_token)
{
    if (!name_token)
        return false;
    SceneViewData* scene = get_or_create_scene(scene_token);
    if (!scene)
        return false;
    return (scene->*map_member).erase(name_token->id) > 0;
}

// Remove views (current scene)
bool SceneView::remove_camera(pnanovdb_editor_token_t* name_token)
{
    return remove_from_map(&SceneViewData::cameras, name_token);
}

bool SceneView::remove_camera(pnanovdb_editor_token_t* scene_token, pnanovdb_editor_token_t* name_token)
{
    return remove_from_map(&SceneViewData::cameras, scene_token, name_token);
}

bool SceneView::remove_nanovdb(pnanovdb_editor_token_t* name_token)
{
    return remove_from_map(&SceneViewData::nanovdbs, name_token);
}

bool SceneView::remove_nanovdb(pnanovdb_editor_token_t* scene_token, pnanovdb_editor_token_t* name_token)
{
    return remove_from_map(&SceneViewData::nanovdbs, scene_token, name_token);
}

bool SceneView::remove_gaussian(pnanovdb_editor_token_t* name_token)
{
    return remove_from_map(&SceneViewData::gaussians, name_token);
}

bool SceneView::remove_gaussian(pnanovdb_editor_token_t* scene_token, pnanovdb_editor_token_t* name_token)
{
    return remove_from_map(&SceneViewData::gaussians, scene_token, name_token);
}

bool SceneView::remove_scene(pnanovdb_editor_token_t* scene_token)
{
    if (!scene_token)
        return false;

    auto it = m_scene_view_data.find(scene_token->id);
    if (it == m_scene_view_data.end())
        return false; // Scene not found

    SceneViewData& scene = it->second;

    // Log what we're about to remove
    Console::getInstance().addLog("[SceneView] Removing scene '%s': cameras=%zu, nanovdbs=%zu, gaussians=%zu",
                                  scene_token->str, scene.cameras.size(), scene.nanovdbs.size(), scene.gaussians.size());

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
                    Console::getInstance().addLog("[SceneView] Switched current scene to '%s'",
                                                  m_current_scene_token ? m_current_scene_token->str : "null");
                    break;
                }
            }
        }
        else
        {
            // No other scenes, set to default
            m_current_scene_token = m_default_scene_token;
            Console::getInstance().addLog("[SceneView] No other scenes, set to default");
        }
    }

    // Remove the scene from the map (this destroys the SceneViewData)
    m_scene_view_data.erase(it);
    Console::getInstance().addLog("[SceneView] Scene '%s' erased from map", scene_token->str);
    return true;
}

} // namespace pnanovdb_editor
