// workgroup_voxelbvh.slang

#include "voxelbvh_common.h"

#ifndef WORKGROUP_VOXELBVH_THREADS
#define WORKGROUP_VOXELBVH_THREADS 128
#endif

groupshared uint64_t s_range[12u];

struct workgroup_voxelbvh_iterator_t
{
    uint active_level_count;
    uint total_count;
    uint pass_idx;
    uint pass_count;
};

void workgroup_voxelbvh_iterator_init(
    pnanovdb_buf_t iter_buf,
    inout workgroup_voxelbvh_iterator_t iter,
    uint thread_idx,
    uint max_pass_count,
    pnanovdb_grid_handle_t grid,
    uint64_t voxel_val_raw)
{
    uint active_level_mask = voxelbvh_get_active_level_mask(voxel_val_raw);
    uint first_range_idx = uint(voxelbvh_get_first_range_idx(voxel_val_raw));

    iter.pass_idx = 0u;
    iter.active_level_count = countbits(active_level_mask);
    if (thread_idx < 12u)
    {
        pnanovdb_uint64_t range = {};
        if (thread_idx < iter.active_level_count)
        {
            pnanovdb_address_t range_addr = pnanovdb_grid_get_gridblindmetadata_value_address(iter_buf, grid, 0u);
            range_addr = pnanovdb_address_offset_product(range_addr, 8u, first_range_idx);
            range = pnanovdb_read_uint64(iter_buf, pnanovdb_address_offset(range_addr, 8u * thread_idx));
        }
        s_range[thread_idx] = range;
    }
    GroupMemoryBarrierWithGroupSync();
    iter.total_count = 0u;
    for (uint active_level_idx = 0u; active_level_idx < iter.active_level_count; active_level_idx++)
    {
        pnanovdb_uint64_t range = s_range[active_level_idx];
        iter.total_count += uint(range >> 32u) - uint(range);
    }
    iter.pass_count = (iter.total_count + WORKGROUP_VOXELBVH_THREADS - 1u) / WORKGROUP_VOXELBVH_THREADS;
    if (iter.pass_count > max_pass_count)
    {
        iter.pass_count = max_pass_count;
    }
}

bool workgroup_voxelbvh_iterator_get(
    inout workgroup_voxelbvh_iterator_t iter,
    uint thread_idx,
    inout uint prim_id_index)
{
    if (iter.pass_idx >= iter.pass_count)
    {
        return false;
    }
    uint flat_prim_id_index = WORKGROUP_VOXELBVH_THREADS * iter.pass_idx + thread_idx;
    uint flat_accum = 0u;
    prim_id_index = ~0u;
    for (uint active_level_idx = 0u; active_level_idx < iter.active_level_count; active_level_idx++)
    {
        uint range_begin = uint(s_range[active_level_idx]);
        uint range_end = uint(s_range[active_level_idx] >> 32u);
        uint prim_id_index_cmp = range_begin + flat_prim_id_index - flat_accum;
        if (prim_id_index_cmp < range_end)
        {
            prim_id_index = prim_id_index_cmp;
            break;
        }
        flat_accum += (range_end - range_begin);
    }
    return true;
}

void workgroup_voxelbvh_iterator_step(
    inout workgroup_voxelbvh_iterator_t iter)
{
    iter.pass_idx++;
}

void workgroup_voxelbvh_iterator_reset(
    inout workgroup_voxelbvh_iterator_t iter)
{
    iter.pass_idx = 0u;
}

uint workgroup_voxelbvh_iterator_count_in_pass(
    inout workgroup_voxelbvh_iterator_t iter)
{
    uint count = iter.total_count - WORKGROUP_VOXELBVH_THREADS * iter.pass_idx;
    if (count > WORKGROUP_VOXELBVH_THREADS)
    {
        count = WORKGROUP_VOXELBVH_THREADS;
    }
    return count;
}
