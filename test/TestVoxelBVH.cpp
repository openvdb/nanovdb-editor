// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/test/main.cpp

    \author Petra Hapalova, Andrew Reidmeyer

    \brief
*/

#include <nanovdb_editor/putil/Raster.h>
#include <nanovdb_editor/putil/Editor.h>
#include <nanovdb_editor/putil/FileFormat.h>
#include <nanovdb_editor/putil/Reflect.h>
#include <nanovdb_editor/putil/VoxelBVH.h>

#include <cstdarg>

static void log_print(pnanovdb_compute_log_level_t level, const char* format, ...)
{
    va_list args;
    va_start(args, format);

    const char* prefix = "Unknown";
    if (level == PNANOVDB_COMPUTE_LOG_LEVEL_ERROR)
    {
        prefix = "Error";
    }
    else if (level == PNANOVDB_COMPUTE_LOG_LEVEL_WARNING)
    {
        prefix = "Warning";
    }
    else if (level == PNANOVDB_COMPUTE_LOG_LEVEL_INFO)
    {
        prefix = "Info";
    }
    else if (level == PNANOVDB_COMPUTE_LOG_LEVEL_DEBUG)
    {
        // prefix = "Debug";
        va_end(args);
        return;
    }
    printf("%s: ", prefix);
    vprintf(format, args);
    printf("\n");

    va_end(args);
}

