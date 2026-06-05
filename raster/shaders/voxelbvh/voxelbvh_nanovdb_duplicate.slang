// voxelbvh_nanovdb_duplicate.slang

struct user_state_t
{
    int unused;
}

#include "voxelbvh_nanovdb_iterate.slang"
#include "voxelbvh_common.h"

void nanovdb_set_bit(RWStructuredBuffer<uint2> write_buf, pnanovdb_address_t addr, uint bit_idx)
{
    uint64_t word_idx = (addr.byte_offset >> 3u) + (bit_idx >> 6u);
    uint64_t val = uint64_t(1u) << (bit_idx & 63u);
    uint2 val2 = uint2(uint(val), uint(val >> 32u));
    if (val2.x != 0u) { InterlockedOr(write_buf[word_idx].x, val2.x); };
    if (val2.y != 0u) { InterlockedOr(write_buf[word_idx].y, val2.y); };
}

bool find_child_bit(
    RWStructuredBuffer<uint2> lookup_buf,
    pnanovdb_grid_handle_t grid,
    uint pass_id,
    pnanovdb_coord_t ijk,
    inout pnanovdb_address_t child_mask_addr,
    inout uint child_mask_n)
{
    pnanovdb_uint32_t grid_type = pnanovdb_grid_get_grid_type(lookup_buf, grid);
    pnanovdb_tree_handle_t tree = pnanovdb_grid_get_tree(lookup_buf, grid);
    pnanovdb_root_handle_t root = pnanovdb_tree_get_root(lookup_buf, tree);

    child_mask_addr = pnanovdb_address_null();
    child_mask_n = 0u;
    pnanovdb_root_tile_handle_t tile = pnanovdb_root_find_tile(grid_type, lookup_buf, root, ijk);
    if (!pnanovdb_address_is_null(tile.address))
    {
        int64_t child_off = pnanovdb_root_tile_get_child(lookup_buf, tile);
        if (pass_id == 0u)
        {
            child_mask_addr = pnanovdb_address_offset(tile.address, PNANOVDB_ROOT_TILE_OFF_CHILD);
            child_mask_n = 0u;
        }
        else
        {
            if (child_off != 0)
            {
                pnanovdb_upper_handle_t upper = pnanovdb_root_get_child(grid_type, lookup_buf, root, tile);
                pnanovdb_uint32_t upper_n = pnanovdb_upper_coord_to_offset(ijk);
                if (pass_id == 1u)
                {
                    child_mask_addr = pnanovdb_address_offset(upper.address, PNANOVDB_UPPER_OFF_CHILD_MASK);
                    child_mask_n = upper_n;
                }
                else
                {
                    if (pnanovdb_upper_get_child_mask(lookup_buf, upper, upper_n))
                    {
                        pnanovdb_lower_handle_t lower = pnanovdb_upper_get_child(grid_type, lookup_buf, upper, upper_n);
                        pnanovdb_uint32_t lower_n = pnanovdb_lower_coord_to_offset(ijk);
                        if (pass_id == 2u)
                        {
                            child_mask_addr = pnanovdb_address_offset(lower.address, PNANOVDB_LOWER_OFF_CHILD_MASK);
                            child_mask_n = lower_n;
                        }
                    }
                }
            }
        }
    }
    return !pnanovdb_address_is_null(child_mask_addr);
}

void iterate_main(pnanovdb_address_t node_arr, pnanovdb_address_t val_addr, uint level, uint n,
                  pnanovdb_coord_t ijk, inout user_state_t user_state)
{
    // Note: single grid assumption
    pnanovdb_grid_handle_t grid = { pnanovdb_address_null() };
    pnanovdb_uint32_t grid_type = pnanovdb_grid_get_grid_type(buf, grid);

    uint upsample = constants.ijkl_count;
    uint pass_id = constants.range_count;

    bool src_child_is_active = false;
    if (level == 3u && pass_id == 0u) // root
    {
        pnanovdb_root_handle_t root = {node_arr};
        pnanovdb_uint32_t tile_count = pnanovdb_root_get_tile_count(buf, root);
        if (n < tile_count)
        {
            pnanovdb_root_tile_handle_t tile = pnanovdb_root_get_tile(grid_type, root, n);
            if (pnanovdb_root_tile_get_child_mask(buf, tile))
            {
                src_child_is_active = true;
            }
        }
    }
    else if (level == 2u && pass_id == 1u) // upper
    {
        pnanovdb_upper_handle_t upper = {node_arr};
        if (pnanovdb_upper_get_child_mask(buf, upper, n))
        {
            src_child_is_active = true;
        }
    }
    else if (level == 1u && pass_id == 2u) // lower
    {
        pnanovdb_lower_handle_t lower = {node_arr};
        if (pnanovdb_lower_get_child_mask(buf, lower, n))
        {
            src_child_is_active = true;
        }
#if 0
        else if (grid_type == PNANOVDB_GRID_TYPE_INT64)
        {
            // Note: assuming VoxelBVH
            // Add extra leaves for mid sized primitives
            // Mask applied here is a heuristic
            pnanovdb_uint64_t voxel_val_raw = pnanovdb_read_uint64(buf, val_addr);
            uint active_level_mask = voxelbvh_get_active_level_mask(voxel_val_raw);
            if ((active_level_mask & 0x1F) != 0u)
            {
                src_child_is_active = true;
            }
        }
#endif
    }
    else if (level == 0u) // leaf
    {
        // no-op
    }

    if (src_child_is_active)
    {
        // Note: single grid assumption
        pnanovdb_grid_handle_t dst_grid = { pnanovdb_address_null() };

        if (upsample != 0u)
        {
            for (uint sample_idx = 0u; sample_idx < 8u; sample_idx++)
            {
                uint shift = 0u;
                if (level == 0u) { shift = 0; }
                if (level == 1u) { shift = 3; }
                if (level == 2u) { shift = 7; }
                if (level == 3u) { shift = 12; }

                int3 ijk_upsample = int3(
                    2 * ijk.x + (((sample_idx >> 2) & 1) << shift),
                    2 * ijk.y + (((sample_idx >> 1) & 1) << shift),
                    2 * ijk.z + (((sample_idx >> 0) & 1) << shift)
                );

                pnanovdb_address_t child_mask_addr = {};
                uint child_mask_n = 0u;
                if (find_child_bit(
                    buf_scratch,
                    dst_grid,
                    pass_id,
                    ijk_upsample,
                    child_mask_addr,
                    child_mask_n))
                {
                    nanovdb_set_bit(buf_scratch, child_mask_addr, child_mask_n);
                }
            }
        }
        else
        {
            pnanovdb_address_t child_mask_addr = {};
            uint child_mask_n = 0u;
            if (find_child_bit(
                buf_scratch,
                dst_grid,
                pass_id,
                ijk,
                child_mask_addr,
                child_mask_n))
            {
                nanovdb_set_bit(buf_scratch, child_mask_addr, child_mask_n);
            }
        }
    }
}

[shader("compute")][numthreads(256, 1, 1)]
void main(uint3 group_idx : SV_GroupID, uint3 thread_idx : SV_GroupThreadID)
{
    user_state_t user_state = {};
    do_iteration(group_idx.x, thread_idx.x, user_state);
}
