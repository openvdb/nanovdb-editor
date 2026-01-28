// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/raster/VoxelBVH.cpp

    \author Andrew Reidmeyer

    \brief
*/

#define PNANOVDB_BUF_BOUNDS_CHECK
#include "Common.h"
#include "nanovdb_editor/putil/ParallelPrimitives.h"
#include "nanovdb_editor/putil/VoxelBVH.h"
#include "nanovdb_editor/putil/ThreadPool.hpp"

#include <stdlib.h>
#include <math.h>
#include <vector>
#include <future>

namespace
{

enum shader
{
    voxelbvh_allocate_leaves_slang,
    voxelbvh_allocate_lowers_slang,
    voxelbvh_allocate_range_headers_slang,
    voxelbvh_count_lower_masks_slang,
    voxelbvh_count_voxel_masks_slang,
    voxelbvh_find_range_starts_slang,
    voxelbvh_gaussians_bbox_reduce1_slang,
    voxelbvh_gaussians_bbox_reduce2_slang,
    voxelbvh_gaussians_to_ijkl_slang,
    voxelbvh_init_grid_slang,
    voxelbvh_scatter_range_headers_slang,
    voxelbvh_set_lower_masks_slang,
    voxelbvh_set_upper_masks_slang,
    voxelbvh_set_voxel_masks_slang,

    shader_count
};

static const char* s_shader_names[shader_count] = {
    "raster/voxelbvh_allocate_leaves.slang",
    "raster/voxelbvh_allocate_lowers.slang",
    "raster/voxelbvh_allocate_range_headers.slang",
    "raster/voxelbvh_count_lower_masks.slang",
    "raster/voxelbvh_count_voxel_masks.slang",
    "raster/voxelbvh_find_range_starts.slang",
    "raster/voxelbvh_gaussians_bbox_reduce1.slang",
    "raster/voxelbvh_gaussians_bbox_reduce2.slang",
    "raster/voxelbvh_gaussians_to_ijkl.slang",
    "raster/voxelbvh_init_grid.slang",
    "raster/voxelbvh_scatter_range_headers.slang",
    "raster/voxelbvh_set_lower_masks.slang",
    "raster/voxelbvh_set_upper_masks.slang",
    "raster/voxelbvh_set_voxel_masks.slang"
 };

struct voxelbvh_context_t
{
    pnanovdb_shader_context_t* shader_ctx[shader_count];

