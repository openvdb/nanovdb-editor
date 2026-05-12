// voxelbvh_nanovdb_level_list_alloc1.slang

struct user_state_t
{
   uint64_t element_count;
};

#include "voxelbvh_nanovdb_iterate.slang"

RWStructuredBuffer<uint2> workgroup_counters_inout;

#include <workgroup_scan_uint64.slang>

void iterate_main(pnanovdb_address_t node_arr, pnanovdb_address_t val_addr, uint level, uint n,
                  pnanovdb_coord_t ijk, inout user_state_t user_state)
{
    uint alloc_count = 0u;
    if (!pnanovdb_address_is_null(val_addr))
    {
        pnanovdb_uint32_t val = pnanovdb_read_uint32(buf, val_addr);
        uint active_lmask = val & 0xFFFF;
        alloc_count = countbits(active_lmask);
    }
    user_state.element_count = user_state.element_count + alloc_count;
}

[shader("compute")][numthreads(256, 1, 1)]
void main(uint3 group_idx : SV_GroupID, uint3 thread_idx : SV_GroupThreadID)
{
    user_state_t user_state = {};
    do_iteration(group_idx.x, thread_idx.x, user_state);

    // reduce all the memory requests for this workgroup
    uint64_t4 total_element_count4 = uint64_t4(user_state.element_count, 0llu, 0llu, 0llu);
    uint64_t workgroup_total_element_count = 0llu;
    workgroup_reduce_uint64(thread_idx.x, total_element_count4, workgroup_total_element_count);
    if (thread_idx.x == 0)
    {
        workgroup_counters_inout[group_idx.x] = uint64_t_to_uint2(workgroup_total_element_count);
    }
}
