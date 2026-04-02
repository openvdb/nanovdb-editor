// voxelbvh_nanovdb_level_list_flatten.slang

struct user_state_t
{
    int unused;
}

#include "voxelbvh_nanovdb_iterate.slang"

RWStructuredBuffer<uint2> range_flat_inout;

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

    pnanovdb_uint64_t ref_val = pnanovdb_read_uint64(buf, val_addr);
    if (uint(ref_val & 0xFFFF) != 0)
    {
        for (uint l = 0u; l < 12; l++)
        {
            if ((uint(ref_val & 0xFFFF) & (1u << l)) != 0u)
            {
                uint lmask = ~((1u << l) - 1u);
                pnanovdb_coord_t ijkmask = ijk & lmask;

                pnanovdb_address_t read_val_addr = pnanovdb_root_get_value_address(grid_type, buf, root, ijkmask);
                pnanovdb_uint64_t read_val = pnanovdb_read_uint64(buf, read_val_addr);

                uint ref_list_idx = uint(ref_val >> 32u) + countbits(uint(ref_val) & ~lmask);
                uint read_list_idx = uint(read_val >> 32u) + countbits(uint(read_val) & ~lmask);
                if (ref_list_idx != read_list_idx)
                {
                    range_flat_inout[ref_list_idx] = range_flat_inout[read_list_idx];
                }
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
