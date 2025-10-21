// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/editor/EditorView.cpp

    \author Petra Hapalova

    \brief  Views in the editor - now with multi-scene support
*/

#include "EditorView.h"
#include "EditorToken.h"

#include <filesystem>

namespace pnanovdb_editor
{

template <typename MapType>
static auto find_in_map(MapType& map, const std::string& name) -> decltype(&map.begin()->second)
{
    auto it = map.find(name);
    return (it != map.end()) ? &it->second : nullptr;
}

// Scene management
SceneViewData* EditorView::get_or_create_scene(pnanovdb_editor_token_t* scene_token)
{
    if (!scene_token)
    {
        // Use default scene if no token provided
        if (!m_default_scene_token)
        {
            m_default_scene_token = EditorToken::getInstance().getToken("default");
        }
        scene_token = m_default_scene_token;
    }

    return &m_scene_view_data[scene_token->id];
}

SceneViewData* EditorView::get_scene(pnanovdb_editor_token_t* scene_token)
{
    if (!scene_token)
        return nullptr;

    auto it = m_scene_view_data.find(scene_token->id);
    return (it != m_scene_view_data.end()) ? &it->second : nullptr;
}

const SceneViewData* EditorView::get_scene(pnanovdb_editor_token_t* scene_token) const
{
    if (!scene_token)
        return nullptr;

    auto it = m_scene_view_data.find(scene_token->id);
    return (it != m_scene_view_data.end()) ? &it->second : nullptr;
}

SceneViewData* EditorView::get_current_scene()
{
    return get_or_create_scene(m_current_scene_token);
}

const SceneViewData* EditorView::get_current_scene() const
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

void EditorView::set_current_scene(pnanovdb_editor_token_t* scene_token)
{
    m_current_scene_token = scene_token;
}

std::vector<pnanovdb_editor_token_t*> EditorView::get_all_scene_tokens() const
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
void EditorView::add_camera(const std::string& name, pnanovdb_camera_view_t* camera)
{
    SceneViewData* scene = get_current_scene();
    if (scene)
    {
        scene->cameras[name] = camera;
    }
}

void EditorView::add_camera(pnanovdb_editor_token_t* scene_token, const std::string& name, pnanovdb_camera_view_t* camera)
{
    SceneViewData* scene = get_or_create_scene(scene_token);
    if (scene)
    {
        scene->cameras[name] = camera;
    }
}

pnanovdb_camera_view_t* EditorView::get_camera(const std::string& name) const
{
    const SceneViewData* scene = get_current_scene();
    if (!scene)
        return nullptr;

    auto it = scene->cameras.find(name);
    return (it != scene->cameras.end()) ? it->second : nullptr;
}

pnanovdb_camera_view_t* EditorView::get_camera(pnanovdb_editor_token_t* scene_token, const std::string& name) const
{
    const SceneViewData* scene = get_scene(scene_token);
    if (!scene)
        return nullptr;

    auto it = scene->cameras.find(name);
    return (it != scene->cameras.end()) ? it->second : nullptr;
}

std::map<std::string, pnanovdb_camera_view_t*>& EditorView::get_cameras()
{
    SceneViewData* scene = get_current_scene();
    static std::map<std::string, pnanovdb_camera_view_t*> empty_map;
    return scene ? scene->cameras : empty_map;
}

const std::map<std::string, pnanovdb_camera_view_t*>& EditorView::get_cameras() const
{
    const SceneViewData* scene = get_current_scene();
    static const std::map<std::string, pnanovdb_camera_view_t*> empty_map;
    return scene ? scene->cameras : empty_map;
}

// Current view selection
void EditorView::set_current_view(const std::string& view_name)
{
    SceneViewData* scene = get_current_scene();
    if (scene)
    {
        scene->current_view = view_name;
        scene->current_view_epoch.fetch_add(1, std::memory_order_relaxed);
    }
}

void EditorView::set_current_view(pnanovdb_editor_token_t* scene_token, const std::string& view_name)
{
    SceneViewData* scene = get_or_create_scene(scene_token);
    if (scene)
    {
        scene->current_view = view_name;
        scene->current_view_epoch.fetch_add(1, std::memory_order_relaxed);
    }
}

const std::string& EditorView::get_current_view() const
{
    const SceneViewData* scene = get_current_scene();
    static const std::string empty_string;
    return scene ? scene->current_view : empty_string;
}

const std::string& EditorView::get_current_view(pnanovdb_editor_token_t* scene_token) const
{
    const SceneViewData* scene = get_scene(scene_token);
    static const std::string empty_string;
    return scene ? scene->current_view : empty_string;
}

uint64_t EditorView::get_current_view_epoch() const
{
    const SceneViewData* scene = get_current_scene();
    return scene ? scene->current_view_epoch.load(std::memory_order_relaxed) : 0;
}

uint64_t EditorView::get_current_view_epoch(pnanovdb_editor_token_t* scene_token) const
{
    const SceneViewData* scene = get_scene(scene_token);
    return scene ? scene->current_view_epoch.load(std::memory_order_relaxed) : 0;
}

// Gaussians
void EditorView::add_gaussian(const std::string& name, const GaussianDataContext& ctx)
{
    SceneViewData* scene = get_current_scene();
    if (scene)
    {
        scene->gaussians[name] = ctx;
    }
}

void EditorView::add_gaussian(pnanovdb_editor_token_t* scene_token, const std::string& name, const GaussianDataContext& ctx)
{
    SceneViewData* scene = get_or_create_scene(scene_token);
    if (scene)
    {
        scene->gaussians[name] = ctx;
    }
}

GaussianDataContext* EditorView::get_gaussian(const std::string& name)
{
    SceneViewData* scene = get_current_scene();
    return scene ? find_in_map(scene->gaussians, name) : nullptr;
}

GaussianDataContext* EditorView::get_gaussian(pnanovdb_editor_token_t* scene_token, const std::string& name)
{
    SceneViewData* scene = get_scene(scene_token);
    return scene ? find_in_map(scene->gaussians, name) : nullptr;
}

const GaussianDataContext* EditorView::get_gaussian(const std::string& name) const
{
    const SceneViewData* scene = get_current_scene();
    return scene ? find_in_map(scene->gaussians, name) : nullptr;
}

const GaussianDataContext* EditorView::get_gaussian(pnanovdb_editor_token_t* scene_token, const std::string& name) const
{
    const SceneViewData* scene = get_scene(scene_token);
    return scene ? find_in_map(scene->gaussians, name) : nullptr;
}

std::map<std::string, GaussianDataContext>& EditorView::get_gaussians()
{
    SceneViewData* scene = get_current_scene();
    static std::map<std::string, GaussianDataContext> empty_map;
    return scene ? scene->gaussians : empty_map;
}

const std::map<std::string, GaussianDataContext>& EditorView::get_gaussians() const
{
    const SceneViewData* scene = get_current_scene();
    static const std::map<std::string, GaussianDataContext> empty_map;
    return scene ? scene->gaussians : empty_map;
}

// NanoVDBs
void EditorView::add_nanovdb(const std::string& name, const NanoVDBContext& ctx)
{
    SceneViewData* scene = get_current_scene();
    if (scene)
    {
        scene->nanovdbs[name] = ctx;
    }
}

void EditorView::add_nanovdb(pnanovdb_editor_token_t* scene_token, const std::string& name, const NanoVDBContext& ctx)
{
    SceneViewData* scene = get_or_create_scene(scene_token);
    if (scene)
    {
        scene->nanovdbs[name] = ctx;
    }
}

NanoVDBContext* EditorView::get_nanovdb(const std::string& name)
{
    SceneViewData* scene = get_current_scene();
    return scene ? find_in_map(scene->nanovdbs, name) : nullptr;
}

NanoVDBContext* EditorView::get_nanovdb(pnanovdb_editor_token_t* scene_token, const std::string& name)
{
    SceneViewData* scene = get_scene(scene_token);
    return scene ? find_in_map(scene->nanovdbs, name) : nullptr;
}

const NanoVDBContext* EditorView::get_nanovdb(const std::string& name) const
{
    const SceneViewData* scene = get_current_scene();
    return scene ? find_in_map(scene->nanovdbs, name) : nullptr;
}

const NanoVDBContext* EditorView::get_nanovdb(pnanovdb_editor_token_t* scene_token, const std::string& name) const
{
    const SceneViewData* scene = get_scene(scene_token);
    return scene ? find_in_map(scene->nanovdbs, name) : nullptr;
}

std::map<std::string, NanoVDBContext>& EditorView::get_nanovdbs()
{
    SceneViewData* scene = get_current_scene();
    static std::map<std::string, NanoVDBContext> empty_map;
    return scene ? scene->nanovdbs : empty_map;
}

const std::map<std::string, NanoVDBContext>& EditorView::get_nanovdbs() const
{
    const SceneViewData* scene = get_current_scene();
    static const std::map<std::string, NanoVDBContext> empty_map;
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

    // Replace if already exists
    auto it = map.find(view_name);
    if (it != map.end())
    {
        map.erase(it);
    }

    // Add new view
    add_func(view_name, std::forward<ContextType>(context));

    return view_name;
}

std::string EditorView::add_nanovdb_view(pnanovdb_compute_array_t* nanovdb_array, void* shader_params)
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
        [this](const std::string& name, NanoVDBContext&& ctx) { add_nanovdb(name, ctx); }, scene->unnamed_counter);

    set_current_view(view_name);

    return view_name;
}

std::string EditorView::add_gaussian_view(pnanovdb_raster_context_t* raster_ctx,
                                          pnanovdb_raster_gaussian_data_t* gaussian_data,
                                          pnanovdb_raster_shader_params_t* shader_params)
{
    if (!gaussian_data || !raster_ctx || !shader_params)
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
        scene->gaussians, "gaussian_", view_name, GaussianDataContext{ raster_ctx, gaussian_data, shader_params },
        [this](const std::string& name, GaussianDataContext&& ctx) { add_gaussian(name, ctx); }, scene->unnamed_counter);

    set_current_view(view_name);

    return view_name;
}

} // namespace pnanovdb_editor
