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
    voxelbvh_nanovdb_clear_slang,
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
    "raster/voxelbvh/voxelbvh_find_range_starts.slang",
    "raster/voxelbvh/voxelbvh_gaussians_bbox_reduce1.slang",
    "raster/voxelbvh/voxelbvh_gaussians_bbox_reduce2.slang",
    "raster/voxelbvh/voxelbvh_gaussians_to_ijkl.slang",
    "raster/voxelbvh/voxelbvh_nanovdb_add_count.slang",
    "raster/voxelbvh/voxelbvh_nanovdb_add_link.slang",
    "raster/voxelbvh/voxelbvh_nanovdb_add_scan.slang",
    "raster/voxelbvh/voxelbvh_nanovdb_clear.slang",
    "raster/voxelbvh/voxelbvh_nanovdb_find_clear.slang",
    "raster/voxelbvh/voxelbvh_nanovdb_find_leaves.slang",
    "raster/voxelbvh/voxelbvh_nanovdb_find_lowers.slang",
    "raster/voxelbvh/voxelbvh_nanovdb_find_root.slang",
    "raster/voxelbvh/voxelbvh_nanovdb_find_uppers.slang",
    "raster/voxelbvh/voxelbvh_nanovdb_init.slang",

    "raster/voxelbvh/voxelbvh_nanovdb_iterate_copy_scratch.slang",
    "raster/voxelbvh/voxelbvh_nanovdb_level_list_alloc1.slang",
    "raster/voxelbvh/voxelbvh_nanovdb_level_list_alloc2.slang",
    "raster/voxelbvh/voxelbvh_nanovdb_level_list_alloc3.slang",
    "raster/voxelbvh/voxelbvh_nanovdb_level_list_flatten.slang",
    "raster/voxelbvh/voxelbvh_nanovdb_level_list_splat.slang",
    "raster/voxelbvh/voxelbvh_nanovdb_level_list_spread.slang",
    "raster/voxelbvh/voxelbvh_nanovdb_level_mask_flatten.slang",
    "raster/voxelbvh/voxelbvh_nanovdb_merge_voxels.slang",

    "raster/voxelbvh/voxelbvh_nanovdb_set_mask_ijkl.slang",
    "raster/voxelbvh/voxelbvh_nanovdb_set_value_ijkl.slang",
    "raster/voxelbvh/voxelbvh_scatter_range_headers.slang",
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

