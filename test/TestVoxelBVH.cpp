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

#if 0
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
#endif

#if 0
    uint64_t range_count = 16u * 1024u;
    uint64_t ijkl_count = 16u * 1024u;
    pnanovdb_compute_array_t* ijkl_array = compute.create_array(8u, ijkl_count, nullptr);
    pnanovdb_compute_array_t* range_array = compute.create_array(8u, ijkl_count, nullptr);
    uint64_t* mapped_ijkl = (uint64_t*)compute.map_array(ijkl_array);
    uint64_t* mapped_range = (uint64_t*)compute.map_array(range_array);
    for (uint64_t idx = 0u; idx < ijkl_count; idx++)
    {
        while (true)
        {
            int i = rand() & 4095;
            int j = rand() & 4095;
            int k = rand() & 4095;
            int l = rand() % 12;
            int lmask = ~((1 << l) - 1);
            i &= lmask;
            j &= lmask;
            k &= lmask;
            mapped_ijkl[idx] = (uint64_t(i << 16u) | (uint64_t(j) << 32u) |
                            (uint64_t(k) << 48u) | uint64_t(l));
            // verify uniqueness
            bool is_unique = true;
            for (uint64_t cmp_idx = 0u; cmp_idx < idx; cmp_idx++)
            {
                if (mapped_ijkl[idx] == mapped_ijkl[cmp_idx])
                {
                    is_unique = false;
                    break;
                }
            }
            if (is_unique)
            {
                break;
            }
        }
        mapped_range[idx] = uint64_t(idx) | (uint64_t(idx + 1u) << 32u);
    }
#else
    pnanovdb_compute_array_t* gaussian_arrays[6u] = {};

    pnanovdb_compute_array_t* ijkl_array = nullptr;
    pnanovdb_compute_array_t* prim_id_array = nullptr;
    pnanovdb_compute_array_t* range_array = nullptr;
    voxel_bvh.ijkl_from_gaussians_file(&compute, queue, voxelbvh_ctx, "./data/garden_eps2d03.ply", &ijkl_array,
                                       &prim_id_array, &range_array, gaussian_arrays, 6u);

    uint64_t range_count = range_array->element_count;
    uint64_t ijkl_count = ijkl_array->element_count;
    uint64_t* mapped_ijkl = (uint64_t*)compute.map_array(ijkl_array);
    uint64_t* mapped_range = (uint64_t*)compute.map_array(range_array);
