// voxelbvh_nanovdb_merge_voxels.slang

struct user_state_t
{
    int unused;
}

#include "voxelbvh_nanovdb_iterate.slang"

struct merge_arg_t
{
    uint level;
};
ConstantBuffer<merge_arg_t> merge_arg_in;

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

    uint l = merge_arg_in.level;
    int flip_mask = 1 << l;

    if ((uint(val >> 16u) & 0xFFFF) == l)
    {
        bool all_match = true;
        for (uint i = 1u; i < 8; i++)
        {
            pnanovdb_coord_t ijkmask = ijk;
            if ((i & 1) != 0u)
            {
                ijkmask.z = ijkmask.z ^ flip_mask;
            }
            if ((i & 2) != 0u)
            {
                ijkmask.y = ijkmask.y ^ flip_mask;
            }
            if ((i & 4) != 0u)
            {
                ijkmask.x = ijkmask.x ^ flip_mask;
            }
            pnanovdb_address_t read_val_addr = pnanovdb_root_get_value_address(grid_type, buf, root, ijkmask);
            pnanovdb_uint64_t read_val = pnanovdb_read_uint64(buf, read_val_addr);
            if (read_val != val)
            {
                all_match = false;
            }
        }
        if (all_match)
        {
            val = (val & ~uint64_t(0xFFFF0000)) | uint64_t(((l + 1) & 0xFFFF) << 16u);
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
