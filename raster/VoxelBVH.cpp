// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/raster/VoxelBVH.cpp

    \author Andrew Reidmeyer

    \brief
*/

#include "nanovdb_editor/putil/Compute.h"
#define PNANOVDB_BUF_BOUNDS_CHECK
#include "Common.h"
#include "nanovdb_editor/putil/ParallelPrimitives.h"
#include "nanovdb_editor/putil/VoxelBVH.h"
#include "nanovdb_editor/putil/ThreadPool.hpp"
#include "nanovdb_editor/putil/FileFormat.h"

#include <stdlib.h>
#include <math.h>
#include <vector>
#include <future>
#include <map>

namespace
{

enum shader
{
    voxelbvh_find_range_starts_slang,
    voxelbvh_gaussians_bbox_reduce1_slang,
    voxelbvh_gaussians_bbox_reduce2_slang,
    voxelbvh_gaussians_to_ijkl_slang,
    voxelbvh_nanovdb_add_count_slang,
    voxelbvh_nanovdb_add_link_slang,
    voxelbvh_nanovdb_add_scan_slang,
    voxelbvh_nanovdb_find_clear_slang,
    voxelbvh_nanovdb_find_leaves_slang,
    voxelbvh_nanovdb_find_lowers_slang,
    voxelbvh_nanovdb_find_root_slang,
    voxelbvh_nanovdb_find_uppers_slang,
    voxelbvh_nanovdb_init_slang,
    voxelbvh_nanovdb_iterate_copy_scratch_slang,
    voxelbvh_nanovdb_level_list_alloc1_slang,
    voxelbvh_nanovdb_level_list_alloc2_slang,
    voxelbvh_nanovdb_level_list_alloc3_slang,
    voxelbvh_nanovdb_level_list_flatten_slang,
    voxelbvh_nanovdb_level_list_splat_slang,
    voxelbvh_nanovdb_level_list_spread_slang,
    voxelbvh_nanovdb_level_mask_flatten_slang,
    voxelbvh_nanovdb_merge_voxels_slang,
    voxelbvh_nanovdb_set_mask_ijkl_slang,
    voxelbvh_nanovdb_set_value_ijkl_slang,
    voxelbvh_scatter_range_headers_slang,

