// voxelbvh_nanovdb_iterate_copy_scratch.slang

struct user_state_t
{
    int unused;
};

#include "voxelbvh/voxelbvh_nanovdb_iterate.slang"

void iterate_main(pnanovdb_address_t node_arr, pnanovdb_address_t val_addr, uint level, uint n,
                  pnanovdb_coord_t ijk, inout user_state_t user_state)
{
    if (pnanovdb_address_is_null(val_addr))
    {
        return;
    }

    pnanovdb_uint64_t val = pnanovdb_read_uint64(buf_scratch, val_addr);

    pnanovdb_write_uint64(buf, val_addr, val);
}

[shader("compute")][numthreads(256, 1, 1)]
void main(uint3 group_idx : SV_GroupID, uint3 thread_idx : SV_GroupThreadID)
{
    user_state_t user_state = {};
    do_iteration(group_idx.x, thread_idx.x, user_state);
}
