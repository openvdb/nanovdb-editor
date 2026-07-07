// voxelbvh_nanovdb_index_allocate2.slang

struct user_state_t
{
    int unused;
}

#include "voxelbvh_nanovdb_iterate.slang"
#include "voxelbvh_common.h"
#include <workgroup_scan_uint64.slang>

RWStructuredBuffer<uint2> workgroup_counters_inout;

void iterate_main(pnanovdb_address_t node_arr, pnanovdb_address_t val_addr, uint level, uint n,
                  pnanovdb_coord_t ijk, inout user_state_t user_state)
{
    // unused
}

uint64_t uint2_to_uint64_t(uint2 v)
{
    return (uint64_t(v.y) << 32u) | uint64_t(v.x);
}

uint2 uint64_t_to_uint2(uint64_t v)
{
    return uint2(uint(v), uint(v >> 32u));
}

[shader("compute")][numthreads(256, 1, 1)]
void main(uint3 group_idx : SV_GroupID, uint3 thread_idx : SV_GroupThreadID)
{
    if (group_idx.x == 0)
    {
        uint64_t total_count = uint2_to_uint64_t(workgroup_counters_inout[thread_idx.x]);

        // scan all the memory requests for this workgroup
        vector<uint64_t, 4> total_count4 = vector<uint64_t, 4>(total_count, 0llu, 0llu, 0llu);
        vector<uint64_t, 4> scan_total_count4 = vector<uint64_t, 4>(0llu, 0llu, 0llu, 0llu);
        uint64_t workgroup_total_count = 0llu;
        workgroup_scan_uint64(thread_idx.x, total_count4, scan_total_count4, workgroup_total_count);

        workgroup_counters_inout[256u + thread_idx.x] = uint64_t_to_uint2(scan_total_count4.x);
    }
}