    shader_count
};

static const char* s_shader_names[shader_count] = {
    "raster/voxelbvh_find_range_starts.slang",
    "raster/voxelbvh_gaussians_bbox_reduce1.slang",
    "raster/voxelbvh_gaussians_bbox_reduce2.slang",
    "raster/voxelbvh_gaussians_to_ijkl.slang",
    "raster/voxelbvh_nanovdb_add_count.slang",
    "raster/voxelbvh_nanovdb_add_link.slang",
    "raster/voxelbvh_nanovdb_add_scan.slang",
    "raster/voxelbvh_nanovdb_find_clear.slang",
    "raster/voxelbvh_nanovdb_find_leaves.slang",
    "raster/voxelbvh_nanovdb_find_lowers.slang",
    "raster/voxelbvh_nanovdb_find_root.slang",
    "raster/voxelbvh_nanovdb_find_uppers.slang",
    "raster/voxelbvh_nanovdb_init.slang",

    "raster/voxelbvh_nanovdb_iterate_copy_scratch.slang",
    "raster/voxelbvh_nanovdb_level_list_alloc1.slang",
    "raster/voxelbvh_nanovdb_level_list_alloc2.slang",
    "raster/voxelbvh_nanovdb_level_list_alloc3.slang",
    "raster/voxelbvh_nanovdb_level_list_flatten.slang",
    "raster/voxelbvh_nanovdb_level_list_splat.slang",
    "raster/voxelbvh_nanovdb_level_list_spread.slang",
    "raster/voxelbvh_nanovdb_level_mask_flatten.slang",
    "raster/voxelbvh_nanovdb_merge_voxels.slang",

    "raster/voxelbvh_nanovdb_set_mask_ijkl.slang",
    "raster/voxelbvh_nanovdb_set_value_ijkl.slang",
    "raster/voxelbvh_scatter_range_headers.slang",
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

void voxelbvh_generate_node_mask(const pnanovdb_compute_t* compute,
                                 pnanovdb_compute_queue_t* queue,
                                 pnanovdb_voxelbvh_context_t* voxelbvh_context,
                                 pnanovdb_compute_buffer_t* nanovdb_inout,
                                 pnanovdb_uint64_t nanovdb_word_count,
                                 pnanovdb_compute_buffer_t* node_mask_out,
                                 pnanovdb_uint64_t node_mask_uint64_count)
{
    auto ctx = cast(voxelbvh_context);

    pnanovdb_compute_interface_t* compute_interface = compute->device_interface.get_compute_interface(queue);
    pnanovdb_compute_context_t* context = compute->device_interface.get_compute_context(queue);

    struct constants_t
    {
        pnanovdb_uint32_t nanovdb_word_count;
        pnanovdb_uint32_t pad0;
        pnanovdb_uint32_t nanovdb_chunk_count;
        pnanovdb_uint32_t node_mask_uint64_count;
    };
    constants_t constants = {};
    constants.nanovdb_word_count = nanovdb_word_count;
    constants.nanovdb_chunk_count = nanovdb_word_count >> 3u;
    constants.node_mask_uint64_count = node_mask_uint64_count;

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
    pnanovdb_compute_buffer_transient_t* nanovdb_transient =
        compute_interface->register_buffer_as_transient(context, nanovdb_inout);
    pnanovdb_compute_buffer_transient_t* node_mask_transient =
        compute_interface->register_buffer_as_transient(context, node_mask_out);

    {
        pnanovdb_compute_resource_t resources[3u] = {};
        resources[0u].buffer_transient = constant_transient;
        resources[1u].buffer_transient = nanovdb_transient;
        resources[2u].buffer_transient = node_mask_transient;

        compute->dispatch_shader(compute_interface, context, ctx->shader_ctx[voxelbvh_nanovdb_find_clear_slang],
                                 resources, 256u, 1u, 1u, "voxelbvh_nanovdb_find_clear");
        compute->dispatch_shader(compute_interface, context, ctx->shader_ctx[voxelbvh_nanovdb_find_root_slang],
                                 resources, 256u, 1u, 1u, "voxelbvh_nanovdb_find_root");
        compute->dispatch_shader(compute_interface, context, ctx->shader_ctx[voxelbvh_nanovdb_find_uppers_slang],
                                 resources, 256u, 1u, 1u, "voxelbvh_nanovdb_find_uppers");
        compute->dispatch_shader(compute_interface, context, ctx->shader_ctx[voxelbvh_nanovdb_find_lowers_slang],
                                 resources, 256u, 1u, 1u, "voxelbvh_nanovdb_find_lowers");
        compute->dispatch_shader(compute_interface, context, ctx->shader_ctx[voxelbvh_nanovdb_find_leaves_slang],
                                 resources, 256u, 1u, 1u, "voxelbvh_nanovdb_find_leaves");
    }

    compute_interface->destroy_buffer(context, constant_buffer);
}

pnanovdb_compute_array_t* voxelbvh_generate_node_mask_array(const pnanovdb_compute_t* compute,
                                                            pnanovdb_compute_queue_t* queue,
                                                            pnanovdb_voxelbvh_context_t* voxelbvh_context,
                                                            pnanovdb_compute_array_t* nanovdb_array)
{
    auto ctx = cast(voxelbvh_context);

    pnanovdb_compute_interface_t* compute_interface = compute->device_interface.get_compute_interface(queue);
    pnanovdb_compute_context_t* context = compute->device_interface.get_compute_context(queue);

    uint64_t buf_size = nanovdb_array->element_size * nanovdb_array->element_count;
    uint64_t node_mask_size = (buf_size + 63u) / 64u;

    uint64_t nanovdb_word_count = (buf_size + 3u) / 4u;
    uint64_t node_mask_uint64_count = (node_mask_size + 7u) / 8u;

    pnanovdb_compute_array_t* node_mask_array = compute->create_array(8u, node_mask_uint64_count, nullptr);

    compute_gpu_array_t* nanovdb_gpu_array = gpu_array_create();
    compute_gpu_array_t* node_mask_gpu_array = gpu_array_create();

    gpu_array_upload(compute, queue, nanovdb_gpu_array, nanovdb_array);
    gpu_array_alloc_device(compute, queue, node_mask_gpu_array, node_mask_array);

    voxelbvh_generate_node_mask(compute, queue, voxelbvh_context, nanovdb_gpu_array->device_buffer, nanovdb_word_count,
                                node_mask_gpu_array->device_buffer, node_mask_uint64_count);

    gpu_array_readback(compute, queue, node_mask_gpu_array, node_mask_array);

    pnanovdb_uint64_t flushed_frame = 0llu;
    compute->device_interface.flush(queue, &flushed_frame, nullptr, nullptr);

    compute->device_interface.wait_idle(queue);

    gpu_array_map(compute, queue, node_mask_gpu_array, node_mask_array);

    gpu_array_destroy(compute, queue, nanovdb_gpu_array);
    gpu_array_destroy(compute, queue, node_mask_gpu_array);

    return node_mask_array;
}

void voxelbvh_nanovdb_init(const pnanovdb_compute_t* compute,
                           pnanovdb_compute_queue_t* queue,
                           pnanovdb_voxelbvh_context_t* voxelbvh_context,
                           pnanovdb_compute_buffer_t* nanovdb_inout,
                           pnanovdb_uint64_t nanovdb_word_count,
                           const pnanovdb_coord_t* root_tile_coords,
                           pnanovdb_uint32_t root_tile_count)
{
    auto ctx = cast(voxelbvh_context);

    pnanovdb_compute_interface_t* compute_interface = compute->device_interface.get_compute_interface(queue);
    pnanovdb_compute_context_t* context = compute->device_interface.get_compute_context(queue);

    pnanovdb_uint32_t grid_type = PNANOVDB_GRID_TYPE_INT64;
    float voxel_size = 1.f;
    float voxel_size_inv = 1.f / voxel_size;

    pnanovdb_uint64_t size = PNANOVDB_GRID_SIZE + PNANOVDB_TREE_SIZE + PNANOVDB_GRID_TYPE_GET(grid_type, root_size) +
                             root_tile_count * PNANOVDB_GRID_TYPE_GET(grid_type, root_tile_size);

    // constants
    pnanovdb_compute_buffer_desc_t buf_desc = {};
    buf_desc.usage = PNANOVDB_COMPUTE_BUFFER_USAGE_STRUCTURED;
    buf_desc.format = PNANOVDB_COMPUTE_FORMAT_UNKNOWN;
    buf_desc.structure_stride = 8u;
    buf_desc.size_in_bytes = size;
    pnanovdb_compute_buffer_t* upload_buffer =
        compute_interface->create_buffer(context, PNANOVDB_COMPUTE_MEMORY_TYPE_UPLOAD, &buf_desc);

    // copy constants
    void* mapped_upload = compute_interface->map_buffer(context, upload_buffer);

    memset(mapped_upload, 0, size);

    pnanovdb_buf_t buf = pnanovdb_make_buf((uint32_t*)mapped_upload, size / 4u);

    pnanovdb_grid_handle_t grid = { pnanovdb_address_null() };
    pnanovdb_grid_set_magic(buf, grid, PNANOVDB_MAGIC_GRID);
    pnanovdb_grid_set_version(buf, grid,
                              pnanovdb_make_version(PNANOVDB_MAJOR_VERSION_NUMBER, PNANOVDB_MINOR_VERSION_NUMBER,
                                                    PNANOVDB_PATCH_VERSION_NUMBER));
    pnanovdb_grid_set_flags(buf, grid, 0u);
    pnanovdb_grid_set_grid_index(buf, grid, 0u);
    pnanovdb_grid_set_grid_count(buf, grid, 1u);
    pnanovdb_grid_set_grid_size(buf, grid, size);
    pnanovdb_grid_set_grid_name(buf, grid, 0u, 0x00687662); // "bvh"
    pnanovdb_grid_set_voxel_size(buf, grid, 0u, voxel_size);
    pnanovdb_grid_set_voxel_size(buf, grid, 1u, voxel_size);
    pnanovdb_grid_set_voxel_size(buf, grid, 2u, voxel_size);
    pnanovdb_grid_set_grid_class(buf, grid, PNANOVDB_GRID_CLASS_UNKNOWN);
    pnanovdb_grid_set_grid_type(buf, grid, grid_type);

    pnanovdb_map_handle_t map = pnanovdb_grid_get_map(buf, grid);
    pnanovdb_map_set_matf(buf, map, 0u, voxel_size);
    pnanovdb_map_set_matf(buf, map, 4u, voxel_size);
    pnanovdb_map_set_matf(buf, map, 8u, voxel_size);
    pnanovdb_map_set_invmatf(buf, map, 0u, voxel_size_inv);
    pnanovdb_map_set_invmatf(buf, map, 4u, voxel_size_inv);
    pnanovdb_map_set_invmatf(buf, map, 8u, voxel_size_inv);
    pnanovdb_map_set_matd(buf, map, 0u, voxel_size);
    pnanovdb_map_set_matd(buf, map, 4u, voxel_size);
    pnanovdb_map_set_matd(buf, map, 8u, voxel_size);
    pnanovdb_map_set_invmatd(buf, map, 0u, voxel_size_inv);
    pnanovdb_map_set_invmatd(buf, map, 4u, voxel_size_inv);
    pnanovdb_map_set_invmatd(buf, map, 8u, voxel_size_inv);

    pnanovdb_tree_handle_t tree = pnanovdb_grid_get_tree(buf, grid);

    pnanovdb_root_handle_t root = { pnanovdb_address_offset(tree.address, PNANOVDB_TREE_SIZE) };

    pnanovdb_tree_set_first_root(buf, tree, root);

    pnanovdb_root_set_tile_count(buf, root, root_tile_count);
    for (pnanovdb_uint32_t n = 0u; n < root_tile_count; n++)
    {
        pnanovdb_root_tile_handle_t root_tile = pnanovdb_root_get_tile(grid_type, root, n);

        pnanovdb_uint64_t key = pnanovdb_coord_to_key(root_tile_coords + n);
        pnanovdb_root_tile_set_key(buf, root_tile, key);
    }

    compute_interface->unmap_buffer(context, upload_buffer);

    pnanovdb_compute_buffer_transient_t* upload_transient =
        compute_interface->register_buffer_as_transient(context, upload_buffer);
    pnanovdb_compute_buffer_transient_t* nanovdb_transient =
        compute_interface->register_buffer_as_transient(context, nanovdb_inout);

    // voxelbvh_nanovdb_init.slang
    {
        pnanovdb_compute_resource_t resources[2u] = {};
        resources[0u].buffer_transient = upload_transient;
        resources[1u].buffer_transient = nanovdb_transient;

        pnanovdb_uint32_t workgroup_count = ((size / 8u) + 255u) / 256u;

        compute->dispatch_shader(compute_interface, context, ctx->shader_ctx[voxelbvh_nanovdb_init_slang], resources,
                                 workgroup_count, 1u, 1u, "voxelbvh_nanovdb_init");
    }
}

void voxelbvh_nanovdb_add_nodes(const pnanovdb_compute_t* compute,
                                pnanovdb_compute_queue_t* queue,
                                pnanovdb_voxelbvh_context_t* voxelbvh_context,
                                pnanovdb_compute_buffer_t* nanovdb_inout,
                                pnanovdb_uint64_t nanovdb_word_count)
{
    auto ctx = cast(voxelbvh_context);

    pnanovdb_compute_interface_t* compute_interface = compute->device_interface.get_compute_interface(queue);
    pnanovdb_compute_context_t* context = compute->device_interface.get_compute_context(queue);

    uint64_t buf_size = nanovdb_word_count * 4u;
    uint64_t node_mask_size = (buf_size + 63u) / 64u;
    uint64_t node_mask_uint64_count = (node_mask_size + 7u) / 8u;

    pnanovdb_compute_buffer_desc_t buf_desc = {};
    buf_desc.usage = PNANOVDB_COMPUTE_BUFFER_USAGE_STRUCTURED | PNANOVDB_COMPUTE_BUFFER_USAGE_RW_STRUCTURED;
    buf_desc.format = PNANOVDB_COMPUTE_FORMAT_UNKNOWN;
    buf_desc.structure_stride = 8u;
    buf_desc.size_in_bytes = node_mask_uint64_count * 8u;
    pnanovdb_compute_buffer_t* node_mask_buffer =
        compute_interface->create_buffer(context, PNANOVDB_COMPUTE_MEMORY_TYPE_DEVICE, &buf_desc);

    voxelbvh_generate_node_mask(
        compute, queue, voxelbvh_context, nanovdb_inout, nanovdb_word_count, node_mask_buffer, node_mask_uint64_count);

    // allocate workgroup counters
    buf_desc.usage = PNANOVDB_COMPUTE_BUFFER_USAGE_STRUCTURED | PNANOVDB_COMPUTE_BUFFER_USAGE_RW_STRUCTURED;
    buf_desc.format = PNANOVDB_COMPUTE_FORMAT_UNKNOWN;
    buf_desc.structure_stride = 8u;
    buf_desc.size_in_bytes = 4u * 256u * 8u;
    pnanovdb_compute_buffer_t* workgroup_count_buffer =
        compute_interface->create_buffer(context, PNANOVDB_COMPUTE_MEMORY_TYPE_DEVICE, &buf_desc);

    struct constants_t
    {
        pnanovdb_uint32_t nanovdb_word_count;
        pnanovdb_uint32_t pad0;
        pnanovdb_uint32_t nanovdb_chunk_count;
        pnanovdb_uint32_t node_mask_uint64_count;
    };
    constants_t constants = {};
    constants.nanovdb_word_count = nanovdb_word_count;
    constants.nanovdb_chunk_count = nanovdb_word_count >> 3u;
    constants.node_mask_uint64_count = node_mask_uint64_count;

    // constants
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
    pnanovdb_compute_buffer_transient_t* nanovdb_transient =
        compute_interface->register_buffer_as_transient(context, nanovdb_inout);
    pnanovdb_compute_buffer_transient_t* node_mask_transient =
        compute_interface->register_buffer_as_transient(context, node_mask_buffer);
    pnanovdb_compute_buffer_transient_t* workgroup_count_transient =
        compute_interface->register_buffer_as_transient(context, workgroup_count_buffer);

    // voxelbvh_nanovdb_add_count.slang
    {
        pnanovdb_compute_resource_t resources[4u] = {};
        resources[0u].buffer_transient = constant_transient;
        resources[1u].buffer_transient = nanovdb_transient;
        resources[2u].buffer_transient = node_mask_transient;
        resources[3u].buffer_transient = workgroup_count_transient;

        compute->dispatch_shader(compute_interface, context, ctx->shader_ctx[voxelbvh_nanovdb_add_count_slang],
                                 resources, 256u, 1u, 1u, "voxelbvh_nanovdb_add_count");
    }
    // voxelbvh_nanovdb_add_scan.slang
    {
        pnanovdb_compute_resource_t resources[4u] = {};
        resources[0u].buffer_transient = constant_transient;
        resources[1u].buffer_transient = nanovdb_transient;
        resources[2u].buffer_transient = node_mask_transient;
        resources[3u].buffer_transient = workgroup_count_transient;

        compute->dispatch_shader(compute_interface, context, ctx->shader_ctx[voxelbvh_nanovdb_add_scan_slang],
                                 resources, 256u, 1u, 1u, "voxelbvh_nanovdb_add_scan");
    }
    // voxelbvh_nanovdb_add_link.slang
    {
        pnanovdb_compute_resource_t resources[4u] = {};
        resources[0u].buffer_transient = constant_transient;
        resources[1u].buffer_transient = nanovdb_transient;
        resources[2u].buffer_transient = node_mask_transient;
        resources[3u].buffer_transient = workgroup_count_transient;

        compute->dispatch_shader(compute_interface, context, ctx->shader_ctx[voxelbvh_nanovdb_add_link_slang],
                                 resources, 256u, 1u, 1u, "voxelbvh_nanovdb_add_link");
    }

    compute_interface->destroy_buffer(context, node_mask_buffer);
    compute_interface->destroy_buffer(context, workgroup_count_buffer);
    compute_interface->destroy_buffer(context, constant_buffer);
}

void voxelbvh_nanovdb_add_nodes_from_key_buffer(const pnanovdb_compute_t* compute,
                                                pnanovdb_compute_queue_t* queue,
                                                pnanovdb_voxelbvh_context_t* voxelbvh_context,
                                                pnanovdb_compute_buffer_t* nanovdb_inout,
                                                pnanovdb_uint64_t nanovdb_word_count,
                                                pnanovdb_compute_buffer_t* range_flat_inout,
                                                pnanovdb_uint64_t range_flat_count,
                                                pnanovdb_compute_buffer_t* ijkl_in,
                                                pnanovdb_compute_buffer_t* range_in,
                                                pnanovdb_uint64_t ijkl_count,
                                                pnanovdb_uint64_t range_count)
{
    auto ctx = cast(voxelbvh_context);

    pnanovdb_compute_interface_t* compute_interface = compute->device_interface.get_compute_interface(queue);
    pnanovdb_compute_context_t* context = compute->device_interface.get_compute_context(queue);

    uint64_t buf_size = nanovdb_word_count * 4u;
    uint64_t node_mask_size = (buf_size + 63u) / 64u;
    uint64_t node_mask_uint64_count = (node_mask_size + 7u) / 8u;

    // allocate NanoVDB scratch buffer
    pnanovdb_compute_buffer_desc_t buf_desc = {};
    buf_desc.usage = PNANOVDB_COMPUTE_BUFFER_USAGE_STRUCTURED | PNANOVDB_COMPUTE_BUFFER_USAGE_RW_STRUCTURED;
    buf_desc.format = PNANOVDB_COMPUTE_FORMAT_UNKNOWN;
    buf_desc.structure_stride = 8u;
    buf_desc.size_in_bytes = buf_size;
    pnanovdb_compute_buffer_t* scratch_buffer =
        compute_interface->create_buffer(context, PNANOVDB_COMPUTE_MEMORY_TYPE_DEVICE, &buf_desc);

    buf_desc.size_in_bytes = 8u * 256u * 2u;
    pnanovdb_compute_buffer_t* workgroup_counter_buffer =
        compute_interface->create_buffer(context, PNANOVDB_COMPUTE_MEMORY_TYPE_DEVICE, &buf_desc);

    struct constants_t
    {
        pnanovdb_uint32_t nanovdb_word_count;
        pnanovdb_uint32_t ijkl_count;
        pnanovdb_uint32_t nanovdb_chunk_count;
        pnanovdb_uint32_t node_mask_uint64_count;
    };
    constants_t constants = {};
    constants.nanovdb_word_count = nanovdb_word_count;
    constants.ijkl_count = ijkl_count;
    constants.nanovdb_chunk_count = nanovdb_word_count >> 3u;
    constants.node_mask_uint64_count = node_mask_uint64_count;

    // constants
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
    pnanovdb_compute_buffer_transient_t* nanovdb_transient =
        compute_interface->register_buffer_as_transient(context, nanovdb_inout);
    pnanovdb_compute_buffer_transient_t* range_flat_transient =
        compute_interface->register_buffer_as_transient(context, range_flat_inout);
    pnanovdb_compute_buffer_transient_t* ijkl_transient =
        compute_interface->register_buffer_as_transient(context, ijkl_in);
    pnanovdb_compute_buffer_transient_t* range_transient =
        compute_interface->register_buffer_as_transient(context, range_in);
    pnanovdb_compute_buffer_transient_t* scratch_transient =
        compute_interface->register_buffer_as_transient(context, scratch_buffer);
    pnanovdb_compute_buffer_transient_t* workgroup_counter_transient =
        compute_interface->register_buffer_as_transient(context, workgroup_counter_buffer);

    for (pnanovdb_uint32_t pass_id = 0u; pass_id < 4u; pass_id++)
    {
        {
            pnanovdb_compute_resource_t resources[3u] = {};
            resources[0u].buffer_transient = constant_transient;
            resources[1u].buffer_transient = ijkl_transient;
            resources[2u].buffer_transient = nanovdb_transient;

            pnanovdb_uint32_t workgroup_count = (ijkl_count + 255u) / 256u;

            compute->dispatch_shader(compute_interface, context, ctx->shader_ctx[voxelbvh_nanovdb_set_mask_ijkl_slang],
                                     resources, workgroup_count, 1u, 1u, "voxelbvh_nanovdb_set_mask_ijkl");
        }

        if (pass_id == 3u)
        {
            break;
        }

        voxelbvh_nanovdb_add_nodes(compute, queue, voxelbvh_context, nanovdb_inout, nanovdb_word_count);
    }

    // set grid values to form level masks
    {
        pnanovdb_compute_resource_t resources[3u] = {};
        resources[0u].buffer_transient = constant_transient;
        resources[1u].buffer_transient = ijkl_transient;
        resources[2u].buffer_transient = nanovdb_transient;

        pnanovdb_uint32_t workgroup_count = (ijkl_count + 255u) / 256u;

        compute->dispatch_shader(compute_interface, context, ctx->shader_ctx[voxelbvh_nanovdb_set_value_ijkl_slang],
                                 resources, workgroup_count, 1u, 1u, "voxelbvh_nanovdb_set_value_ijkl");
    }

    // generate node mask buffer for grid operations
    pnanovdb_compute_buffer_t* node_mask_buffer = nullptr;
    {
        pnanovdb_compute_buffer_desc_t buf_desc = {};
        buf_desc.usage = PNANOVDB_COMPUTE_BUFFER_USAGE_STRUCTURED | PNANOVDB_COMPUTE_BUFFER_USAGE_RW_STRUCTURED;
        buf_desc.format = PNANOVDB_COMPUTE_FORMAT_UNKNOWN;
        buf_desc.structure_stride = 8u;
        buf_desc.size_in_bytes = node_mask_uint64_count * 8u;
        node_mask_buffer = compute_interface->create_buffer(context, PNANOVDB_COMPUTE_MEMORY_TYPE_DEVICE, &buf_desc);

        voxelbvh_generate_node_mask(compute, queue, voxelbvh_context, nanovdb_inout, nanovdb_word_count,
                                    node_mask_buffer, node_mask_uint64_count);
    }
    pnanovdb_compute_buffer_transient_t* node_mask_transient =
        compute_interface->register_buffer_as_transient(context, node_mask_buffer);

    // flatten grid value level masks
    {
        pnanovdb_compute_resource_t resources[4u] = {};
        resources[0u].buffer_transient = constant_transient;
        resources[1u].buffer_transient = nanovdb_transient;
        resources[2u].buffer_transient = node_mask_transient;
        resources[3u].buffer_transient = scratch_transient;

        compute->dispatch_shader(compute_interface, context, ctx->shader_ctx[voxelbvh_nanovdb_level_mask_flatten_slang],
                                 resources, 256u, 1u, 1u, "voxelbvh_nanovdb_level_mask_flatten");

        compute->dispatch_shader(compute_interface, context, ctx->shader_ctx[voxelbvh_nanovdb_iterate_copy_scratch_slang],
                                 resources, 256u, 1u, 1u, "voxelbvh_nanovdb_iterate_copy_scratch");
    }

    // allocate list indices for each voxel/tile
    {
        pnanovdb_compute_resource_t resources[5u] = {};
        resources[0u].buffer_transient = constant_transient;
        resources[1u].buffer_transient = nanovdb_transient;
        resources[2u].buffer_transient = node_mask_transient;
        resources[3u].buffer_transient = scratch_transient;
        resources[4u].buffer_transient = workgroup_counter_transient;

        compute->dispatch_shader(compute_interface, context, ctx->shader_ctx[voxelbvh_nanovdb_level_list_alloc1_slang],
                                 resources, 256u, 1u, 1u, "voxelbvh_nanovdb_level_list_alloc1");
        compute->dispatch_shader(compute_interface, context, ctx->shader_ctx[voxelbvh_nanovdb_level_list_alloc2_slang],
                                 resources, 256u, 1u, 1u, "voxelbvh_nanovdb_level_list_alloc2");
        compute->dispatch_shader(compute_interface, context, ctx->shader_ctx[voxelbvh_nanovdb_level_list_alloc3_slang],
                                 resources, 256u, 1u, 1u, "voxelbvh_nanovdb_level_list_alloc3");

        compute->dispatch_shader(compute_interface, context, ctx->shader_ctx[voxelbvh_nanovdb_iterate_copy_scratch_slang],
                                 resources, 256u, 1u, 1u, "voxelbvh_nanovdb_iterate_copy_scratch");
    }

    // splat lists to grid
    {
        pnanovdb_compute_resource_t resources[5u] = {};
        resources[0u].buffer_transient = constant_transient;
        resources[1u].buffer_transient = ijkl_transient;
        resources[2u].buffer_transient = range_transient;
        resources[3u].buffer_transient = nanovdb_transient;
        resources[4u].buffer_transient = range_flat_transient;

        pnanovdb_uint32_t workgroup_count = (ijkl_count + 255u) / 256u;

        compute->dispatch_shader(compute_interface, context, ctx->shader_ctx[voxelbvh_nanovdb_level_list_splat_slang],
                                 resources, workgroup_count, 1u, 1u, "voxelbvh_nanovdb_level_list_splat");
    }

    // flatten lists
    {
        pnanovdb_compute_resource_t resources[5u] = {};
        resources[0u].buffer_transient = constant_transient;
        resources[1u].buffer_transient = nanovdb_transient;
        resources[2u].buffer_transient = node_mask_transient;
        resources[3u].buffer_transient = scratch_transient;
        resources[4u].buffer_transient = range_flat_transient;

        compute->dispatch_shader(compute_interface, context, ctx->shader_ctx[voxelbvh_nanovdb_level_list_flatten_slang],
                                 resources, 256u, 1u, 1u, "voxelbvh_nanovdb_level_list_flatten");
    }

    // spread
    {
        pnanovdb_compute_resource_t resources[4u] = {};
        resources[0u].buffer_transient = constant_transient;
        resources[1u].buffer_transient = nanovdb_transient;
        resources[2u].buffer_transient = node_mask_transient;
        resources[3u].buffer_transient = scratch_transient;

        compute->dispatch_shader(compute_interface, context, ctx->shader_ctx[voxelbvh_nanovdb_level_list_spread_slang],
                                 resources, 256u, 1u, 1u, "voxelbvh_nanovdb_level_list_spread");

        compute->dispatch_shader(compute_interface, context, ctx->shader_ctx[voxelbvh_nanovdb_iterate_copy_scratch_slang],
                                 resources, 256u, 1u, 1u, "voxelbvh_nanovdb_iterate_copy_scratch");
    }

    // merge
    for (uint l = 1u; l <= 12; l++)
    {
        struct merge_arg_t
        {
            pnanovdb_uint32_t level;
        };
        merge_arg_t merge_arg = {};
        merge_arg.level = l;

        buf_desc.usage = PNANOVDB_COMPUTE_BUFFER_USAGE_CONSTANT;
        buf_desc.format = PNANOVDB_COMPUTE_FORMAT_UNKNOWN;
        buf_desc.structure_stride = 0u;
        buf_desc.size_in_bytes = sizeof(merge_arg_t);
        pnanovdb_compute_buffer_t* merge_arg_buffer =
            compute_interface->create_buffer(context, PNANOVDB_COMPUTE_MEMORY_TYPE_UPLOAD, &buf_desc);

        void* mapped_merge_arg = compute_interface->map_buffer(context, merge_arg_buffer);
        memcpy(mapped_merge_arg, &merge_arg, sizeof(merge_arg_t));
        compute_interface->unmap_buffer(context, merge_arg_buffer);

        pnanovdb_compute_buffer_transient_t* merge_arg_transient =
            compute_interface->register_buffer_as_transient(context, merge_arg_buffer);

        pnanovdb_compute_resource_t resources[5u] = {};
        resources[0u].buffer_transient = constant_transient;
        resources[1u].buffer_transient = nanovdb_transient;
        resources[2u].buffer_transient = node_mask_transient;
        resources[3u].buffer_transient = scratch_transient;
        resources[4u].buffer_transient = merge_arg_transient;

        compute->dispatch_shader(compute_interface, context, ctx->shader_ctx[voxelbvh_nanovdb_merge_voxels_slang],
                                 resources, 256u, 1u, 1u, "voxelbvh_nanovdb_merge_voxels");

        compute->dispatch_shader(compute_interface, context, ctx->shader_ctx[voxelbvh_nanovdb_iterate_copy_scratch_slang],
                                 resources, 256u, 1u, 1u, "voxelbvh_nanovdb_iterate_copy_scratch");

        compute_interface->destroy_buffer(context, merge_arg_buffer);
    }

    compute_interface->destroy_buffer(context, constant_buffer);
    compute_interface->destroy_buffer(context, node_mask_buffer);
    compute_interface->destroy_buffer(context, scratch_buffer);
    compute_interface->destroy_buffer(context, workgroup_counter_buffer);
}

void voxelbvh_nanovdb_add_nodes_from_key_array(const pnanovdb_compute_t* compute,
                                               pnanovdb_compute_queue_t* queue,
                                               pnanovdb_voxelbvh_context_t* voxelbvh_context,
                                               pnanovdb_compute_array_t** out_nanovdb,
                                               pnanovdb_compute_array_t** out_flat_range,
                                               pnanovdb_compute_array_t* ijkl_in,
                                               pnanovdb_compute_array_t* range_in)
{
    auto ctx = cast(voxelbvh_context);

    pnanovdb_compute_interface_t* compute_interface = compute->device_interface.get_compute_interface(queue);
    pnanovdb_compute_context_t* context = compute->device_interface.get_compute_context(queue);

    // default to 1GB return for now
    uint64_t buf_size = 1024llu * 1024llu * 1024llu;
    uint64_t nanovdb_uint64_count = (buf_size + 7u) / 8u;

    pnanovdb_coord_t root_coords[1u] = {};

    uint64_t ijkl_count = (ijkl_in->element_size * ijkl_in->element_count) / 8u;
    uint64_t range_count = (range_in->element_size * range_in->element_count) / 8u;

    pnanovdb_compute_array_t* nanovdb_array = compute->create_array(8u, nanovdb_uint64_count, nullptr);
    pnanovdb_compute_array_t* flat_range_array = compute->create_array(8u, nanovdb_uint64_count, nullptr);

    compute_gpu_array_t* ijkl_gpu_array = gpu_array_create();
    compute_gpu_array_t* range_gpu_array = gpu_array_create();
    compute_gpu_array_t* nanovdb_gpu_array = gpu_array_create();
    compute_gpu_array_t* flat_range_gpu_array = gpu_array_create();

    gpu_array_upload(compute, queue, ijkl_gpu_array, ijkl_in);
    gpu_array_upload(compute, queue, range_gpu_array, range_in);
    gpu_array_alloc_device(compute, queue, nanovdb_gpu_array, nanovdb_array);
    gpu_array_alloc_device(compute, queue, flat_range_gpu_array, flat_range_array);

    voxelbvh_nanovdb_init(
        compute, queue, voxelbvh_context, nanovdb_gpu_array->device_buffer, 2u * nanovdb_uint64_count, root_coords, 1u);

    voxelbvh_nanovdb_add_nodes_from_key_buffer(compute, queue, voxelbvh_context, nanovdb_gpu_array->device_buffer,
                                               2u * nanovdb_uint64_count, flat_range_gpu_array->device_buffer,
                                               nanovdb_uint64_count, ijkl_gpu_array->device_buffer,
                                               range_gpu_array->device_buffer, ijkl_count, range_count);

    gpu_array_readback(compute, queue, nanovdb_gpu_array, nanovdb_array);
    gpu_array_readback(compute, queue, flat_range_gpu_array, flat_range_array);

    pnanovdb_uint64_t flushed_frame = 0llu;
    compute->device_interface.flush(queue, &flushed_frame, nullptr, nullptr);

    compute->device_interface.wait_idle(queue);

    gpu_array_map(compute, queue, nanovdb_gpu_array, nanovdb_array);
    gpu_array_map(compute, queue, flat_range_gpu_array, flat_range_array);

    gpu_array_destroy(compute, queue, ijkl_gpu_array);
    gpu_array_destroy(compute, queue, range_gpu_array);
    gpu_array_destroy(compute, queue, nanovdb_gpu_array);
    gpu_array_destroy(compute, queue, flat_range_gpu_array);

    *out_nanovdb = nanovdb_array;
    *out_flat_range = flat_range_array;
}

void voxelbvh_from_gaussians(const pnanovdb_compute_t* compute,
                             pnanovdb_compute_queue_t* queue,
                             pnanovdb_voxelbvh_context_t* voxelbvh_context,
                             pnanovdb_compute_buffer_t** gaussian_array_buffers, // means, opacities, quats, scales,
                                                                                 // sh0, shn
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
        pnanovdb_uint32_t voxel_count;
        pnanovdb_uint32_t voxel_workgroup_count;
    };
    constants_t constants = {};
    constants.point_count = (pnanovdb_uint32_t)gaussian_count;
    constants.workgroup_count = (constants.point_count + 255u) / 256u;
    constants.voxel_count = 8u * constants.point_count;
    constants.voxel_workgroup_count = (constants.voxel_count + 255u) / 256u;

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

    buf_desc.usage = PNANOVDB_COMPUTE_BUFFER_USAGE_STRUCTURED | PNANOVDB_COMPUTE_BUFFER_USAGE_RW_STRUCTURED |
                     PNANOVDB_COMPUTE_BUFFER_USAGE_COPY_SRC | PNANOVDB_COMPUTE_BUFFER_USAGE_COPY_DST;
    buf_desc.format = PNANOVDB_COMPUTE_FORMAT_UNKNOWN;
    buf_desc.structure_stride = 4u;
    buf_desc.size_in_bytes = 6u * 4u * constants.workgroup_count;
    pnanovdb_compute_buffer_t* bbox_reduce1_buffer =
        compute_interface->create_buffer(context, PNANOVDB_COMPUTE_MEMORY_TYPE_DEVICE, &buf_desc);
    buf_desc.size_in_bytes = 6u * 4u;
    pnanovdb_compute_buffer_t* bbox_reduce2_buffer =
        compute_interface->create_buffer(context, PNANOVDB_COMPUTE_MEMORY_TYPE_DEVICE, &buf_desc);
    buf_desc.size_in_bytes = 8u * constants.voxel_count;
    pnanovdb_compute_buffer_t* keys_buffer =
        compute_interface->create_buffer(context, PNANOVDB_COMPUTE_MEMORY_TYPE_DEVICE, &buf_desc);
    buf_desc.size_in_bytes = 4u * constants.voxel_count;
    pnanovdb_compute_buffer_t* bbox_ids_buffer =
        compute_interface->create_buffer(context, PNANOVDB_COMPUTE_MEMORY_TYPE_DEVICE, &buf_desc);

    pnanovdb_compute_buffer_transient_t* bbox_reduce1_transient =
        compute_interface->register_buffer_as_transient(context, bbox_reduce1_buffer);
    pnanovdb_compute_buffer_transient_t* bbox_reduce2_transient =
        compute_interface->register_buffer_as_transient(context, bbox_reduce2_buffer);
    pnanovdb_compute_buffer_transient_t* keys_transient =
        compute_interface->register_buffer_as_transient(context, keys_buffer);
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

        compute->dispatch_shader(compute_interface, context, ctx->shader_ctx[voxelbvh_gaussians_bbox_reduce1_slang],
                                 resources, constants.workgroup_count, 1u, 1u, "voxelbvh_gaussians_bbox_reduce1");
    }
    {
        pnanovdb_compute_resource_t resources[3u] = {};
        resources[0u].buffer_transient = constant_transient;
        resources[1u].buffer_transient = bbox_reduce1_transient;
        resources[2u].buffer_transient = bbox_reduce2_transient;

        compute->dispatch_shader(compute_interface, context, ctx->shader_ctx[voxelbvh_gaussians_bbox_reduce2_slang],
                                 resources, 1u, 1u, 1u, "voxelbvh_gaussians_bbox_reduce2");
    }
    {
        pnanovdb_compute_resource_t resources[8u] = {};
        resources[0u].buffer_transient = constant_transient;
        resources[1u].buffer_transient = means_transient;
        resources[2u].buffer_transient = opacities_transient;
        resources[3u].buffer_transient = quats_transient;
        resources[4u].buffer_transient = scales_transient;
        resources[5u].buffer_transient = bbox_reduce2_transient;
        resources[6u].buffer_transient = keys_transient;
        resources[7u].buffer_transient = bbox_ids_transient;

        compute->dispatch_shader(compute_interface, context, ctx->shader_ctx[voxelbvh_gaussians_to_ijkl_slang],
                                 resources, constants.workgroup_count, 1u, 1u, "voxelbvh_gaussians_to_ijkl");
    }

