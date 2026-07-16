// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   editor/PipelineParams.h

    \author Petra Hapalova

    \brief
*/

#pragma once

#include "ParamWidget.h"
#include "PipelineTypes.h"
#include "nanovdb_editor/putil/Reflect.h"

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <map>
#include <string>

namespace pnanovdb_editor
{
inline constexpr const char* PIPELINE_PARAM_JSON = "PipelineParams";

struct GaussianVoxelizeParams
{
    float voxels_per_unit = k_default_voxels_per_unit;
};

struct MeshLoadParams
{
    float inflation_radius = 0.f; //!< 0 = auto for line-based renders
    pnanovdb_uint32_t resolution = k_default_bvh_resolution; //!< 1..k_max_bvh_resolution
    pnanovdb_uint32_t show_debug = 0u; //!< nonzero -> triangles_debug_render
};

#define PNANOVDB_REFLECT_TYPE GaussianVoxelizeParams
PNANOVDB_REFLECT_BEGIN()
PNANOVDB_REFLECT_VALUE(float, voxels_per_unit, 0, 0)
PNANOVDB_REFLECT_END(0)
#undef PNANOVDB_REFLECT_TYPE

PNANOVDB_REFLECT_STRUCT_OPAQUE_IMPL(MeshLoadParams)

namespace detail
{
template <typename Params, typename Field>
inline Field params_field_get(const pnanovdb_pipeline_params_t* params, Field Params::*member, Field fallback)
{
    if (!params || !params->data || params->size < sizeof(Params))
    {
        return fallback;
    }
    return static_cast<const Params*>(params->data)->*member;
}

template <typename Params, typename Field>
inline bool params_field_set(pnanovdb_pipeline_params_t* params, Field Params::*member, Field value)
{
    if (!params || !params->data || params->size < sizeof(Params))
    {
        return false;
    }
    static_cast<Params*>(params->data)->*member = value;
    return true;
}
} // namespace detail

inline float voxels_per_unit_clamp(float value)
{
    if (!std::isfinite(value))
    {
        return k_default_voxels_per_unit;
    }
    return std::clamp(value, 1.0f, 512.0f);
}

inline float pipeline_params_get_voxels_per_unit(const pnanovdb_pipeline_params_t* params)
{
    return voxels_per_unit_clamp(
        detail::params_field_get(params, &GaussianVoxelizeParams::voxels_per_unit, k_default_voxels_per_unit));
}

inline bool pipeline_params_set_voxels_per_unit(pnanovdb_pipeline_params_t* params, float value)
{
    return detail::params_field_set(params, &GaussianVoxelizeParams::voxels_per_unit, voxels_per_unit_clamp(value));
}

inline float pipeline_params_get_mesh_load_inflation_radius(const pnanovdb_pipeline_params_t* params)
{
    return detail::params_field_get(params, &MeshLoadParams::inflation_radius, 0.f);
}

inline bool pipeline_params_set_mesh_load_inflation_radius(pnanovdb_pipeline_params_t* params, float value)
{
    return detail::params_field_set(params, &MeshLoadParams::inflation_radius, value);
}

inline pnanovdb_uint32_t pipeline_params_get_mesh_load_resolution(const pnanovdb_pipeline_params_t* params)
{
    return detail::params_field_get(params, &MeshLoadParams::resolution, k_default_bvh_resolution);
}

inline bool pipeline_params_set_mesh_load_resolution(pnanovdb_pipeline_params_t* params, pnanovdb_uint32_t value)
{
    return detail::params_field_set(params, &MeshLoadParams::resolution, value);
}

inline bool pipeline_params_get_mesh_load_show_debug(const pnanovdb_pipeline_params_t* params)
{
    return detail::params_field_get<MeshLoadParams, pnanovdb_uint32_t>(params, &MeshLoadParams::show_debug, 0u) != 0u;
}

inline bool pipeline_params_set_mesh_load_show_debug(pnanovdb_pipeline_params_t* params, bool value)
{
    return detail::params_field_set<MeshLoadParams, pnanovdb_uint32_t>(
        params, &MeshLoadParams::show_debug, value ? 1u : 0u);
}

class PipelineParams
{
public:
    struct EditResult
    {
        bool any_edited = false;
        bool any_active = false;
        bool any_committed = false;
    };

    EditResult render(const pnanovdb_reflect_data_type_t* data_type,
                      const char* hints_name,
                      unsigned char* data,
                      size_t size,
                      const char* id_suffix);

    bool primary_field(const pnanovdb_reflect_data_type_t* data_type,
                       const char* hints_name,
                       const unsigned char* data,
                       size_t size,
                       std::string& out_label,
                       double& out_value);

    void clear_cache();

private:
    const std::map<std::string, ParamWidgetHints>& hints_for(const char* hints_name);

    std::map<std::string, std::map<std::string, ParamWidgetHints>> hints_cache_;
};

} // namespace pnanovdb_editor
