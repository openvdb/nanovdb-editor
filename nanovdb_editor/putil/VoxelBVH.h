// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/putil/VoxelBVH.h

    \author Andrew Reidmeyer

    \brief
*/

#ifndef NANOVDB_PUTILS_VOXELBVH_H_HAS_BEEN_INCLUDED
#define NANOVDB_PUTILS_VOXELBVH_H_HAS_BEEN_INCLUDED

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

    void(PNANOVDB_ABI* voxelbvh_from_gaussians)(const pnanovdb_compute_t* compute,
                                                pnanovdb_compute_queue_t* queue,
                                                pnanovdb_voxelbvh_context_t* context,
                                                pnanovdb_compute_buffer_t** gaussian_array_buffers, // means, opacities, quats, scales, sh0, shn
                                                pnanovdb_uint32_t gaussian_array_count,
                                                pnanovdb_uint64_t gaussian_count,
                                                pnanovdb_compute_buffer_t* nanovdb_out,
                                                pnanovdb_uint64_t nanovdb_word_count);

} pnanovdb_voxelbvh_t;

#define PNANOVDB_REFLECT_TYPE pnanovdb_voxelbvh_t
PNANOVDB_REFLECT_BEGIN()
PNANOVDB_REFLECT_POINTER(pnanovdb_compute_t, compute, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(create_context, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(destroy_context, 0, 0)
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