    // sort ijk-level requests to bring requests together
    // radix sort
    {
        ctx->parallel_primitives.radix_sort_key64(compute, queue, ctx->parallel_primitives_ctx, keys_buffer,
                                                  bbox_ids_buffer, constants.voxel_count, constants.voxel_count, 64u);
    }

    buf_desc.size_in_bytes = 4u * constants.voxel_count;
    pnanovdb_compute_buffer_t* range_starts_buffer =
        compute_interface->create_buffer(context, PNANOVDB_COMPUTE_MEMORY_TYPE_DEVICE, &buf_desc);
    pnanovdb_compute_buffer_t* range_scan_buffer =
        compute_interface->create_buffer(context, PNANOVDB_COMPUTE_MEMORY_TYPE_DEVICE, &buf_desc);
    buf_desc.size_in_bytes = 2u * 4u * constants.voxel_count;
    pnanovdb_compute_buffer_t* range_headers_buffer =
        compute_interface->create_buffer(context, PNANOVDB_COMPUTE_MEMORY_TYPE_DEVICE, &buf_desc);

    pnanovdb_compute_buffer_transient_t* range_starts_transient =
        compute_interface->register_buffer_as_transient(context, range_starts_buffer);
    pnanovdb_compute_buffer_transient_t* range_scan_transient =
        compute_interface->register_buffer_as_transient(context, range_scan_buffer);
    pnanovdb_compute_buffer_transient_t* range_headers_transient =
        compute_interface->register_buffer_as_transient(context, range_headers_buffer);

