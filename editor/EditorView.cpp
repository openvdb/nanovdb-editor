// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/editor/EditorView.cpp

    \author Petra Hapalova

    \brief  Views in the editor
*/

#include "EditorView.h"

#include <filesystem>

namespace pnanovdb_editor
{

template <typename MapType>
static auto find_in_map(MapType& map, const std::string& name) -> decltype(&map.begin()->second)
{
    auto it = map.find(name);
    return (it != map.end()) ? &it->second : nullptr;
}

pnanovdb_camera_view_t* EditorView::get_camera(const std::string& name) const
{
    auto it = m_cameras.find(name);
    return (it != m_cameras.end()) ? it->second : nullptr;
}

GaussianDataContext* EditorView::get_gaussian(const std::string& name)
{
    return find_in_map(m_gaussians, name);
}

const GaussianDataContext* EditorView::get_gaussian(const std::string& name) const
{
    return find_in_map(m_gaussians, name);
}

NanoVDBContext* EditorView::get_nanovdb(const std::string& name)
{
    return find_in_map(m_nanovdbs, name);
}

const NanoVDBContext* EditorView::get_nanovdb(const std::string& name) const
{
    return find_in_map(m_nanovdbs, name);
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

    view_name = add_view(
        m_nanovdbs, "nanovdb_", view_name, NanoVDBContext{ nanovdb_array, shader_params },
        [this](const std::string& name, NanoVDBContext&& ctx) { add_nanovdb(name, ctx); }, m_unnamed_counter);

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

    view_name = add_view(
        m_gaussians, "gaussian_", view_name, GaussianDataContext{ raster_ctx, gaussian_data, shader_params },
        [this](const std::string& name, GaussianDataContext&& ctx) { add_gaussian(name, ctx); }, m_unnamed_counter);

    set_current_view(view_name);

    return view_name;
}

bool EditorView::remove_view(const std::string& name)
{
    bool removed = false;

    // Try to remove from cameras
    auto camera_it = m_cameras.find(name);
    if (camera_it != m_cameras.end())
    {
        m_cameras.erase(camera_it);
        removed = true;
    }

    // Try to remove from gaussians
    auto gaussian_it = m_gaussians.find(name);
    if (gaussian_it != m_gaussians.end())
    {
        m_gaussians.erase(gaussian_it);
        removed = true;
    }

    // Try to remove from nanovdbs
    auto nanovdb_it = m_nanovdbs.find(name);
    if (nanovdb_it != m_nanovdbs.end())
    {
        m_nanovdbs.erase(nanovdb_it);
        removed = true;
    }

    // If the removed view was the current view, clear it
    if (removed && m_current_view == name)
    {
        m_current_view.clear();
        m_current_view_epoch.fetch_add(1, std::memory_order_relaxed);
    }


    return removed;
}

} // namespace pnanovdb_editor