    pnanovdb_parallel_primitives_t parallel_primitives;
    pnanovdb_parallel_primitives_context_t* parallel_primitives_ctx;
};

PNANOVDB_CAST_PAIR(pnanovdb_voxelbvh_context_t, voxelbvh_context_t)

static pnanovdb_voxelbvh_context_t* create_context(const pnanovdb_compute_t* compute, pnanovdb_compute_queue_t* queue)
{
    voxelbvh_context_t* ctx = new voxelbvh_context_t();

    pnanovdb_parallel_primitives_load(&ctx->parallel_primitives, compute);
    ctx->parallel_primitives_ctx = ctx->parallel_primitives.create_context(compute, queue);

    pnanovdb_compiler_settings_t compile_settings = {};
    pnanovdb_compiler_settings_init(&compile_settings);

    pnanovdb_util::ThreadPool pool;
    std::vector<std::future<bool>> futures;

    for (pnanovdb_uint32_t idx = 0u; idx < shader_count; idx++)
    {
        auto future = pool.enqueue(
            [compute, queue, ctx, idx, &compile_settings]() -> bool
            {
                ctx->shader_ctx[idx] = compute->create_shader_context(s_shader_names[idx]);
                return compute->init_shader(compute, queue, ctx->shader_ctx[idx], &compile_settings) == PNANOVDB_TRUE;
            });
        futures.push_back(std::move(future));
    }

    for (auto& future : futures)
    {
        bool success = future.get();
        if (!success)
        {
            return nullptr;
        }
    }

    return cast(ctx);
}

static void destroy_context(const pnanovdb_compute_t* compute,
                            pnanovdb_compute_queue_t* queue,
                            pnanovdb_voxelbvh_context_t* context_in)
{
    auto ctx = cast(context_in);

    for (pnanovdb_uint32_t idx = 0u; idx < shader_count; idx++)
    {
        compute->destroy_shader_context(compute, queue, ctx->shader_ctx[idx]);
    }

    ctx->parallel_primitives.destroy_context(compute, queue, ctx->parallel_primitives_ctx);
    pnanovdb_parallel_primitives_free(&ctx->parallel_primitives);

    delete ctx;
}

void voxelbvh_from_gaussians(const pnanovdb_compute_t* compute,
                             pnanovdb_compute_queue_t* queue,
                             pnanovdb_voxelbvh_context_t* voxelbvh_context,
                             pnanovdb_compute_buffer_t** gaussian_array_buffers, // means, opacities, quats, scales, sh0, shn
                             pnanovdb_uint32_t gaussian_array_count,
                             pnanovdb_uint64_t gaussian_count,
                             pnanovdb_compute_buffer_t* nanovdb_out,
                             pnanovdb_uint64_t nanovdb_word_count)
{
    auto ctx = cast(voxelbvh_context);

    pnanovdb_compute_interface_t* compute_interface = compute->device_interface.get_compute_interface(queue);
    pnanovdb_compute_context_t* context = compute->device_interface.get_compute_context(queue);

    pnanovdb_compute_buffer_transient_t* means_transient =
        compute_interface->register_buffer_as_transient(context, gaussian_array_buffers[0]);
    pnanovdb_compute_buffer_transient_t* opacities_transient =
        compute_interface->register_buffer_as_transient(context, gaussian_array_buffers[1]);
    pnanovdb_compute_buffer_transient_t* quats_transient =
        compute_interface->register_buffer_as_transient(context, gaussian_array_buffers[2]);
    pnanovdb_compute_buffer_transient_t* scales_transient =
        compute_interface->register_buffer_as_transient(context, gaussian_array_buffers[3]);
    pnanovdb_compute_buffer_transient_t* sh0_transient =
        compute_interface->register_buffer_as_transient(context, gaussian_array_buffers[4]);
    pnanovdb_compute_buffer_transient_t* shn_transient =
        compute_interface->register_buffer_as_transient(context, gaussian_array_buffers[5]);

    struct constants_t
    {
        pnanovdb_uint32_t point_count;
        pnanovdb_uint32_t workgroup_count;
        pnanovdb_uint32_t pad2;
        pnanovdb_uint32_t pad3;
    };
    constants_t constants = {};
    constants.point_count = (pnanovdb_uint32_t)gaussian_count;
    constants.workgroup_count = (constants.point_count + 255u) / 256u;

    // constants
    pnanovdb_compute_buffer_desc_t buf_desc = {};
    buf_desc.usage = PNANOVDB_COMPUTE_BUFFER_USAGE_CONSTANT;
    buf_desc.format = PNANOVDB_COMPUTE_FORMAT_UNKNOWN;
    buf_desc.structure_stride = 0u;
    buf_desc.size_in_bytes = sizeof(constants_t);
    pnanovdb_compute_buffer_t* constant_buffer =
        compute_interface->create_buffer(context, PNANOVDB_COMPUTE_MEMORY_TYPE_UPLOAD, &buf_desc);

    // copy constants
    void* mapped_constants = compute_interface->map_buffer(context, constant_buffer);
    memcpy(mapped_constants, &constants, sizeof(constants_t));
    compute_interface->unmap_buffer(context, constant_buffer);

    pnanovdb_compute_buffer_transient_t* constant_transient =
        compute_interface->register_buffer_as_transient(context, constant_buffer);

    buf_desc.usage = PNANOVDB_COMPUTE_BUFFER_USAGE_STRUCTURED | PNANOVDB_COMPUTE_BUFFER_USAGE_RW_STRUCTURED;
    buf_desc.format = PNANOVDB_COMPUTE_FORMAT_UNKNOWN;
    buf_desc.structure_stride = 4u;
    buf_desc.size_in_bytes = 6u * 4u * constants.workgroup_count;
    pnanovdb_compute_buffer_t* bbox_reduce1_buffer =
        compute_interface->create_buffer(context, PNANOVDB_COMPUTE_MEMORY_TYPE_DEVICE, &buf_desc);
    buf_desc.size_in_bytes = 6u * 4u;
    pnanovdb_compute_buffer_t* bbox_reduce2_buffer =
        compute_interface->create_buffer(context, PNANOVDB_COMPUTE_MEMORY_TYPE_DEVICE, &buf_desc);
    buf_desc.size_in_bytes = 8u * 4u * constants.workgroup_count;
    pnanovdb_compute_buffer_t* keys_low_buffer =
        compute_interface->create_buffer(context, PNANOVDB_COMPUTE_MEMORY_TYPE_DEVICE, &buf_desc);
    pnanovdb_compute_buffer_t* keys_high_buffer =
        compute_interface->create_buffer(context, PNANOVDB_COMPUTE_MEMORY_TYPE_DEVICE, &buf_desc);
    pnanovdb_compute_buffer_t* bbox_ids_buffer =
        compute_interface->create_buffer(context, PNANOVDB_COMPUTE_MEMORY_TYPE_DEVICE, &buf_desc);

    pnanovdb_compute_buffer_transient_t* bbox_reduce1_transient =
        compute_interface->register_buffer_as_transient(context, bbox_reduce1_buffer);
    pnanovdb_compute_buffer_transient_t* bbox_reduce2_transient =
        compute_interface->register_buffer_as_transient(context, bbox_reduce2_buffer);
    pnanovdb_compute_buffer_transient_t* keys_low_transient =
        compute_interface->register_buffer_as_transient(context, keys_low_buffer);
    pnanovdb_compute_buffer_transient_t* keys_high_transient =
        compute_interface->register_buffer_as_transient(context, keys_high_buffer);
    pnanovdb_compute_buffer_transient_t* bbox_ids_transient =
        compute_interface->register_buffer_as_transient(context, bbox_ids_buffer);

    // gaussian to ijk-level request
    // voxelbvh_gaussians_bbox_reduce1.slang
    // voxelbvh_gaussians_bbox_reduce2.slang
    // voxelbvh_gaussians_to_ijkl.slang
    {
        pnanovdb_compute_resource_t resources[6u] = {};
        resources[0u].buffer_transient = constant_transient;
        resources[1u].buffer_transient = means_transient;
        resources[2u].buffer_transient = opacities_transient;
        resources[3u].buffer_transient = quats_transient;
        resources[4u].buffer_transient = scales_transient;
        resources[5u].buffer_transient = bbox_reduce1_transient;

        compute->dispatch_shader(compute_interface, context, ctx->shader_ctx[voxelbvh_gaussians_bbox_reduce1_slang], resources,
                                 constants.workgroup_count, 1u, 1u, "voxelbvh_gaussians_bbox_reduce1");
    }
    {
        pnanovdb_compute_resource_t resources[3u] = {};
        resources[0u].buffer_transient = constant_transient;
        resources[1u].buffer_transient = bbox_reduce1_transient;
        resources[2u].buffer_transient = bbox_reduce2_transient;;

        compute->dispatch_shader(compute_interface, context, ctx->shader_ctx[voxelbvh_gaussians_bbox_reduce2_slang], resources,
                                 1u, 1u, 1u, "voxelbvh_gaussians_bbox_reduce2");
    }
    {
        pnanovdb_compute_resource_t resources[9u] = {};
        resources[0u].buffer_transient = constant_transient;
        resources[1u].buffer_transient = means_transient;
        resources[2u].buffer_transient = opacities_transient;
        resources[3u].buffer_transient = quats_transient;
        resources[4u].buffer_transient = scales_transient;
        resources[5u].buffer_transient = bbox_reduce2_transient;
        resources[6u].buffer_transient = keys_low_transient;
        resources[7u].buffer_transient = keys_high_transient;
        resources[8u].buffer_transient = bbox_ids_transient;

        compute->dispatch_shader(compute_interface, context, ctx->shader_ctx[voxelbvh_gaussians_to_ijkl_slang], resources,
                                 constants.workgroup_count, 1u, 1u, "voxelbvh_gaussians_to_ijkl");
    }

    // sort ijk-level requests to bring requests together
    // radix sort
    {
        ctx->parallel_primitives.radix_sort_dual_key(compute, queue, ctx->parallel_primitives_ctx,
                                                     keys_low_buffer, keys_high_buffer,
                                                     bbox_ids_buffer, 8u * constants.point_count,
                                                     8u * constants.point_count, 32u, 32u);
    }

    // identify range starts
    // voxelbvh_find_range_starts.slang
    // global scan to allocate range headers
    // scatter range headers
    // voxelbvh_scatter_range_headers.slang

    // form grid header, tree, root, upper from template
    // voxelbvh_init_grid.slang

    // set child masks on upper
    // voxelbvh_set_upper_masks.slang
    // count new lowers and scan new lowers to allocate
    // voxelbvh_allocate_lowers.slang

    // set child masks on lower
    // voxelbvh_set_lower_masks.slang
    // count new leaves
    // voxelbvh_count_lower_masks.slang
    // scan new leaves
    // voxelbvh_allocate_leaves.slang

    // set level masks on voxels
    // voxelbvh_set_voxel_masks.slang
    // count per voxel range header counts
    // voxelbvh_count_voxel_masks.slang
    // scan per voxel range header counts
    // allocate per voxel range headers
    // voxelbvh_allocate_range_headers.slang
    // scatter per voxel range headers
    // voxelbvh_scatter_range_headers.slang

    compute_interface->destroy_buffer(context, constant_buffer);
    compute_interface->destroy_buffer(context, bbox_reduce1_buffer);
    compute_interface->destroy_buffer(context, bbox_reduce2_buffer);
    compute_interface->destroy_buffer(context, keys_low_buffer);
    compute_interface->destroy_buffer(context, keys_high_buffer);
    compute_interface->destroy_buffer(context, bbox_ids_buffer);
}

}

pnanovdb_voxelbvh_t* pnanovdb_get_voxelbvh()
{
    static pnanovdb_voxelbvh_t iface = { PNANOVDB_REFLECT_INTERFACE_INIT(pnanovdb_voxelbvh_t) };

    iface.create_context = create_context;
    iface.destroy_context = destroy_context;
    iface.voxelbvh_from_gaussians = voxelbvh_from_gaussians;

    return &iface;
}
