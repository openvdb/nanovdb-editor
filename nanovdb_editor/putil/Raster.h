// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/putil/Raster.h

    \author Andrew Reidmeyer

    \brief  This file provides an interface for rasterization of voxels to NanoVDB.
*/

#ifndef NANOVDB_PUTILS_RASTER_H_HAS_BEEN_INCLUDED
#define NANOVDB_PUTILS_RASTER_H_HAS_BEEN_INCLUDED

#include "nanovdb_editor/putil/Compute.h"
#include "nanovdb_editor/putil/Camera.h"

/// ********************************* Raster ***************************************

struct pnanovdb_raster_context_t;
typedef struct pnanovdb_raster_context_t pnanovdb_raster_context_t;

struct pnanovdb_raster_gaussian_data_t;
typedef struct pnanovdb_raster_gaussian_data_t pnanovdb_raster_gaussian_data_t;

typedef struct pnanovdb_raster_shader_params_t
{
    float eps2d;
    float min_radius_2d;
    pnanovdb_uint32_t tile_size;
    pnanovdb_int32_t sh_degree_override;
    pnanovdb_uint32_t sh_stride_rgbrgbrgb_override;

    const pnanovdb_reflect_data_type_t* data_type;
    const char* name; // displayed in UI
} pnanovdb_raster_shader_params_t;

static const pnanovdb_raster_shader_params_t default_shader_params = {
    0.3f, // eps2d
    0.f, // min_radius_2d
    16u, // tile_size
    -1, // sh_degree override, <0 means loaded SH degree
    0, // sh_stride_rgbrgbrgb override, 0 means SH are packed rrr...ggg...bbb
    NULL, // data_type
    NULL // name
};

#define PNANOVDB_REFLECT_TYPE pnanovdb_raster_shader_params_t
PNANOVDB_REFLECT_BEGIN()
PNANOVDB_REFLECT_VALUE(float, eps2d, 0, 0)
PNANOVDB_REFLECT_VALUE(float, min_radius_2d, 0, 0)
PNANOVDB_REFLECT_VALUE(pnanovdb_uint32_t, tile_size, 0, 0)
PNANOVDB_REFLECT_VALUE(pnanovdb_int32_t, sh_degree_override, 0, 0)
PNANOVDB_REFLECT_VALUE(pnanovdb_uint32_t, sh_stride_rgbrgbrgb_override, 0, 0)
PNANOVDB_REFLECT_END(&default_shader_params)
#undef PNANOVDB_REFLECT_TYPE

