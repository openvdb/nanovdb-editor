// voxelbvh_nanovdb_index_allocate1.slang

struct user_state_t
{
    uint64_t count;
}

#include "voxelbvh_nanovdb_iterate.slang"
#include "voxelbvh_common.h"
#include <workgroup_scan_uint64.slang>

RWStructuredBuffer<uint2> workgroup_counters_out;

void iterate_main(pnanovdb_address_t node_arr, pnanovdb_address_t val_addr, uint level, uint n,
                  pnanovdb_coord_t ijk, inout user_state_t user_state)
{
    // Note: single grid assumption
    pnanovdb_grid_handle_t grid = { pnanovdb_address_null() };
    pnanovdb_uint32_t grid_type = pnanovdb_grid_get_grid_type(buf, grid);

    if (level == 4u) // background
    {
        // allocate a single background value
        if (n == 0u)
        {
            user_state.count++;
        }
    }
    else if (level == 3u) // root
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
                user_state.count++;
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
            user_state.count++;
        }
    }
    else if (level == 1u) // lower
    {
        // count number of active values
        pnanovdb_lower_handle_t lower = {node_arr};
        if (pnanovdb_lower_get_value_mask(buf, lower, n) &&
           !pnanovdb_lower_get_child_mask(buf, lower, n))
        {
            user_state.count++;
        }
    }
    else if (level == 0u) // leaf
    {
        // count number of active values only
        pnanovdb_leaf_handle_t leaf = {node_arr};
        if (pnanovdb_leaf_get_value_mask(buf, lower, n))
        {
            user_state.count++;
        }
    }
}

uint2 uint64_t_to_uint2(uint64_t v)
{
    return uint2(uint(v), uint(v >> 32u));
}

[shader("compute")][numthreads(256, 1, 1)]
void main(uint3 group_idx : SV_GroupID, uint3 thread_idx : SV_GroupThreadID)
{
    user_state_t user_state = {};
    do_iteration(group_idx.x, thread_idx.x, user_state);

    vector<uint64_t, 4> total_count4 = vector<uint64_t, 4>(user_state.count, 0llu, 0llu, 0llu);
    uint64_t workgroup_total_count = 0llu;
    workgroup_reduce_uint64(thread_idx.x, total_count4, workgroup_total_count);

    if (thread_idx.x == 0u)
    {
        workgroup_counters_out[group_idx.x] = uint64_t_to_uint2(workgroup_total_count);
    }
}