    // identify range starts
    // voxelbvh_find_range_starts.slang
    {
        pnanovdb_compute_resource_t resources[4u] = {};
        resources[0u].buffer_transient = constant_transient;
        resources[1u].buffer_transient = keys_transient;
        resources[2u].buffer_transient = range_starts_transient;
        resources[3u].buffer_transient = range_headers_transient;

        compute->dispatch_shader(compute_interface, context, ctx->shader_ctx[voxelbvh_find_range_starts_slang],
                                 resources, constants.voxel_workgroup_count, 1u, 1u, "voxelbvh_find_range_starts");
    }

    // global scan to allocate range headers
    {
        ctx->parallel_primitives.global_scan(compute, queue, ctx->parallel_primitives_ctx, range_starts_buffer,
                                             range_scan_buffer, constants.voxel_count, 1u);
    }

    // scatter range headers
    // voxelbvh_scatter_range_headers.slang
    {
        pnanovdb_compute_resource_t resources[5u] = {};
        resources[0u].buffer_transient = constant_transient;
        resources[1u].buffer_transient = keys_transient;
        resources[2u].buffer_transient = range_starts_transient;
        resources[3u].buffer_transient = range_scan_transient;
        resources[4u].buffer_transient = range_headers_transient;

        compute->dispatch_shader(compute_interface, context, ctx->shader_ctx[voxelbvh_scatter_range_headers_slang],
                                 resources, constants.voxel_workgroup_count, 1u, 1u, "voxelbvh_scatter_range_headers");
    }