void voxelbvh_test()
{
    // load compiler and compute
    pnanovdb_compiler_t compiler = {};
    pnanovdb_compiler_load(&compiler);
    pnanovdb_compute_t compute = {};
    pnanovdb_compute_load(&compute, &compiler);

    // create device
    pnanovdb_compute_device_desc_t device_desc = {};
    device_desc.log_print = log_print;
    pnanovdb_compute_device_manager_t* device_manager = compute.device_interface.create_device_manager(PNANOVDB_FALSE);
    pnanovdb_compute_device_t* device = compute.device_interface.create_device(device_manager, &device_desc);
    pnanovdb_compute_queue_t* queue = compute.device_interface.get_compute_queue(device);

    pnanovdb_voxelbvh_t voxel_bvh = {};
    pnanovdb_voxelbvh_load(&voxel_bvh, &compute);

    auto voxelbvh_ctx = voxel_bvh.create_context(&compute, queue);

    voxel_bvh.voxelbvh_from_gaussians_file(&compute, queue, voxelbvh_ctx, "./data/splats.npz");

    pnanovdb_compute_array_t* nanovdb_arr = compute.load_nanovdb("./data/dragon.nvdb");

    pnanovdb_compute_array_t* node_mask_arr =
        voxel_bvh.voxelbvh_generate_node_mask_array(&compute, queue, voxelbvh_ctx, nanovdb_arr);

    uint32_t node_type_count[16u] = {};
    uint64_t node_type_max[16u] = {};
    for (uint64_t idx = 0u; idx < node_mask_arr->element_count; idx++)
    {
        uint64_t type_raw = ((uint64_t*)node_mask_arr->data)[idx];
        for (uint32_t sub_idx = 0u; sub_idx < 16u; sub_idx++)
        {
            uint32_t type = uint32_t(type_raw >> (sub_idx << 2)) & 15;
            node_type_count[type]++;
            node_type_max[type] = 32u * (16u * idx + sub_idx);
        }
    }
    for (uint32_t type = 0u; type < 16u; type++)
    {
        printf("type[%d] count(%d) max(%zu)\n", type, node_type_count[type], node_type_max[type]);
    }
    compute.destroy_array(node_mask_arr);
    compute.destroy_array(nanovdb_arr);

    uint64_t ijkl_count = 16u * 1024u;
    pnanovdb_compute_array_t* ijkl_array = compute.create_array(8u, ijkl_count, nullptr);
    uint64_t* mapped_ijkl = (uint64_t*)compute.map_array(ijkl_array);
    for (uint64_t idx = 0u; idx < ijkl_count; idx++)
    {
        mapped_ijkl[idx] = uint64_t(rand() & 4095) | (uint64_t(rand() & 4095) << 16u) | (uint64_t(rand() & 4095) << 32u);
    }

    pnanovdb_compute_array_t* built_nanovdb_array =
        voxel_bvh.voxelbvh_nanovdb_add_nodes_from_key_array(&compute, queue, voxelbvh_ctx, PNANOVDB_FALSE, ijkl_array);

    pnanovdb_buf_t buf = pnanovdb_make_buf(
        (uint32_t*)built_nanovdb_array->data,
        built_nanovdb_array->element_size * built_nanovdb_array->element_count / 4u);

    pnanovdb_grid_handle_t grid = {};
    pnanovdb_tree_handle_t tree = pnanovdb_grid_get_tree(buf, grid);
    pnanovdb_root_handle_t root = pnanovdb_tree_get_root(buf, tree);

    printf("root(%zu)\n", root.address.byte_offset);

    uint64_t grid_size = pnanovdb_grid_get_grid_size(buf, grid);

    pnanovdb_uint32_t grid_type = pnanovdb_grid_get_grid_type(buf, grid);

    printf("grid_type(%u)\n", grid_type);

    // list out root tiles
    pnanovdb_uint32_t root_tile_count = pnanovdb_root_get_tile_count(buf, root);
    pnanovdb_uint32_t upper_count = 0u;
    pnanovdb_uint32_t lower_count = 0u;
    pnanovdb_uint32_t leaf_count = 0u;
    pnanovdb_uint32_t upper_count_bad = 0u;
    pnanovdb_uint32_t lower_count_bad = 0u;
    pnanovdb_uint32_t leaf_count_bad = 0u;
    for (pnanovdb_uint32_t root_n = 0u; root_n < root_tile_count; root_n++)
    {
        pnanovdb_root_tile_handle_t tile = pnanovdb_root_get_tile(grid_type, root, root_n);
        pnanovdb_int64_t child = pnanovdb_root_tile_get_child(buf, tile);
        printf("root_tile[%u] child(%zu)\n", root_n, child);
        if (child)
        {
            pnanovdb_upper_handle_t upper = pnanovdb_root_get_child(grid_type, buf, root, tile);
            if (upper.address.byte_offset == root.address.byte_offset)
            {
                upper_count_bad++;
                continue;
            }
            upper_count++;
            if (upper_count < 16u)
            {
                printf("upper[%zu]\n", upper.address.byte_offset);
            }
            for (pnanovdb_uint32_t upper_n = 0u; upper_n < PNANOVDB_UPPER_TABLE_COUNT; upper_n++)
            {
                if (pnanovdb_upper_get_child_mask(buf, upper, upper_n))
                {
                    pnanovdb_lower_handle_t lower = pnanovdb_upper_get_child(grid_type, buf, upper, upper_n);
                    if (lower.address.byte_offset == upper.address.byte_offset)
                    {
                        lower_count_bad++;
                        continue;
                    }
                    lower_count++;
                    if (lower_count < 16u)
                    {
                        printf("lower[%zu]\n", lower.address.byte_offset);
                    }
                    for (pnanovdb_uint32_t lower_n = 0u; lower_n < PNANOVDB_LOWER_TABLE_COUNT; lower_n++)
                    {
                        if (pnanovdb_lower_get_child_mask(buf, lower, lower_n))
                        {
                            pnanovdb_leaf_handle_t leaf = pnanovdb_lower_get_child(grid_type, buf, lower, lower_n);
                            if (leaf.address.byte_offset == lower.address.byte_offset)
                            {
                                leaf_count_bad++;
                                continue;
                            }
                            leaf_count++;
                            if (leaf_count < 16u)
                            {
                                printf("leaf[%zu]\n", leaf.address.byte_offset);
                            }
                        }
                    }
                }
            }
        }
    }
    printf("node_counts: root(%u) upper(%u) lower(%u) leaf(%u)\n", root_tile_count, upper_count, lower_count, leaf_count);
    printf("bad node_counts: upper(%u) lower(%u) leaf(%u)\n", upper_count_bad, lower_count_bad, leaf_count_bad);

    uint64_t unique_count = 0llu;
    pnanovdb_address_t addr_old = {};
    for (uint64_t idx = 0u; idx < ijkl_count; idx++)
    {
        uint64_t ijkl_raw = mapped_ijkl[idx];
        pnanovdb_coord_t ijk = {
            int(ijkl_raw >> 32u) & 0xFFFF,
            int(ijkl_raw >> 16u) & 0xFFFF,
            int(ijkl_raw) & 0xFFFF
        };
        pnanovdb_address_t addr = pnanovdb_root_get_value_address(grid_type, buf, root, PNANOVDB_REF(ijk));
        if (addr.byte_offset != addr_old.byte_offset)
        {
            unique_count++;
        }
        addr_old = addr;
    }

    printf("grid_size(%zu) unique_count(%zu)\n", grid_size, unique_count);

    compute.destroy_array(ijkl_array);

    voxel_bvh.destroy_context(&compute, queue, voxelbvh_ctx);

    pnanovdb_voxelbvh_free(&voxel_bvh);

    compute.device_interface.destroy_device(device_manager, device);
    compute.device_interface.destroy_device_manager(device_manager);

    pnanovdb_compute_free(&compute);
    pnanovdb_compiler_free(&compiler);
}
