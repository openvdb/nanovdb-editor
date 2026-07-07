// voxelbvh_nanovdb_index_allocate3.slang

struct user_state_t
{
    uint64_t global_offset;
}

#include "voxelbvh_nanovdb_iterate.slang"
#include "voxelbvh_common.h"
#include <workgroup_scan_uint64.slang>

RWStructuredBuffer<uint2> workgroup_counters_in;
RWStructuredBuffer<uint2> total_count_out;

void iterate_main(pnanovdb_address_t node_arr, pnanovdb_address_t val_addr, uint level, uint n,
                  pnanovdb_coord_t ijk, inout user_state_t user_state)
{
    uint thread_idx = n & 255;

    // Note: single grid assumption
    pnanovdb_grid_handle_t grid = { pnanovdb_address_null() };
    pnanovdb_uint32_t grid_type = pnanovdb_grid_get_grid_type(buf, grid);

    bool is_active = false;
    if (level == 3u) // root
    {
        // count number of root tiles with active value
        pnanovdb_root_handle_t root = {node_arr};
        pnanovdb_uint32_t tile_count = pnanovdb_root_get_tile_count(buf, root);
        if (n < tile_count)
        {
            pnanovdb_root_tile_handle_t tile = pnanovdb_root_get_tile(grid_type, root, n);
            pnanovdb_uint32_t state = pnanovdb_root_tile_get_state(buf, tile);
            if (state != 0u && !pnanovdb_root_tile_get_child_mask(buf, tile))
            {
                is_active = true;
            }
        }
    }
    else if (level == 2u) // upper
    {
        // count number of active values
        pnanovdb_upper_handle_t upper = {node_arr};
        if (pnanovdb_upper_get_value_mask(buf, upper, n) &&
           !pnanovdb_upper_get_child_mask(buf, upper, n))
        {
            is_active = true;
        }
    }
    else if (level == 1u) // lower
    {
        // count number of active values
        pnanovdb_lower_handle_t lower = {node_arr};
        if (pnanovdb_lower_get_value_mask(buf, lower, n) &&
           !pnanovdb_lower_get_child_mask(buf, lower, n))
        {
            is_active = true;
        }
    }
    else if (level == 0u) // leaf
    {
        // count number of active values only
        pnanovdb_leaf_handle_t leaf = {node_arr};
        if (pnanovdb_leaf_get_value_mask(buf, leaf, n))
        {
            is_active = true;
        }
    }

    vector<uint64_t, 4> total_count4 = vector<uint64_t, 4>(is_active ? 1llu : 0llu, 0llu, 0llu, 0llu);
    vector<uint64_t, 4> scan_total_count4 = vector<uint64_t, 4>(0llu, 0llu, 0llu, 0llu);
    uint64_t workgroup_total_count = 0llu;
    workgroup_scan_uint64(thread_idx, total_count4, scan_total_count4, workgroup_total_count);

    uint64_t index_val = scan_total_count4.x - total_count4.x;

    if (level == 3u && is_active) // root
    {
        pnanovdb_write_uint64(buf, val_addr, index_val);
    }
    else if (level == 2u && is_active) // upper
    {
        pnanovdb_write_uint64(buf, val_addr, index_val);
    }
    else if (level == 1u && is_active) // lower
    {
        pnanovdb_write_uint64(buf, val_addr, index_val);
    }
    else if (level == 0u) // leaf
    {
        if (grid_type == PNANOVDB_GRID_TYPE_INDEX ||
            grid_type == PNANOVDB_GRID_TYPE_ONINDEX)
        {
            if (n == 0u)
            {
                pnanovdb_write_uint64(buf, val_addr, index_val);

                pnanovdb_leaf_handle_t leaf = {node_arr};
                // need to update the prefix sum based on the mask
                uint64_t prefix_sum = 0llu;
                for (uint word_idx = 0u; word_idx < 7u; word_idx++)
                {
                    uint64_t val_mask = pnanovdb_read_uint64(buf,
                        pnanovdb_address_offset(leaf.address, PNANOVDB_LEAF_OFF_VALUE_MASK + 8u * word_idx));
                    uint sum = pnanovdb_uint64_countbits(val_mask);
                    prefix_sum = prefix_sum | (uint64_t(sum) << (9 * word_idx));
                }
                pnanovdb_write_uint64(buf, pnanovdb_address_offset(val_addr, 8u), prefix_sum);
            }
        }
        else // dense leaf nodes
        {
            if (is_active)
            {
                pnanovdb_write_uint64(buf, val_addr, index_val);
            }
        }
    }

    user_state.global_offset += workgroup_total_count;
}

[shader("compute")][numthreads(256, 1, 1)]
void main(uint3 group_idx : SV_GroupID, uint3 thread_idx : SV_GroupThreadID)
{
    user_state_t user_state = {};
    uint64_t total_count = uint2_to_uint64_t(workgroup_counters_in[group_idx.x]);
    uint64_t scan_total_count = uint2_to_uint64_t(workgroup_counters_in[256u + group_idx.x]);
    user_state.global_offset = scan_total_count - total_count;
    do_iteration(group_idx.x, thread_idx.x, user_state);

    if (group_idx.x == 0u && thread_idx.x == 0u)
    {
        // grab last element of scan to get total
        total_count_out[0u] = workgroup_counters_in[256u + 255u];
    }
}
