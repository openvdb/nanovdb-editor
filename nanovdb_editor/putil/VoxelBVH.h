// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/putil/VoxelBVH.h

    \author Andrew Reidmeyer

    \brief
*/

#ifndef NANOVDB_PUTILS_VOXELBVH_H_HAS_BEEN_INCLUDED
#define NANOVDB_PUTILS_VOXELBVH_H_HAS_BEEN_INCLUDED

#include "nanovdb_editor/putil/Camera.h"
#include "nanovdb_editor/putil/Compute.h"

/// ********************************* VoxelBVH ***************************************

struct pnanovdb_voxelbvh_context_t;
typedef struct pnanovdb_voxelbvh_context_t pnanovdb_voxelbvh_context_t;

typedef struct pnanovdb_voxelbvh_t
{
    PNANOVDB_REFLECT_INTERFACE();

    const pnanovdb_compute_t* compute;

    pnanovdb_voxelbvh_context_t*(PNANOVDB_ABI* create_context)(const pnanovdb_compute_t* compute,
                                                               pnanovdb_compute_queue_t* queue);

    void(PNANOVDB_ABI* destroy_context)(const pnanovdb_compute_t* compute,
                                        pnanovdb_compute_queue_t* queue,
                                        pnanovdb_voxelbvh_context_t* context);

    void(PNANOVDB_ABI* nanovdb_generate_node_mask)(const pnanovdb_compute_t* compute,
                                                   pnanovdb_compute_queue_t* queue,
                                                   pnanovdb_voxelbvh_context_t* context,
                                                   pnanovdb_compute_buffer_t* nanovdb_inout,
                                                   pnanovdb_uint64_t nanovdb_word_count,
                                                   pnanovdb_compute_buffer_t* node_mask_out,
                                                   pnanovdb_uint64_t node_mask_uint64_count);

    pnanovdb_compute_array_t*(PNANOVDB_ABI* nanovdb_generate_node_mask_array)(const pnanovdb_compute_t* compute,
                                                                              pnanovdb_compute_queue_t* queue,
                                                                              pnanovdb_voxelbvh_context_t* context,
                                                                              pnanovdb_compute_array_t* nanovdb_array);

    void(PNANOVDB_ABI* nanovdb_init)(const pnanovdb_compute_t* compute,
                                     pnanovdb_compute_queue_t* queue,
                                     pnanovdb_voxelbvh_context_t* context,
                                     pnanovdb_compute_buffer_t* nanovdb_inout,
                                     pnanovdb_uint64_t nanovdb_word_count,
                                     pnanovdb_compute_buffer_t* world_bbox_in,
                                     pnanovdb_uint32_t resolution,
                                     const float* transform_floats,
                                     pnanovdb_uint32_t transform_float_count,
                                     pnanovdb_uint32_t grid_type);

    void(PNANOVDB_ABI* nanovdb_add_nodes)(const pnanovdb_compute_t* compute,
                                          pnanovdb_compute_queue_t* queue,
                                          pnanovdb_voxelbvh_context_t* context,
                                          pnanovdb_compute_buffer_t* nanovdb_inout,
                                          pnanovdb_uint64_t nanovdb_word_count);

    void(PNANOVDB_ABI* nanovdb_add_nodes_from_ijkl_buffer)(const pnanovdb_compute_t* compute,
                                                           pnanovdb_compute_queue_t* queue,
                                                           pnanovdb_voxelbvh_context_t* context,
                                                           pnanovdb_compute_buffer_t* nanovdb_inout,
                                                           pnanovdb_uint64_t nanovdb_word_count,
                                                           pnanovdb_compute_buffer_t* range_flat_inout,
                                                           pnanovdb_uint64_t range_flat_count,
                                                           pnanovdb_compute_buffer_t* ijkl_in,
                                                           pnanovdb_compute_buffer_t* range_in,
                                                           pnanovdb_uint64_t ijkl_count,
                                                           pnanovdb_uint64_t range_count);

    void(PNANOVDB_ABI* nanovdb_add_nodes_from_ijkl_array)(const pnanovdb_compute_t* compute,
                                                          pnanovdb_compute_queue_t* queue,
                                                          pnanovdb_voxelbvh_context_t* context,
                                                          pnanovdb_compute_array_t** out_nanovdb,
                                                          pnanovdb_compute_array_t** out_flat_range,
                                                          pnanovdb_compute_array_t* ijkl_in,
                                                          pnanovdb_compute_array_t* range_in,
                                                          pnanovdb_compute_array_t* world_bbox_in,
                                                          pnanovdb_uint32_t resolution,
                                                          const float* transform_floats,
                                                          pnanovdb_uint32_t transform_float_count);

    void(PNANOVDB_ABI* ijkl_from_gaussians)(const pnanovdb_compute_t* compute,
                                            pnanovdb_compute_queue_t* queue,
                                            pnanovdb_voxelbvh_context_t* context,
                                            pnanovdb_compute_buffer_t** gaussian_array_buffers, // means, opacities,
                                                                                                // quats, scales,
                                                                                                // sh0, shn
                                            pnanovdb_uint32_t gaussian_array_count,
                                            pnanovdb_uint64_t gaussian_count,
                                            pnanovdb_compute_buffer_t* ijkl_out,
                                            pnanovdb_compute_buffer_t* prim_id_out,
                                            pnanovdb_compute_buffer_t* range_out,
                                            pnanovdb_compute_buffer_t* world_bbox_out,
                                            pnanovdb_uint32_t resolution,
                                            const float* transform_floats,
                                            pnanovdb_uint32_t transform_float_count);

    void(PNANOVDB_ABI* ijkl_from_gaussians_file)(const pnanovdb_compute_t* compute,
                                                 pnanovdb_compute_queue_t* queue,
                                                 pnanovdb_voxelbvh_context_t* context,
                                                 const char* filename,
                                                 pnanovdb_compute_array_t** ijkl_out,
                                                 pnanovdb_compute_array_t** prim_id_out,
                                                 pnanovdb_compute_array_t** range_out,
                                                 pnanovdb_compute_array_t** world_bbox_out,
                                                 pnanovdb_uint32_t resolution,
                                                 pnanovdb_compute_array_t** gaussian_arrays_out,
                                                 pnanovdb_uint32_t gaussian_array_count,
                                                 const float* transform_floats,
                                                 pnanovdb_uint32_t transform_float_count);

    void(PNANOVDB_ABI* nanovdb_append_metadata)(const pnanovdb_compute_t* compute,
                                                pnanovdb_compute_array_t* nanovdb_in,
                                                pnanovdb_compute_array_t** nanovdb_out,
                                                pnanovdb_compute_array_t** metadata_arrays,
                                                pnanovdb_uint32_t metadata_count);

    void(PNANOVDB_ABI* ijkl_from_lines)(const pnanovdb_compute_t* compute,
                                        pnanovdb_compute_queue_t* queue,
                                        pnanovdb_voxelbvh_context_t* context,
                                        pnanovdb_compute_buffer_t* indices_buffer,
                                        pnanovdb_compute_buffer_t* positions_buffer,
                                        pnanovdb_uint64_t line_count,
                                        float inflation_radius,
                                        pnanovdb_compute_buffer_t* ijkl_out,
                                        pnanovdb_compute_buffer_t* prim_id_out,
                                        pnanovdb_compute_buffer_t* range_out,
                                        pnanovdb_compute_buffer_t* world_bbox_out,
                                        pnanovdb_uint32_t resolution);

    void(PNANOVDB_ABI* ijkl_from_lines_array)(const pnanovdb_compute_t* compute,
                                              pnanovdb_compute_queue_t* queue,
                                              pnanovdb_voxelbvh_context_t* context,
                                              pnanovdb_compute_array_t* indices_array,
                                              pnanovdb_compute_array_t* positions_array,
                                              float inflation_radius,
                                              pnanovdb_compute_array_t** ijkl_out,
                                              pnanovdb_compute_array_t** prim_id_out,
                                              pnanovdb_compute_array_t** range_out,
                                              pnanovdb_compute_array_t** world_bbox_out,
                                              pnanovdb_uint32_t resolution);

    void(PNANOVDB_ABI* ijkl_from_triangles)(const pnanovdb_compute_t* compute,
                                            pnanovdb_compute_queue_t* queue,
                                            pnanovdb_voxelbvh_context_t* context,
                                            pnanovdb_compute_buffer_t* indices_buffer,
                                            pnanovdb_compute_buffer_t* positions_buffer,
                                            pnanovdb_uint64_t triangle_count,
                                            float inflation_radius,
                                            pnanovdb_compute_buffer_t* ijkl_out,
                                            pnanovdb_compute_buffer_t* prim_id_out,
                                            pnanovdb_compute_buffer_t* range_out,
                                            pnanovdb_compute_buffer_t* world_bbox_out,
                                            pnanovdb_uint32_t resolution);

    void(PNANOVDB_ABI* ijkl_from_triangles_array)(const pnanovdb_compute_t* compute,
                                                  pnanovdb_compute_queue_t* queue,
                                                  pnanovdb_voxelbvh_context_t* context,
                                                  pnanovdb_compute_array_t* indices_array,
                                                  pnanovdb_compute_array_t* positions_array,
                                                  float inflation_radius,
                                                  pnanovdb_compute_array_t** ijkl_out,
                                                  pnanovdb_compute_array_t** prim_id_out,
                                                  pnanovdb_compute_array_t** range_out,
                                                  pnanovdb_compute_array_t** world_bbox_out,
                                                  pnanovdb_uint32_t resolution);

    pnanovdb_compute_array_t*(PNANOVDB_ABI* nanovdb_from_gaussians_file)(const pnanovdb_compute_t* compute,
                                                                         pnanovdb_compute_queue_t* queue,
                                                                         pnanovdb_voxelbvh_context_t* context,
                                                                         const char* filename,
                                                                         pnanovdb_uint32_t resolution);

    pnanovdb_compute_array_t*(PNANOVDB_ABI* nanovdb_from_gaussians_array)(
        const pnanovdb_compute_t* compute,
        pnanovdb_compute_queue_t* queue,
        pnanovdb_voxelbvh_context_t* context,
        pnanovdb_compute_array_t** gaussian_arrays, // [means, opacities, quaternions, scales, sh_0, sh_n]
        pnanovdb_uint32_t gaussian_array_count, // must be 6
        pnanovdb_uint32_t resolution);

    pnanovdb_compute_array_t*(PNANOVDB_ABI* nanovdb_from_triangles_array)(const pnanovdb_compute_t* compute,
                                                                          pnanovdb_compute_queue_t* queue,
                                                                          pnanovdb_voxelbvh_context_t* context,
                                                                          pnanovdb_compute_array_t* indices_array,
                                                                          pnanovdb_compute_array_t* positions_array,
                                                                          pnanovdb_compute_array_t* colors_array,
                                                                          float inflation_radius,
                                                                          pnanovdb_uint32_t resolution);

    pnanovdb_compute_array_t*(PNANOVDB_ABI* nanovdb_from_lines_array)(const pnanovdb_compute_t* compute,
                                                                      pnanovdb_compute_queue_t* queue,
                                                                      pnanovdb_voxelbvh_context_t* context,
                                                                      pnanovdb_compute_array_t* indices_array,
                                                                      pnanovdb_compute_array_t* positions_array,
                                                                      pnanovdb_compute_array_t* colors_array,
                                                                      float inflation_radius,
                                                                      pnanovdb_uint32_t resolution);

    void(PNANOVDB_ABI* nanovdb_duplicate_topology)(const pnanovdb_compute_t* compute,
                                                   pnanovdb_compute_queue_t* queue,
                                                   pnanovdb_voxelbvh_context_t* context,
                                                   pnanovdb_compute_buffer_t* dst_nanovdb_inout,
                                                   pnanovdb_uint64_t dst_nanovdb_word_count,
                                                   pnanovdb_compute_buffer_t* src_nanovdb_in,
                                                   pnanovdb_uint64_t src_nanovdb_word_count,
                                                   pnanovdb_uint32_t dst_grid_type,
                                                   pnanovdb_uint32_t upsample_factor);

    void(PNANOVDB_ABI* nanovdb_duplicate_topology_array)(const pnanovdb_compute_t* compute,
                                                         pnanovdb_compute_queue_t* queue,
                                                         pnanovdb_voxelbvh_context_t* context,
                                                         pnanovdb_compute_array_t** dst_nanovdb_out,
                                                         pnanovdb_compute_array_t* src_nanovdb_in,
                                                         pnanovdb_uint32_t dst_grid_type,
                                                         pnanovdb_uint32_t upsample_factor);

    void(PNANOVDB_ABI* nanovdb_rgba8_from_voxelbvh)(const pnanovdb_compute_t* compute,
                                                    pnanovdb_compute_queue_t* queue,
                                                    pnanovdb_voxelbvh_context_t* context,
                                                    pnanovdb_compute_buffer_t* dst_nanovdb_inout,
                                                    pnanovdb_uint64_t dst_nanovdb_word_count,
                                                    pnanovdb_compute_buffer_t* src_nanovdb_in,
                                                    pnanovdb_uint64_t src_nanovdb_word_count,
                                                    pnanovdb_vec3_t index_space_ray_direction);

    void(PNANOVDB_ABI* nanovdb_rgba8_from_voxelbvh_array)(const pnanovdb_compute_t* compute,
                                                          pnanovdb_compute_queue_t* queue,
                                                          pnanovdb_voxelbvh_context_t* context,
                                                          pnanovdb_compute_array_t* dst_nanovdb_inout,
                                                          pnanovdb_compute_array_t* src_nanovdb_in,
                                                          pnanovdb_vec3_t index_space_ray_direction);

} pnanovdb_voxelbvh_t;

