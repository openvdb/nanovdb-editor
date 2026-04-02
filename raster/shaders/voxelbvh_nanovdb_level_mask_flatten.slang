// voxelbvh_nanovdb_level_mask_flatten.slang

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
    if ((uint(val) & 0xFFFF) != 0u)
    {
        for (uint l = 0u; l <= 12; l++)
        {
            uint lmask = ~((1u << l) - 1u);
            pnanovdb_coord_t ijkmask = ijk & lmask;

            pnanovdb_address_t read_val_addr = pnanovdb_root_get_value_address(grid_type, buf, root, ijkmask);

            pnanovdb_uint32_t val_read = pnanovdb_read_uint32(buf, read_val_addr);

            val = val | (val_read & (1u << l));
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