    pnanovdb_compute_array_t* debug_array = compute->create_array(4u, 2u * constants.voxel_count, nullptr);
    pnanovdb_compute_array_t* debug_array2 = compute->create_array(4u, 6u, nullptr);
    compute_gpu_array_t* debug_gpu_array = gpu_array_create();
    compute_gpu_array_t* debug_gpu_array2 = gpu_array_create();
    gpu_array_alloc_device(compute, queue, debug_gpu_array, debug_array);
    gpu_array_alloc_device(compute, queue, debug_gpu_array2, debug_array2);

    gpu_array_copy(compute, queue, debug_gpu_array, range_headers_buffer, 0llu, 4u * 2u * constants.voxel_count);
    gpu_array_copy(compute, queue, debug_gpu_array2, bbox_reduce2_buffer, 0llu, 4u * 6u);

    gpu_array_readback(compute, queue, debug_gpu_array, debug_array);
    gpu_array_readback(compute, queue, debug_gpu_array2, debug_array2);

    pnanovdb_uint64_t flushed_frame = 0llu;
    compute->device_interface.flush(queue, &flushed_frame, nullptr, nullptr);

    compute->device_interface.wait_idle(queue);

    gpu_array_map(compute, queue, debug_gpu_array, debug_array);
    gpu_array_map(compute, queue, debug_gpu_array2, debug_array2);

