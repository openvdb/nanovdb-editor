// voxelbvh_nanovdb_level_list_alloc2.slang

struct user_state_t
{
    int unused;
};

#include "voxelbvh_nanovdb_iterate.slang"

RWStructuredBuffer<uint2> workgroup_counters_inout;

#include <workgroup_scan_uint64.slang>

void iterate_main(pnanovdb_address_t node_arr, pnanovdb_address_t val_addr, uint level, uint n,
                  pnanovdb_coord_t ijk, inout user_state_t user_state)
{
    // not used
}

[shader("compute")][numthreads(256, 1, 1)]
void main(uint3 group_idx : SV_GroupID, uint3 thread_idx : SV_GroupThreadID)
{
    if (group_idx.x == 0)
    {
        uint64_t total_element_count = uint2_to_uint64_t(workgroup_counters_inout[thread_idx.x]);

        // scan all the memory requests for this workgroup
        uint64_t4 total_element_count4 = uint64_t4(total_element_count, 0llu, 0llu, 0llu);
        uint64_t4 scan_total_element_count4 = uint64_t4(0llu, 0llu, 0llu, 0llu);
        uint64_t workgroup_total_element_count = 0llu;
        workgroup_scan_uint64(thread_idx.x, total_element_count4, scan_total_element_count4, workgroup_total_element_count);

        workgroup_counters_inout[256u + thread_idx.x] = uint64_t_to_uint2(scan_total_element_count4.x);
    }
}
