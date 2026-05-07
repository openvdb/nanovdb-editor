// voxelbvh_nanovdb_level_list_spread.slang

struct user_state_t
{
    int unused;
}

#include "voxelbvh_nanovdb_iterate.slang"

void iterate_main(pnanovdb_address_t node_arr, pnanovdb_address_t val_addr, uint level, uint n,
                  pnanovdb_coord_t ijk, inout user_state_t user_state)
{
    if (pnanovdb_address_is_null(val_addr))
    {
        return;
    }

    // Note: single grid assumption
    pnanovdb_grid_handle_t grid = { pnanovdb_address_null() };
    pnanovdb_uint32_t grid_type = pnanovdb_grid_get_grid_type(buf, grid);
    pnanovdb_tree_handle_t tree = pnanovdb_grid_get_tree(buf, grid);
    pnanovdb_root_handle_t root = pnanovdb_tree_get_root(buf, tree);

    pnanovdb_uint64_t val = pnanovdb_read_uint64(buf, val_addr);
    if (uint(val & 0xFFFF) == 0)
    {
        for (uint l = 1u; l < 12; l++)
        {
            uint lmask = ~((1u << l) - 1u);
            pnanovdb_coord_t ijkmask = ijk & lmask;

            pnanovdb_address_t read_val_addr = pnanovdb_root_get_value_address(grid_type, buf, root, ijkmask);
            pnanovdb_uint64_t read_val = pnanovdb_read_uint64(buf, read_val_addr);

            uint read_active_level_mask = uint(read_val) & 0xFFFF;
            if ((read_active_level_mask & lmask) != 0u)
            {
                uint read_list_begin_idx = uint(read_val >> 32u);
                uint read_skip_level = (uint(read_val) >> 16u) & 0xFFFF;

                // the lsbs of the active level mask are false positives for this voxel
                // we should dump the lsbs of the active level mask and offset the list begin accordingly
                uint active_level_mask = read_active_level_mask & lmask;
                uint list_begin_idx = read_list_begin_idx + countbits(read_active_level_mask & ~lmask);
                uint skip_level = read_skip_level;

                val = (uint64_t(list_begin_idx) << 32u) | uint64_t((skip_level << 16u) | active_level_mask);
                break;
            }
        }
    }

    pnanovdb_write_uint64(buf_scratch, val_addr, val);
}

[shader("compute")][numthreads(256, 1, 1)]
void main(uint3 group_idx : SV_GroupID, uint3 thread_idx : SV_GroupThreadID)
{
    user_state_t user_state = {};
    do_iteration(group_idx.x, thread_idx.x, user_state);
}
