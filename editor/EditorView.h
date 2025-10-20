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

namespace pnanovdb_editor
{

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

// Views representing a loaded scene via editor's API, does not own the data
class EditorView
{
public:
    EditorView() = default;

    // Cameras
    void add_camera(const std::string& name, pnanovdb_camera_view_t* camera)
    {
        m_cameras[name] = camera;
    }
    pnanovdb_camera_view_t* get_camera(const std::string& name) const;
    std::map<std::string, pnanovdb_camera_view_t*>& get_cameras()
    {
        return m_cameras;
    }
    const std::map<std::string, pnanovdb_camera_view_t*>& get_cameras() const
    {
        return m_cameras;
    }

    // Current view selection
    void set_current_view(const std::string& view_name)
    {
        m_current_view = view_name;
        m_current_view_epoch.fetch_add(1, std::memory_order_relaxed);
    }
    const std::string& get_current_view() const
    {
        return m_current_view;
    }
    uint64_t get_current_view_epoch() const
    {
        return m_current_view_epoch.load(std::memory_order_relaxed);
    }

    // Gaussians
    void add_gaussian(const std::string& name, const GaussianDataContext& ctx)
    {
        m_gaussians[name] = ctx;
    }
    GaussianDataContext* get_gaussian(const std::string& name);
    const GaussianDataContext* get_gaussian(const std::string& name) const;
    std::map<std::string, GaussianDataContext>& get_gaussians()
    {
        return m_gaussians;
    }
    const std::map<std::string, GaussianDataContext>& get_gaussians() const
    {
        return m_gaussians;
    }

    // NanoVDBs
    void add_nanovdb(const std::string& name, const NanoVDBContext& ctx)
    {
        m_nanovdbs[name] = ctx;
    }
    NanoVDBContext* get_nanovdb(const std::string& name);
    const NanoVDBContext* get_nanovdb(const std::string& name) const;
    std::map<std::string, NanoVDBContext>& get_nanovdbs()
    {
        return m_nanovdbs;
    }
    const std::map<std::string, NanoVDBContext>& get_nanovdbs() const
    {
        return m_nanovdbs;
    }

    std::string add_nanovdb_view(pnanovdb_compute_array_t* nanovdb_array, void* shader_params);
    std::string add_gaussian_view(pnanovdb_raster_context_t* raster_ctx,
                                  pnanovdb_raster_gaussian_data_t* gaussian_data,
                                  pnanovdb_raster_shader_params_t* shader_params);

private:
    std::map<std::string, pnanovdb_camera_view_t*> m_cameras;
    std::map<std::string, GaussianDataContext> m_gaussians;
    std::map<std::string, NanoVDBContext> m_nanovdbs;
    int m_unnamed_counter = 0;
    std::string m_current_view; // Name of the currently selected view
    std::atomic<uint64_t> m_current_view_epoch{ 0 };
};

} // namespace pnanovdb_editor