    pnanovdb_uint64_t valid_count = 0u;
    pnanovdb_uint64_t invalid_count = 0u;
    pnanovdb_uint32_t* mapped_debug = (pnanovdb_uint32_t*)debug_array->data;
    for (uint64_t idx = 0llu; idx < constants.voxel_count; idx++)
    {
        if (mapped_debug[2u * idx + 0u] < mapped_debug[2u * idx + 1u])
        {
            valid_count++;
        }
        else
        {
            invalid_count++;
        }
    }

    float* mapped_debug2 = (float*)debug_array2->data;
    printf("bbox{(%f,%f,%f), (%f,%f,%f)}\n", mapped_debug2[0], mapped_debug2[1], mapped_debug2[2], mapped_debug2[3],
           mapped_debug2[4], mapped_debug2[5]);

    printf("valid_count(%zu) invalid_count(%zu) voxel_count(%u)\n", valid_count, invalid_count, constants.voxel_count);

    gpu_array_destroy(compute, queue, debug_gpu_array);
    gpu_array_destroy(compute, queue, debug_gpu_array2);

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
    compute_interface->destroy_buffer(context, keys_buffer);
    compute_interface->destroy_buffer(context, bbox_ids_buffer);

    compute_interface->destroy_buffer(context, range_starts_buffer);
    compute_interface->destroy_buffer(context, range_scan_buffer);
    compute_interface->destroy_buffer(context, range_headers_buffer);
}

