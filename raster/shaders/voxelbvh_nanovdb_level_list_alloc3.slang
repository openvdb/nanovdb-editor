// voxelbvh_nanovdb_level_list_alloc3.slang

struct user_state_t
{
   uint64_t workgroup_alloc_idx;
};

#include "voxelbvh_nanovdb_iterate.slang"

StructuredBuffer<uint2> workgroup_counters_inout;

#include <workgroup_scan_uint64.slang>

void iterate_main(pnanovdb_address_t node_arr, pnanovdb_address_t val_addr, uint level, uint n,
                  pnanovdb_coord_t ijk, inout user_state_t user_state)
{
    uint thread_idx = n & 255;

    uint alloc_count = 0u;
    pnanovdb_uint64_t val = uint64_t(0u);
    if (!pnanovdb_address_is_null(val_addr))
    {
        val = pnanovdb_read_uint64(buf, val_addr);
        uint level_mask = uint(val) & 0xFFFF;
        alloc_count = countbits(level_mask);
        // only allocate background once
        if (level == 4u && n != 0u)
        {
            alloc_count = 0u;
        }
    }

    uint64_t4 alloc_count4 = uint64_t4(alloc_count, 0llu, 0llu, 0llu);
    uint64_t4 scan_alloc_count4 = uint64_t4(0llu, 0llu, 0llu, 0llu);
    uint64_t workgroup_alloc_count = 0llu;
    workgroup_scan_uint64(thread_idx, alloc_count4, scan_alloc_count4, workgroup_alloc_count);

    if (!pnanovdb_address_is_null(val_addr))
    {
        uint64_t val_alloc_idx = user_state.workgroup_alloc_idx + scan_alloc_count4.x - alloc_count4.x;
        // voxel value is only 32-bit for now
        uint val_alloc_idx32 = uint(val_alloc_idx);
        val = val | (uint64_t(val_alloc_idx32) << 32u);
        pnanovdb_write_uint64(buf, val_addr, val);
    }

    // update state for next iterate_main call on this worker
    user_state.workgroup_alloc_idx = user_state.workgroup_alloc_idx + workgroup_alloc_count;
}

[shader("compute")][numthreads(256, 1, 1)]
void main(uint3 group_idx : SV_GroupID, uint3 thread_idx : SV_GroupThreadID)
{
    uint64_t workgroup_element_count = uint2_to_uint64_t(workgroup_counters_inout[group_idx.x]);
    uint64_t workgroup_scan_element_count = uint2_to_uint64_t(workgroup_counters_inout[256u + group_idx.x]);
    uint64_t workgroup_begin_idx = workgroup_scan_element_count - workgroup_element_count;

    user_state_t user_state = {};
    user_state.workgroup_alloc_idx = workgroup_begin_idx;
    do_iteration(group_idx.x, thread_idx.x, user_state);
}