typedef struct pnanovdb_raster_t
{
    PNANOVDB_REFLECT_INTERFACE();

    const pnanovdb_compute_t* compute;

    pnanovdb_raster_context_t*(PNANOVDB_ABI* create_context)(const pnanovdb_compute_t* compute,
                                                             pnanovdb_compute_queue_t* queue);

    void(PNANOVDB_ABI* destroy_context)(const pnanovdb_compute_t* compute,
                                        pnanovdb_compute_queue_t* queue,
                                        pnanovdb_raster_context_t* context);

    pnanovdb_raster_gaussian_data_t*(PNANOVDB_ABI* create_gaussian_data)(const pnanovdb_compute_t* compute,
                                                                         pnanovdb_compute_queue_t* queue,
                                                                         pnanovdb_raster_context_t* context,
                                                                         pnanovdb_compute_array_t* means,
                                                                         pnanovdb_compute_array_t* quaternions,
                                                                         pnanovdb_compute_array_t* scales,
                                                                         pnanovdb_compute_array_t* colors,
                                                                         pnanovdb_compute_array_t* sh_0,
                                                                         pnanovdb_compute_array_t* sh_n,
                                                                         pnanovdb_compute_array_t* opacities,
                                                                         pnanovdb_compute_array_t** shader_params_arrays,
                                                                         pnanovdb_raster_shader_params_t* raster_params);

    void(PNANOVDB_ABI* upload_gaussian_data)(const pnanovdb_compute_t* compute,
                                             pnanovdb_compute_queue_t* queue,
                                             pnanovdb_raster_context_t* context,
                                             pnanovdb_raster_gaussian_data_t* data);

    void(PNANOVDB_ABI* destroy_gaussian_data)(const pnanovdb_compute_t* compute,
                                              pnanovdb_compute_queue_t* queue,
                                              pnanovdb_raster_gaussian_data_t* data);

    void(PNANOVDB_ABI* raster_gaussian_2d)(const pnanovdb_compute_t* compute,
                                           pnanovdb_compute_queue_t* queue,
                                           pnanovdb_raster_context_t* context,
                                           pnanovdb_raster_gaussian_data_t* data,
                                           pnanovdb_compute_texture_t* color_2d,
                                           pnanovdb_uint32_t image_width,
                                           pnanovdb_uint32_t image_height,
                                           const pnanovdb_camera_mat_t* view,
                                           const pnanovdb_camera_mat_t* projection,
                                           const pnanovdb_raster_shader_params_t* shader_params);

    void(PNANOVDB_ABI* raster_gaussian_3d)(const pnanovdb_compute_t* compute,
                                           pnanovdb_compute_queue_t* queue,
                                           pnanovdb_raster_context_t* context,
                                           float voxel_size,
                                           pnanovdb_raster_gaussian_data_t* data,
                                           pnanovdb_compute_buffer_t* nanovdb_out,
                                           pnanovdb_uint64_t nanovdb_word_count,
                                           void* userdata);

    pnanovdb_compute_array_t*(PNANOVDB_ABI* raster_to_nanovdb)(const pnanovdb_compute_t* compute,
                                                               pnanovdb_compute_queue_t* queue,
                                                               float voxel_size,
                                                               pnanovdb_compute_array_t* means,
                                                               pnanovdb_compute_array_t* quaternions,
                                                               pnanovdb_compute_array_t* scales,
                                                               pnanovdb_compute_array_t* colors,
                                                               pnanovdb_compute_array_t* sh_0,
                                                               pnanovdb_compute_array_t* sh_n,
                                                               pnanovdb_compute_array_t* opacities,
                                                               pnanovdb_compute_array_t** shader_params_arrays,
                                                               pnanovdb_profiler_report_t profiler_report,
                                                               void* userdata);

    pnanovdb_bool_t(PNANOVDB_ABI* raster_file)(pnanovdb_raster_t* raster,
                                               const pnanovdb_compute_t* compute,
                                               pnanovdb_compute_queue_t* queue,
                                               const char* filename,
                                               float voxel_size,
                                               pnanovdb_compute_array_t** nanovdb_arr,
                                               pnanovdb_raster_gaussian_data_t** gaussian_data,
                                               pnanovdb_raster_context_t** raster_context,
                                               pnanovdb_compute_array_t** shader_params_arrays,
                                               pnanovdb_raster_shader_params_t* raster_params,
                                               pnanovdb_profiler_report_t profiler_report,
                                               void* userdata);

    pnanovdb_bool_t(PNANOVDB_ABI* raster_to_nanovdb_from_arrays)(pnanovdb_raster_t* raster,
                                                                 const pnanovdb_compute_t* compute,
                                                                 pnanovdb_compute_queue_t* queue,
                                                                 float voxel_size,
                                                                 pnanovdb_compute_array_t** arrays_gaussian, // means,
                                                                                                             // opacities,
                                                                                                             // quats,
                                                                                                             // scales,
                                                                                                             // sh
                                                                 pnanovdb_uint32_t array_count,
                                                                 pnanovdb_compute_array_t** out_nanovdb_arr);

    pnanovdb_bool_t(PNANOVDB_ABI* create_gaussian_data_from_desc)(pnanovdb_raster_t* raster,
                                                                  const pnanovdb_compute_t* compute,
                                                                  pnanovdb_compute_queue_t* queue,
                                                                  const struct pnanovdb_editor_gaussian_data_desc_t* desc,
                                                                  const char* name,
                                                                  pnanovdb_raster_gaussian_data_t** gaussian_data,
                                                                  pnanovdb_raster_shader_params_t* raster_params,
                                                                  pnanovdb_raster_context_t** raster_context);
} pnanovdb_raster_t;

#define PNANOVDB_REFLECT_TYPE pnanovdb_raster_t
PNANOVDB_REFLECT_BEGIN()
PNANOVDB_REFLECT_FUNCTION_POINTER(create_context, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(destroy_context, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(create_gaussian_data, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(upload_gaussian_data, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(destroy_gaussian_data, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(raster_gaussian_2d, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(raster_gaussian_3d, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(raster_to_nanovdb, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(raster_file, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(raster_to_nanovdb_from_arrays, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(create_gaussian_data_from_desc, 0, 0)
PNANOVDB_REFLECT_POINTER(pnanovdb_compute_t, compute, 0, 0)
PNANOVDB_REFLECT_END(0)
PNANOVDB_REFLECT_INTERFACE_IMPL()
#undef PNANOVDB_REFLECT_TYPE

typedef pnanovdb_raster_t*(PNANOVDB_ABI* PFN_pnanovdb_get_raster)();

PNANOVDB_API pnanovdb_raster_t* pnanovdb_get_raster();

static inline void pnanovdb_raster_load(pnanovdb_raster_t* raster, const pnanovdb_compute_t* compute)
{
    auto get_raster = (PFN_pnanovdb_get_raster)pnanovdb_get_proc_address(compute->module, "pnanovdb_get_raster");
    if (!get_raster)
    {
        printf("Error: Failed to acquire raster\n");
        return;
    }
    *raster = *get_raster();

    raster->compute = compute;
}

static inline void pnanovdb_raster_free(pnanovdb_raster_t* raster)
{
    // NOP for now
}

namespace pnanovdb_raster
{
enum shader
{
    // raster points shaders
    gaussian_frag_alloc_slang,
    gaussian_frag_color_slang,
    gaussian_prim_slang,
    point_frag_alloc_slang,
    point_frag_color_slang,

    // raster 2d shaders
    gaussian_count_tiles_slang,
    gaussian_projection_slang,
    gaussian_rasterize_2d_slang,
    gaussian_rasterize_2d_null_slang,
    gaussian_spherical_harmonics_slang,
    gaussian_tile_intersections_slang,
    gaussian_tile_offsets_slang,

    shader_count
};

enum shader_params
{
    raster_2d_shaders = shader_count, // all 2d shaders use the same params for now
    shader_param_count
};
}

#endif