void voxelbvh_from_gaussians_file(const pnanovdb_compute_t* compute,
                                  pnanovdb_compute_queue_t* queue,
                                  pnanovdb_voxelbvh_context_t* voxelbvh_context,
                                  const char* filename)
{

    pnanovdb_fileformat_t fileformat = {};
    pnanovdb_fileformat_load(&fileformat, compute);

    const char* array_names_gaussian[] = { "means", "opacities", "quaternions", "scales", "sh_0", "sh_n" };
    pnanovdb_compute_array_t* arrays_gaussian[6] = {};

    pnanovdb_bool_t loaded_gaussian = fileformat.load_file(filename, 6, array_names_gaussian, arrays_gaussian);
    if (loaded_gaussian == PNANOVDB_TRUE)
    {
        pnanovdb_uint64_t gaussian_count = arrays_gaussian[1]->element_count;

        // normalize quats
        float* mapped_quat = (float*)compute->map_array(arrays_gaussian[2]);
        for (pnanovdb_uint64_t point_idx = 0u; point_idx < gaussian_count; point_idx++)
        {
            float x = mapped_quat[4u * point_idx + 1u];
            float y = mapped_quat[4u * point_idx + 2u];
            float z = mapped_quat[4u * point_idx + 3u];
            float w = mapped_quat[4u * point_idx + 0u];

            float magn_inv = 1.f / sqrtf(x * x + y * y + z * z + w * w);

            mapped_quat[4u * point_idx + 1u] = x * magn_inv;
            mapped_quat[4u * point_idx + 2u] = y * magn_inv;
            mapped_quat[4u * point_idx + 3u] = z * magn_inv;
            mapped_quat[4u * point_idx + 0u] = w * magn_inv;
        }
        compute->unmap_array(arrays_gaussian[2]);

        // transform scale
        float* mapped_scale = (float*)compute->map_array(arrays_gaussian[3]);
        for (pnanovdb_uint64_t point_idx = 0u; point_idx < gaussian_count; point_idx++)
        {
            mapped_scale[3u * point_idx + 0u] = expf(mapped_scale[3u * point_idx + 0u]);
            mapped_scale[3u * point_idx + 1u] = expf(mapped_scale[3u * point_idx + 1u]);
            mapped_scale[3u * point_idx + 2u] = expf(mapped_scale[3u * point_idx + 2u]);
        }
        compute->unmap_array(arrays_gaussian[3]);

        // transform opacity
        float* mapped_opacity = (float*)compute->map_array(arrays_gaussian[1]);
        for (pnanovdb_uint64_t point_idx = 0u; point_idx < gaussian_count; point_idx++)
        {
            mapped_opacity[point_idx] = 1.f / (1.f + expf(-mapped_opacity[point_idx]));
        }
        compute->unmap_array(arrays_gaussian[1]);

        compute_gpu_array_t* means_gpu_array = gpu_array_create();
        compute_gpu_array_t* opacities_gpu_array = gpu_array_create();
        compute_gpu_array_t* quaternions_gpu_array = gpu_array_create();
        compute_gpu_array_t* scales_gpu_array = gpu_array_create();
        compute_gpu_array_t* sh_0_gpu_array = gpu_array_create();
        compute_gpu_array_t* sh_n_gpu_array = gpu_array_create();

        gpu_array_upload(compute, queue, means_gpu_array, arrays_gaussian[0]);
        gpu_array_upload(compute, queue, opacities_gpu_array, arrays_gaussian[1]);
        gpu_array_upload(compute, queue, quaternions_gpu_array, arrays_gaussian[2]);
        gpu_array_upload(compute, queue, scales_gpu_array, arrays_gaussian[3]);
        gpu_array_upload(compute, queue, sh_0_gpu_array, arrays_gaussian[4]);
        gpu_array_upload(compute, queue, sh_n_gpu_array, arrays_gaussian[5]);

        pnanovdb_compute_buffer_t* gpu_buffers[6u] = {
            means_gpu_array->device_buffer,  opacities_gpu_array->device_buffer, quaternions_gpu_array->device_buffer,
            scales_gpu_array->device_buffer, sh_0_gpu_array->device_buffer,      sh_n_gpu_array->device_buffer
        };

        voxelbvh_from_gaussians(compute, queue, voxelbvh_context, gpu_buffers, 6u, gaussian_count, nullptr, 0llu);

        gpu_array_destroy(compute, queue, means_gpu_array);
        gpu_array_destroy(compute, queue, opacities_gpu_array);
        gpu_array_destroy(compute, queue, quaternions_gpu_array);
        gpu_array_destroy(compute, queue, scales_gpu_array);
        gpu_array_destroy(compute, queue, sh_0_gpu_array);
        gpu_array_destroy(compute, queue, sh_n_gpu_array);
    }

    for (pnanovdb_uint32_t idx = 0u; idx < 6u; idx++)
    {
        if (arrays_gaussian[idx])
        {
            compute->destroy_array(arrays_gaussian[idx]);
        }
    }
}

}

pnanovdb_voxelbvh_t* pnanovdb_get_voxelbvh()
{
    static pnanovdb_voxelbvh_t iface = { PNANOVDB_REFLECT_INTERFACE_INIT(pnanovdb_voxelbvh_t) };

    iface.create_context = create_context;
    iface.destroy_context = destroy_context;
    iface.voxelbvh_generate_node_mask = voxelbvh_generate_node_mask;
    iface.voxelbvh_generate_node_mask_array = voxelbvh_generate_node_mask_array;
    iface.voxelbvh_nanovdb_init = voxelbvh_nanovdb_init;
    iface.voxelbvh_nanovdb_add_nodes = voxelbvh_nanovdb_add_nodes;
    iface.voxelbvh_nanovdb_add_nodes_from_key_buffer = voxelbvh_nanovdb_add_nodes_from_key_buffer;
    iface.voxelbvh_nanovdb_add_nodes_from_key_array = voxelbvh_nanovdb_add_nodes_from_key_array;
    iface.voxelbvh_from_gaussians = voxelbvh_from_gaussians;
    iface.voxelbvh_from_gaussians_file = voxelbvh_from_gaussians_file;

    return &iface;
}