#define PNANOVDB_REFLECT_TYPE pnanovdb_voxelbvh_t
PNANOVDB_REFLECT_BEGIN()
PNANOVDB_REFLECT_POINTER(pnanovdb_compute_t, compute, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(create_context, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(destroy_context, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(nanovdb_generate_node_mask, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(nanovdb_generate_node_mask_array, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(nanovdb_init, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(nanovdb_add_nodes, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(nanovdb_add_nodes_from_ijkl_buffer, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(nanovdb_add_nodes_from_ijkl_array, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(ijkl_from_gaussians, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(ijkl_from_gaussians_file, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(nanovdb_append_metadata, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(ijkl_from_lines, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(ijkl_from_lines_array, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(ijkl_from_triangles, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(ijkl_from_triangles_array, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(nanovdb_from_gaussians_file, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(nanovdb_from_gaussians_array, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(nanovdb_from_triangles_array, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(nanovdb_from_lines_array, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(nanovdb_duplicate_topology, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(nanovdb_duplicate_topology_array, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(nanovdb_rgba8_from_voxelbvh, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(nanovdb_rgba8_from_voxelbvh_array, 0, 0)
PNANOVDB_REFLECT_END(0)
PNANOVDB_REFLECT_INTERFACE_IMPL()
#undef PNANOVDB_REFLECT_TYPE

typedef pnanovdb_voxelbvh_t*(PNANOVDB_ABI* PFN_pnanovdb_get_voxelbvh)();

PNANOVDB_API pnanovdb_voxelbvh_t* pnanovdb_get_voxelbvh();

static void pnanovdb_voxelbvh_load(pnanovdb_voxelbvh_t* voxelbvh, const pnanovdb_compute_t* compute)
{
    auto get_voxelbvh = (PFN_pnanovdb_get_voxelbvh)pnanovdb_get_proc_address(compute->module, "pnanovdb_get_voxelbvh");
    if (!get_voxelbvh)
    {
        printf("Error: Failed to acquire grid build\n");
        return;
    }
    *voxelbvh = *get_voxelbvh();

    voxelbvh->compute = compute;
}

static void pnanovdb_voxelbvh_free(pnanovdb_voxelbvh_t* voxelbvh)
{
    // NOP for now
}

#endif