void nanovdb_generate_node_mask(const pnanovdb_compute_t* compute,
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

static pnanovdb_compute_array_t* nanovdb_generate_node_mask_array(const pnanovdb_compute_t* compute,
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

    nanovdb_generate_node_mask(compute, queue, voxelbvh_context, nanovdb_gpu_array->device_buffer, nanovdb_word_count,
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

static void nanovdb_clear(const pnanovdb_compute_t* compute,
                          pnanovdb_compute_queue_t* queue,
                          pnanovdb_voxelbvh_context_t* voxelbvh_context,
                          pnanovdb_compute_buffer_t* nanovdb_inout,
                          pnanovdb_uint64_t nanovdb_word_count)
{
    auto ctx = cast(voxelbvh_context);

    pnanovdb_compute_interface_t* compute_interface = compute->device_interface.get_compute_interface(queue);
    pnanovdb_compute_context_t* context = compute->device_interface.get_compute_context(queue);

    struct constants_t
    {
        pnanovdb_uint32_t nanovdb_word_count;
    };
    constants_t constants = {};
    constants.nanovdb_word_count = nanovdb_word_count;

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

    {
        pnanovdb_compute_resource_t resources[2u] = {};
        resources[0u].buffer_transient = constant_transient;
        resources[1u].buffer_transient = nanovdb_transient;

        compute->dispatch_shader(compute_interface, context, ctx->shader_ctx[voxelbvh_nanovdb_clear_slang], resources,
                                 256u, 1u, 1u, "voxelbvh_nanovdb_clear");
    }

    compute_interface->destroy_buffer(context, constant_buffer);
}

static void nanovdb_init(const pnanovdb_compute_t* compute,
                         pnanovdb_compute_queue_t* queue,
                         pnanovdb_voxelbvh_context_t* voxelbvh_context,
                         pnanovdb_compute_buffer_t* nanovdb_inout,
                         pnanovdb_uint64_t nanovdb_word_count,
                         pnanovdb_compute_buffer_t* world_bbox_in,
                         pnanovdb_uint32_t integer_space_max,
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
    struct constants_t
    {
        pnanovdb_uint32_t grid_size;
        pnanovdb_uint32_t integer_space_max;
        pnanovdb_uint32_t pad0;
        pnanovdb_uint32_t pad1;
    };
    constants_t constants = {};
    constants.grid_size = (pnanovdb_uint32_t)size;
    constants.integer_space_max = integer_space_max;

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

    // allocate upload buffer
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

    pnanovdb_compute_buffer_transient_t* constant_transient =
        compute_interface->register_buffer_as_transient(context, constant_buffer);
    pnanovdb_compute_buffer_transient_t* upload_transient =
        compute_interface->register_buffer_as_transient(context, upload_buffer);
    pnanovdb_compute_buffer_transient_t* nanovdb_transient =
        compute_interface->register_buffer_as_transient(context, nanovdb_inout);
    pnanovdb_compute_buffer_transient_t* world_bbox_transient =
        compute_interface->register_buffer_as_transient(context, world_bbox_in);

    // clear buffer before initialization, to ensure everything starts 0
    nanovdb_clear(compute, queue, voxelbvh_context, nanovdb_inout, nanovdb_word_count);

    // voxelbvh_nanovdb_init.slang
    {
        pnanovdb_compute_resource_t resources[4u] = {};
        resources[0u].buffer_transient = constant_transient;
        resources[1u].buffer_transient = upload_transient;
        resources[2u].buffer_transient = nanovdb_transient;
        resources[3u].buffer_transient = world_bbox_transient;

        pnanovdb_uint32_t workgroup_count = ((size / 8u) + 255u) / 256u;

        compute->dispatch_shader(compute_interface, context, ctx->shader_ctx[voxelbvh_nanovdb_init_slang], resources,
                                 workgroup_count, 1u, 1u, "voxelbvh_nanovdb_init");
    }

    compute_interface->destroy_buffer(context, upload_buffer);
    compute_interface->destroy_buffer(context, constant_buffer);
}

static void nanovdb_add_nodes(const pnanovdb_compute_t* compute,
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

    nanovdb_generate_node_mask(
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

static void nanovdb_add_nodes_from_ijkl_buffer(const pnanovdb_compute_t* compute,
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

    // clear scratch buffer for now
    nanovdb_clear(compute, queue, voxelbvh_context, scratch_buffer, nanovdb_word_count);

    buf_desc.size_in_bytes = 8u * 256u * 2u;
    pnanovdb_compute_buffer_t* workgroup_counter_buffer =
        compute_interface->create_buffer(context, PNANOVDB_COMPUTE_MEMORY_TYPE_DEVICE, &buf_desc);

    struct constants_t
    {
        pnanovdb_uint32_t nanovdb_word_count;
        pnanovdb_uint32_t ijkl_count;
        pnanovdb_uint32_t nanovdb_chunk_count;
        pnanovdb_uint32_t node_mask_uint64_count;
        pnanovdb_uint32_t range_count;
    };
    constants_t constants = {};
    constants.nanovdb_word_count = nanovdb_word_count;
    constants.ijkl_count = ijkl_count;
    constants.nanovdb_chunk_count = nanovdb_word_count >> 3u;
    constants.node_mask_uint64_count = node_mask_uint64_count;
    constants.range_count = range_count;

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
            pnanovdb_compute_resource_t resources[4u] = {};
            resources[0u].buffer_transient = constant_transient;
            resources[1u].buffer_transient = ijkl_transient;
            resources[2u].buffer_transient = range_transient;
            resources[3u].buffer_transient = nanovdb_transient;

            pnanovdb_uint32_t workgroup_count = (range_count + 255u) / 256u;

            compute->dispatch_shader(compute_interface, context, ctx->shader_ctx[voxelbvh_nanovdb_set_mask_ijkl_slang],
                                     resources, workgroup_count, 1u, 1u, "voxelbvh_nanovdb_set_mask_ijkl");
        }

        if (pass_id == 3u)
        {
            break;
        }

        nanovdb_add_nodes(compute, queue, voxelbvh_context, nanovdb_inout, nanovdb_word_count);
    }

    // set grid values to form level masks
    {
        pnanovdb_compute_resource_t resources[4u] = {};
        resources[0u].buffer_transient = constant_transient;
        resources[1u].buffer_transient = ijkl_transient;
        resources[2u].buffer_transient = range_transient;
        resources[3u].buffer_transient = nanovdb_transient;

        pnanovdb_uint32_t workgroup_count = (range_count + 255u) / 256u;

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

        nanovdb_generate_node_mask(compute, queue, voxelbvh_context, nanovdb_inout, nanovdb_word_count,
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
    }

    // splat lists to grid
    {
        pnanovdb_compute_resource_t resources[5u] = {};
        resources[0u].buffer_transient = constant_transient;
        resources[1u].buffer_transient = ijkl_transient;
        resources[2u].buffer_transient = range_transient;
        resources[3u].buffer_transient = nanovdb_transient;
        resources[4u].buffer_transient = range_flat_transient;

        pnanovdb_uint32_t workgroup_count = (range_count + 255u) / 256u;

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
    for (uint l = 0u; l < 12; l++)
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

static void nanovdb_add_nodes_from_ijkl_array(const pnanovdb_compute_t* compute,
                                              pnanovdb_compute_queue_t* queue,
                                              pnanovdb_voxelbvh_context_t* voxelbvh_context,
                                              pnanovdb_compute_array_t** out_nanovdb,
                                              pnanovdb_compute_array_t** out_flat_range,
                                              pnanovdb_compute_array_t* ijkl_in,
                                              pnanovdb_compute_array_t* range_in,
                                              pnanovdb_compute_array_t* world_bbox_in,
                                              pnanovdb_uint32_t integer_space_max)
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
    compute_gpu_array_t* world_bbox_gpu_array = gpu_array_create();
    compute_gpu_array_t* nanovdb_gpu_array = gpu_array_create();
    compute_gpu_array_t* flat_range_gpu_array = gpu_array_create();

    gpu_array_upload(compute, queue, ijkl_gpu_array, ijkl_in);
    gpu_array_upload(compute, queue, range_gpu_array, range_in);
    gpu_array_upload(compute, queue, world_bbox_gpu_array, world_bbox_in);
    gpu_array_alloc_device(compute, queue, nanovdb_gpu_array, nanovdb_array);
    gpu_array_alloc_device(compute, queue, flat_range_gpu_array, flat_range_array);

    nanovdb_init(compute, queue, voxelbvh_context, nanovdb_gpu_array->device_buffer, 2u * nanovdb_uint64_count,
                 world_bbox_gpu_array->device_buffer, integer_space_max, root_coords, 1u);

    nanovdb_add_nodes_from_ijkl_buffer(compute, queue, voxelbvh_context, nanovdb_gpu_array->device_buffer,
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
    gpu_array_destroy(compute, queue, world_bbox_gpu_array);
    gpu_array_destroy(compute, queue, nanovdb_gpu_array);
    gpu_array_destroy(compute, queue, flat_range_gpu_array);

    *out_nanovdb = nanovdb_array;
    *out_flat_range = flat_range_array;
}

static void ijkl_from_gaussians(const pnanovdb_compute_t* compute,
                                pnanovdb_compute_queue_t* queue,
                                pnanovdb_voxelbvh_context_t* voxelbvh_context,
                                pnanovdb_compute_buffer_t** gaussian_array_buffers, // means, opacities, quats, scales,
                                                                                    // sh0, shn
                                pnanovdb_uint32_t gaussian_array_count,
                                pnanovdb_uint64_t gaussian_count,
                                pnanovdb_compute_buffer_t* ijkl_out,
                                pnanovdb_compute_buffer_t* prim_id_out,
                                pnanovdb_compute_buffer_t* range_out,
                                pnanovdb_compute_buffer_t* world_bbox_out,
                                pnanovdb_uint32_t integer_space_max)
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
        pnanovdb_uint32_t integer_space_max;
    };
    constants_t constants = {};
    constants.point_count = (pnanovdb_uint32_t)gaussian_count;
    constants.workgroup_count = (constants.point_count + 255u) / 256u;
    constants.voxel_count = 8u * constants.point_count;
    constants.voxel_workgroup_count = (constants.voxel_count + 255u) / 256u;
    constants.integer_space_max = integer_space_max;

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
    // buf_desc.size_in_bytes = 8u * constants.voxel_count;
    // pnanovdb_compute_buffer_t* keys_buffer =
    //     compute_interface->create_buffer(context, PNANOVDB_COMPUTE_MEMORY_TYPE_DEVICE, &buf_desc);
    // buf_desc.size_in_bytes = 4u * constants.voxel_count;
    // pnanovdb_compute_buffer_t* bbox_ids_buffer =
    //     compute_interface->create_buffer(context, PNANOVDB_COMPUTE_MEMORY_TYPE_DEVICE, &buf_desc);

    pnanovdb_compute_buffer_transient_t* bbox_reduce1_transient =
        compute_interface->register_buffer_as_transient(context, bbox_reduce1_buffer);
    pnanovdb_compute_buffer_transient_t* ijkl_transient =
        compute_interface->register_buffer_as_transient(context, ijkl_out);
    pnanovdb_compute_buffer_transient_t* prim_id_transient =
        compute_interface->register_buffer_as_transient(context, prim_id_out);
    pnanovdb_compute_buffer_transient_t* world_bbox_transient =
        compute_interface->register_buffer_as_transient(context, world_bbox_out);

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
        resources[2u].buffer_transient = world_bbox_transient;

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
        resources[5u].buffer_transient = world_bbox_transient;
        resources[6u].buffer_transient = ijkl_transient;
        resources[7u].buffer_transient = prim_id_transient;

        compute->dispatch_shader(compute_interface, context, ctx->shader_ctx[voxelbvh_gaussians_to_ijkl_slang],
                                 resources, constants.workgroup_count, 1u, 1u, "voxelbvh_gaussians_to_ijkl");
    }

    // sort ijk-level requests to bring requests together
    // radix sort
    {
        ctx->parallel_primitives.radix_sort_key64(compute, queue, ctx->parallel_primitives_ctx, ijkl_out, prim_id_out,
                                                  constants.voxel_count, constants.voxel_count, 64u);
    }

    buf_desc.size_in_bytes = 4u * constants.voxel_count;
    pnanovdb_compute_buffer_t* range_starts_buffer =
        compute_interface->create_buffer(context, PNANOVDB_COMPUTE_MEMORY_TYPE_DEVICE, &buf_desc);
    pnanovdb_compute_buffer_t* range_scan_buffer =
        compute_interface->create_buffer(context, PNANOVDB_COMPUTE_MEMORY_TYPE_DEVICE, &buf_desc);
    // buf_desc.size_in_bytes = 2u * 4u * constants.voxel_count;
    // pnanovdb_compute_buffer_t* range_headers_buffer =
    //     compute_interface->create_buffer(context, PNANOVDB_COMPUTE_MEMORY_TYPE_DEVICE, &buf_desc);

    pnanovdb_compute_buffer_transient_t* range_starts_transient =
        compute_interface->register_buffer_as_transient(context, range_starts_buffer);
    pnanovdb_compute_buffer_transient_t* range_scan_transient =
        compute_interface->register_buffer_as_transient(context, range_scan_buffer);
    pnanovdb_compute_buffer_transient_t* range_transient =
        compute_interface->register_buffer_as_transient(context, range_out);

    // identify range starts
    // voxelbvh_find_range_starts.slang
    {
        pnanovdb_compute_resource_t resources[4u] = {};
        resources[0u].buffer_transient = constant_transient;
        resources[1u].buffer_transient = ijkl_transient;
        resources[2u].buffer_transient = range_starts_transient;
        resources[3u].buffer_transient = range_transient;

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
        resources[1u].buffer_transient = ijkl_transient;
        resources[2u].buffer_transient = range_starts_transient;
        resources[3u].buffer_transient = range_scan_transient;
        resources[4u].buffer_transient = range_transient;

        compute->dispatch_shader(compute_interface, context, ctx->shader_ctx[voxelbvh_scatter_range_headers_slang],
                                 resources, constants.voxel_workgroup_count, 1u, 1u, "voxelbvh_scatter_range_headers");
    }

    compute_interface->destroy_buffer(context, constant_buffer);
    compute_interface->destroy_buffer(context, bbox_reduce1_buffer);

    compute_interface->destroy_buffer(context, range_starts_buffer);
    compute_interface->destroy_buffer(context, range_scan_buffer);
}

static void ijkl_from_gaussians_file(const pnanovdb_compute_t* compute,
                                     pnanovdb_compute_queue_t* queue,
                                     pnanovdb_voxelbvh_context_t* voxelbvh_context,
                                     const char* filename,
                                     pnanovdb_compute_array_t** ijkl_out,
                                     pnanovdb_compute_array_t** prim_id_out,
                                     pnanovdb_compute_array_t** range_out,
                                     pnanovdb_compute_array_t** world_bbox_out,
                                     pnanovdb_uint32_t integer_space_max,
                                     pnanovdb_compute_array_t** gaussian_arrays_out,
                                     pnanovdb_uint32_t gaussian_array_count)
{

    pnanovdb_fileformat_t fileformat = {};
    pnanovdb_fileformat_load(&fileformat, compute);

    const char* array_names_gaussian[] = { "means", "opacities", "quaternions", "scales", "sh_0", "sh_n" };
    pnanovdb_compute_array_t* arrays_gaussian[6] = {};

    pnanovdb_bool_t loaded_gaussian = fileformat.load_file(filename, 6, array_names_gaussian, arrays_gaussian);
    if (loaded_gaussian == PNANOVDB_TRUE)
    {
        pnanovdb_uint64_t gaussian_count = arrays_gaussian[1]->element_count;

        pnanovdb_uint64_t voxel_count = 8u * gaussian_count;

        pnanovdb_compute_array_t* ijkl_array = compute->create_array(8u, voxel_count, nullptr);
        pnanovdb_compute_array_t* prim_id_array = compute->create_array(4u, voxel_count, nullptr);
        pnanovdb_compute_array_t* range_array = compute->create_array(8u, voxel_count, nullptr);
        pnanovdb_compute_array_t* world_bbox_array = compute->create_array(4u, 6u, nullptr);

        compute_gpu_array_t* ijkl_gpu_array = gpu_array_create();
        compute_gpu_array_t* prim_id_gpu_array = gpu_array_create();
        compute_gpu_array_t* range_gpu_array = gpu_array_create();
        compute_gpu_array_t* world_bbox_gpu_array = gpu_array_create();

        gpu_array_alloc_device(compute, queue, ijkl_gpu_array, ijkl_array);
        gpu_array_alloc_device(compute, queue, prim_id_gpu_array, prim_id_array);
        gpu_array_alloc_device(compute, queue, range_gpu_array, range_array);
        gpu_array_alloc_device(compute, queue, world_bbox_gpu_array, world_bbox_array);

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

        ijkl_from_gaussians(compute, queue, voxelbvh_context, gpu_buffers, 6u, gaussian_count,
                            ijkl_gpu_array->device_buffer, prim_id_gpu_array->device_buffer,
                            range_gpu_array->device_buffer, world_bbox_gpu_array->device_buffer, integer_space_max);

        gpu_array_destroy(compute, queue, means_gpu_array);
        gpu_array_destroy(compute, queue, opacities_gpu_array);
        gpu_array_destroy(compute, queue, quaternions_gpu_array);
        gpu_array_destroy(compute, queue, scales_gpu_array);
        gpu_array_destroy(compute, queue, sh_0_gpu_array);
        gpu_array_destroy(compute, queue, sh_n_gpu_array);

        // readback results
        gpu_array_readback(compute, queue, ijkl_gpu_array, ijkl_array);
        gpu_array_readback(compute, queue, prim_id_gpu_array, prim_id_array);
        gpu_array_readback(compute, queue, range_gpu_array, range_array);
        gpu_array_readback(compute, queue, world_bbox_gpu_array, world_bbox_array);

        pnanovdb_uint64_t flushed_frame = 0llu;
        compute->device_interface.flush(queue, &flushed_frame, nullptr, nullptr);

        compute->device_interface.wait_idle(queue);

        gpu_array_map(compute, queue, ijkl_gpu_array, ijkl_array);
        gpu_array_map(compute, queue, prim_id_gpu_array, prim_id_array);
        gpu_array_map(compute, queue, range_gpu_array, range_array);
        gpu_array_map(compute, queue, world_bbox_gpu_array, world_bbox_array);

        *ijkl_out = ijkl_array;
        *prim_id_out = prim_id_array;
        *range_out = range_array;
        *world_bbox_out = world_bbox_array;

        gpu_array_destroy(compute, queue, ijkl_gpu_array);
        gpu_array_destroy(compute, queue, prim_id_gpu_array);
        gpu_array_destroy(compute, queue, range_gpu_array);
        gpu_array_destroy(compute, queue, world_bbox_gpu_array);
    }

    if (gaussian_arrays_out && gaussian_array_count == 6u)
    {
        for (pnanovdb_uint32_t idx = 0u; idx < 6u; idx++)
        {
            gaussian_arrays_out[idx] = arrays_gaussian[idx];
        }
    }
    else
    {
        for (pnanovdb_uint32_t idx = 0u; idx < 6u; idx++)
        {
            if (arrays_gaussian[idx])
            {
                compute->destroy_array(arrays_gaussian[idx]);
            }
        }
    }
}

static void nanovdb_append_metadata(const pnanovdb_compute_t* compute,
                                    pnanovdb_compute_array_t* nanovdb_in,
                                    pnanovdb_compute_array_t** nanovdb_out,
                                    pnanovdb_compute_array_t** metadata_arrays,
                                    pnanovdb_uint32_t metadata_count)
{
    pnanovdb_buf_t src_buf =
        pnanovdb_make_buf((uint32_t*)nanovdb_in->data, nanovdb_in->element_size * nanovdb_in->element_count / 4u);
    pnanovdb_grid_handle_t src_grid = {};
    pnanovdb_uint64_t src_grid_size = pnanovdb_grid_get_grid_size(src_buf, src_grid);

    pnanovdb_uint64_t total_size = src_grid_size;
    for (pnanovdb_uint32_t metadata_idx = 0u; metadata_idx < metadata_count; metadata_idx++)
    {
        pnanovdb_uint64_t raw_size =
            metadata_arrays[metadata_idx]->element_size * metadata_arrays[metadata_idx]->element_count;
        pnanovdb_uint64_t aligned_size = 32u * ((raw_size + 31u) / 32u);
        total_size += aligned_size;
        total_size += PNANOVDB_GRIDBLINDMETADATA_SIZE;
    }

    pnanovdb_uint64_t nanovdb_uint64_count = (total_size + 7u) / 8u;
    *nanovdb_out = compute->create_array(8u, nanovdb_uint64_count, nullptr);

    pnanovdb_buf_t buf = pnanovdb_make_buf(
        (uint32_t*)(*nanovdb_out)->data, (*nanovdb_out)->element_size * (*nanovdb_out)->element_count / 4u);
    pnanovdb_grid_handle_t grid = {};
    pnanovdb_uint64_t grid_size = pnanovdb_grid_get_grid_size(buf, grid);

    // copy original grid
    memcpy(buf.data, src_buf.data, src_grid_size);

    // add metadata headers
    pnanovdb_grid_set_blind_metadata_offset(buf, grid, src_grid_size - grid.address.byte_offset);
    pnanovdb_grid_set_blind_metadata_count(buf, grid, metadata_count);

    pnanovdb_address_t meta_addr = { src_grid_size + PNANOVDB_GRIDBLINDMETADATA_SIZE * metadata_count };
    for (pnanovdb_uint32_t metadata_idx = 0u; metadata_idx < metadata_count; metadata_idx++)
    {
        pnanovdb_uint64_t raw_size =
            metadata_arrays[metadata_idx]->element_size * metadata_arrays[metadata_idx]->element_count;
        pnanovdb_uint64_t aligned_size = 32u * ((raw_size + 31u) / 32u);

        pnanovdb_gridblindmetadata_handle_t meta = pnanovdb_grid_get_gridblindmetadata(buf, grid, metadata_idx);

        pnanovdb_gridblindmetadata_set_data_offset(buf, meta, pnanovdb_address_diff(meta_addr, meta.address));
        pnanovdb_gridblindmetadata_set_value_count(buf, meta, metadata_arrays[metadata_idx]->element_count);
        pnanovdb_gridblindmetadata_set_value_size(buf, meta, metadata_arrays[metadata_idx]->element_size);

        memcpy(((uint8_t*)buf.data) + meta_addr.byte_offset, metadata_arrays[metadata_idx]->data, raw_size);

        meta_addr = pnanovdb_address_offset64(meta_addr, aligned_size);
    }

    pnanovdb_grid_set_grid_size(buf, grid, pnanovdb_address_diff(meta_addr, pnanovdb_address_null()));
}

}

pnanovdb_voxelbvh_t* pnanovdb_get_voxelbvh()
{
    static pnanovdb_voxelbvh_t iface = { PNANOVDB_REFLECT_INTERFACE_INIT(pnanovdb_voxelbvh_t) };

    iface.create_context = create_context;
    iface.destroy_context = destroy_context;
    iface.nanovdb_generate_node_mask = nanovdb_generate_node_mask;
    iface.nanovdb_generate_node_mask_array = nanovdb_generate_node_mask_array;
    iface.nanovdb_init = nanovdb_init;
    iface.nanovdb_add_nodes = nanovdb_add_nodes;
    iface.nanovdb_add_nodes_from_ijkl_buffer = nanovdb_add_nodes_from_ijkl_buffer;
    iface.nanovdb_add_nodes_from_ijkl_array = nanovdb_add_nodes_from_ijkl_array;
    iface.ijkl_from_gaussians = ijkl_from_gaussians;
    iface.ijkl_from_gaussians_file = ijkl_from_gaussians_file;
    iface.nanovdb_append_metadata = nanovdb_append_metadata;

    return &iface;
}