#endif

    pnanovdb_compute_array_t* built_nanovdb_array = nullptr;
    pnanovdb_compute_array_t* built_flat_range_array = nullptr;
    voxel_bvh.nanovdb_add_nodes_from_ijkl_array(
        &compute, queue, voxelbvh_ctx, &built_nanovdb_array, &built_flat_range_array, ijkl_array, range_array);

    pnanovdb_buf_t buf = pnanovdb_make_buf((uint32_t*)built_nanovdb_array->data,
                                           built_nanovdb_array->element_size * built_nanovdb_array->element_count / 4u);
    pnanovdb_uint64_t* range_flat_ptr = (pnanovdb_uint64_t*)built_flat_range_array->data;

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
    pnanovdb_uint32_t max_list_idx = 0u;
    pnanovdb_uint64_t bit_count = 0u;
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
                            for (pnanovdb_uint32_t leaf_n = 0u; leaf_n < PNANOVDB_LEAF_TABLE_COUNT; leaf_n++)
                            {
                                pnanovdb_address_t val_addr =
                                    pnanovdb_leaf_get_table_address(grid_type, buf, leaf, leaf_n);
                                pnanovdb_uint64_t val = pnanovdb_read_uint64(buf, val_addr);
                                pnanovdb_uint32_t list_idx = pnanovdb_uint32_t(val >> 32u);
                                if (list_idx > max_list_idx)
                                {
                                    max_list_idx = list_idx;
                                }
                                bit_count += pnanovdb_uint64_countbits(val & 0xFFFF);
                            }
                        }
                        else
                        {
                            pnanovdb_address_t val_addr =
                                pnanovdb_lower_get_table_address(grid_type, buf, lower, lower_n);
                            pnanovdb_uint64_t val = pnanovdb_read_uint64(buf, val_addr);
                            pnanovdb_uint32_t list_idx = pnanovdb_uint32_t(val >> 32u);
                            if (list_idx > max_list_idx)
                            {
                                max_list_idx = list_idx;
                            }
                            bit_count += pnanovdb_uint64_countbits(val & 0xFFFF);
                        }
                    }
                }
                else
                {
                    pnanovdb_address_t val_addr = pnanovdb_upper_get_table_address(grid_type, buf, upper, upper_n);
                    pnanovdb_uint64_t val = pnanovdb_read_uint64(buf, val_addr);
                    pnanovdb_uint32_t list_idx = pnanovdb_uint32_t(val >> 32u);
                    if (list_idx > max_list_idx)
                    {
                        max_list_idx = list_idx;
                    }
                    bit_count += pnanovdb_uint64_countbits(val & 0xFFFF);
                }
            }
        }
        else
        {
            pnanovdb_address_t val_addr = pnanovdb_root_tile_get_value_address(grid_type, buf, tile);
            pnanovdb_uint64_t val = pnanovdb_read_uint64(buf, val_addr);
            pnanovdb_uint32_t list_idx = pnanovdb_uint32_t(val >> 32u);
            if (list_idx > max_list_idx)
            {
                max_list_idx = list_idx;
            }
            bit_count += pnanovdb_uint64_countbits(val & 0xFFFF);
        }
    }
    printf("node_counts: root(%u) upper(%u) lower(%u) leaf(%u)\n", root_tile_count, upper_count, lower_count, leaf_count);
    printf("bad node_counts: upper(%u) lower(%u) leaf(%u)\n", upper_count_bad, lower_count_bad, leaf_count_bad);
    printf("max_list_idx(%u) bit_count(%zu)\n", max_list_idx, bit_count);
    printf("grid_size(%zu)\n", grid_size);

    pnanovdb_readaccessor_t acc;
    pnanovdb_readaccessor_init(PNANOVDB_REF(acc), root);

    uint64_t unique_count = 0llu;
    uint64_t val_pass_count = 0llu;
    uint64_t not_leaf_count = 0llu;
    uint64_t list_mismatch_count = 0llu;
    uint64_t collision_count = 0llu;
    uint32_t range_count_max = 0u;
    uint32_t range_flat_count_max = 0u;
    uint32_t range_flat_mask_max = 0u;
    pnanovdb_address_t addr_old = {};
    for (uint64_t range_idx = 0u; range_idx < range_count; range_idx++)
    {
        uint64_t range = mapped_range[range_idx];
        uint32_t range_begin = uint32_t(range);
        uint32_t range_end = uint32_t(range >> 32u);
        for (uint32_t ijkl_idx = range_begin; ijkl_idx < range_end; ijkl_idx++)
        {
            uint64_t ijkl_raw = mapped_ijkl[ijkl_idx];
            pnanovdb_coord_t ijk = { int(ijkl_raw >> 48u) & 0xFFFF, int(ijkl_raw >> 32u) & 0xFFFF,
                                     int(ijkl_raw >> 16u) & 0xFFFF };

            // apply mask to go directly to expected voxel/tile
            int l = int(ijkl_raw) & 0xFFFF;
            if (l == 65535)
            {
                continue;
            }
            int lmask = ~((1 << l) - 1);
            ijk.x &= lmask;
            ijk.y &= lmask;
            ijk.z &= lmask;

            // track max range size only for valid keys
            uint32_t range_count = range_end - range_begin;
            if (range_count > range_count_max)
            {
                range_count_max = range_count;
            }

            pnanovdb_uint32_t level = 0u;
            pnanovdb_address_t addr = pnanovdb_readaccessor_get_value_address_and_level(
                grid_type, buf, PNANOVDB_REF(acc), PNANOVDB_REF(ijk), PNANOVDB_REF(level));
            pnanovdb_int64_t val = pnanovdb_read_int64(buf, addr);
            if ((val & (1llu << (ijkl_raw & 0xFFFF))) != 0u)
            {
                val_pass_count++;
            }

            uint active_lmask = uint(val & 0xFFFF);
            pnanovdb_uint32_t list_begin_idx = pnanovdb_uint32_t(val >> 32u);
            pnanovdb_uint32_t list_countbits = pnanovdb_uint32_countbits(active_lmask & ~lmask);
            pnanovdb_uint32_t list_idx = list_begin_idx + list_countbits;
            uint64_t range_val = range_flat_ptr[list_idx];
            if (list_countbits != 0u)
            {
                collision_count++;
            }
            if (range_val != range)
            {
                if (list_mismatch_count < 32u)
                {
                    printf("range_val(%u,%u) vs idx(%u,%d) list_countbits(%u) l(%u) ~lmask(0x%x)\n",
                           int(range_val >> 32u), int(range_val), int(range >> 32u), int(range), list_countbits, l,
                           ~lmask);
                }
                list_mismatch_count++;
            }

            pnanovdb_uint32_t range_flat_count = 0u;
            pnanovdb_uint32_t list_flat_countbits = pnanovdb_uint32_countbits(active_lmask & 0xFFFF);
            for (pnanovdb_uint32_t sub_idx = 0u; sub_idx < list_flat_countbits; sub_idx++)
            {
                uint64_t range_flat_val = range_flat_ptr[list_begin_idx + sub_idx];
                uint32_t range_flat_begin = uint32_t(range_flat_val);
                uint32_t range_flat_end = uint32_t(range_flat_val >> 32u);
                range_flat_count += range_flat_end - range_flat_begin;
            }
            if (range_flat_count > range_flat_count_max)
            {
                range_flat_count_max = range_flat_count;
                range_flat_mask_max = active_lmask;
            }

            if (addr.byte_offset != addr_old.byte_offset)
            {
                unique_count++;
            }
            if (level == 0u)
            {
                if (range_idx < 64u)
                {
                    pnanovdb_coord_t leaf_ijk = pnanovdb_leaf_get_bbox_min(buf, acc.leaf);
                    printf("leaf ijk(%d,%d,%d) vs point ijk(%d,%d,%d) val(%d, 0x%x)\n", leaf_ijk.x, leaf_ijk.y,
                           leaf_ijk.z, ijk.x, ijk.y, ijk.z, uint32_t(val >> 32u), uint32_t(val));
                }
            }
            else
            {
                not_leaf_count++;
            }
            addr_old = addr;
        }
    }

    printf("unique_count(%zu) val_pass_count(%zu) not_leaf_count(%zu)\n", unique_count, val_pass_count, not_leaf_count);
    printf("list_mismatch_count(%zu) collision_count(%zu) range_count_max(%d) range_flat_count_max(%d,0x%x)\n",
           list_mismatch_count, collision_count, range_count_max, range_flat_count_max, range_flat_mask_max);

    // print out some range_flat values
    for (uint64_t idx = 0u; idx < 64u; idx++)
    {
        uint64_t range_val = range_flat_ptr[idx];
        uint32_t range_begin = uint32_t(range_val);
        uint32_t range_end = uint32_t(range_val >> 32u);
        printf("range_flat[%zu] (%d, %d)\n", idx, range_begin, range_end);
    }

    // find first invalid key
    uint64_t last_valid_key = ijkl_count;
    for (uint64_t idx = 0u; idx < ijkl_count; idx++)
    {
        if ((mapped_ijkl[idx] & 0xFFFF) != 0xFFFF)
        {
            last_valid_key = idx;
        }
    }
    // shrink prim_id array
    prim_id_array->element_count = last_valid_key + 1u;

    // find first invalid range
    uint64_t last_valid_range = range_count;
    for (uint64_t idx = 0u; idx < range_count; idx++)
    {
        if (mapped_range[idx] != 0u)
        {
            last_valid_range = idx;
        }
    }
    // shrink ranges array
    range_array->element_count = last_valid_range + 1u;

    printf("Shrinking arrays prim_id(%zu vs %zu) range(%zu vs %zu)\n", prim_id_array->element_count, ijkl_count,
           range_array->element_count, range_count);

    pnanovdb_compute_array_t* metadata_arrays[8u] = { range_array,        prim_id_array,      gaussian_arrays[0],
                                                      gaussian_arrays[1], gaussian_arrays[2], gaussian_arrays[3],
                                                      gaussian_arrays[4], gaussian_arrays[5] };

    pnanovdb_compute_array_t* nanovdb_meta = nullptr;
    voxel_bvh.nanovdb_append_metadata(&compute, built_nanovdb_array, &nanovdb_meta, metadata_arrays, 8u);

    // check metadata
    {
        buf = pnanovdb_make_buf(
            (uint32_t*)nanovdb_meta->data, nanovdb_meta->element_size * nanovdb_meta->element_count / 4u);

        for (uint32_t list_begin_idx = 0u; list_begin_idx < 32u; list_begin_idx++)
        {
            pnanovdb_address_t range_addr = pnanovdb_grid_get_gridblindmetadata_value_address(buf, grid, 0u);
            pnanovdb_uint64_t range =
                pnanovdb_read_uint64(buf, pnanovdb_address_offset_product(range_addr, 8u, list_begin_idx));
            uint range_begin = uint(range);
            uint range_end = uint(range >> 32u);
            printf("list_begin_idx[%u] range_addr(%zu) range(%u,%u) ", list_begin_idx, range_addr.byte_offset,
                   range_begin, range_end);
            if (range_begin < range_end)
            {
                pnanovdb_address_t prim_id_addr = pnanovdb_grid_get_gridblindmetadata_value_address(buf, grid, 1u);
                pnanovdb_uint32_t prim_id =
                    pnanovdb_read_uint32(buf, pnanovdb_address_offset_product(prim_id_addr, 4u, range_begin));

                pnanovdb_address_t sh0_addr = pnanovdb_grid_get_gridblindmetadata_value_address(buf, grid, 6u);
                pnanovdb_vec3_t sh0 = pnanovdb_read_vec3(buf, pnanovdb_address_offset_product(sh0_addr, 12u, prim_id));

                printf("prim_id_addr(%zu) prim_id(%u) sh0_addr(%zu) sh0(%f,%f,%f)", prim_id_addr.byte_offset, prim_id,
                       sh0_addr.byte_offset, sh0.x, sh0.y, sh0.z);
            }
            printf("\n");
        }
    }

    // save NanoVDB out to disk
    compute.save_nanovdb(nanovdb_meta, "./data/voxelbvh.nvdb");

    compute.destroy_array(nanovdb_meta);
    for (uint32_t idx = 0u; idx < 6u; idx++)
    {
        compute.destroy_array(gaussian_arrays[idx]);
    }

    compute.destroy_array(ijkl_array);
    compute.destroy_array(prim_id_array);
    compute.destroy_array(range_array);
    compute.destroy_array(built_nanovdb_array);
    compute.destroy_array(built_flat_range_array);

    voxel_bvh.destroy_context(&compute, queue, voxelbvh_ctx);

    pnanovdb_voxelbvh_free(&voxel_bvh);

    compute.device_interface.destroy_device(device_manager, device);
    compute.device_interface.destroy_device_manager(device_manager);

    pnanovdb_compute_free(&compute);
    pnanovdb_compiler_free(&compiler);
}
