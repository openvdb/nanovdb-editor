// voxelbvh_nanovdb_add_scan.slang
#define PNANOVDB_HLSL
#define PNANOVDB_ADDRESS_64
#define PNANOVDB_BUF_HLSL_64
#define PNANOVDB_BUF_HLSL_RW
#include "PNanoVDB.h"

struct constants_t
{
    uint nanovdb_word_count;
    uint pad0;
    uint nanovdb_chunk_count;
    uint node_mask_uint64_count;
};

ConstantBuffer<constants_t> constants;
RWStructuredBuffer<uint2> buf;
StructuredBuffer<uint2> node_mask_in;
RWStructuredBuffer<uint2> workgroup_counters_inout;

uint64_t uint2_to_uint64_t(uint2 v)
{
    return (uint64_t(v.y) << 32u) | uint64_t(v.x);
}

uint2 uint64_t_to_uint2(uint64_t v)
{
    return uint2(uint(v), uint(v >> 32u));
}

#include <workgroup_scan_uint64.slang>

[shader("compute")][numthreads(256, 1, 1)]
void main(uint3 group_idx : SV_GroupID, uint3 thread_idx : SV_GroupThreadID)
{
    if (group_idx.x == 0)
    {
        uint64_t total_new_bytes = uint2_to_uint64_t(workgroup_counters_inout[thread_idx.x]);

        // scan all the memory requests for this workgroup
        vector<uint64_t, 4> total_new_bytes4 = vector<uint64_t, 4>(total_new_bytes, 0llu, 0llu, 0llu);
        vector<uint64_t, 4> scan_total_new_bytes4 = vector<uint64_t, 4>(0llu, 0llu, 0llu, 0llu);
        uint64_t workgroup_total_new_bytes = 0llu;
        workgroup_scan_uint64(thread_idx.x, total_new_bytes4, scan_total_new_bytes4, workgroup_total_new_bytes);

        workgroup_counters_inout[512u + thread_idx.x] = uint64_t_to_uint2(scan_total_new_bytes4.x);

        uint64_t max_addr = uint2_to_uint64_t(workgroup_counters_inout[256u + thread_idx.x]);

        // scan all the memory requests for this workgroup
        vector<uint64_t, 4> max_addr4 = vector<uint64_t, 4>(max_addr, 0llu, 0llu, 0llu);
        uint64_t workgroup_max_addr = 0llu;
        workgroup_uint64_reduce_max(thread_idx.x, max_addr4, workgroup_max_addr);

        workgroup_counters_inout[768u + thread_idx.x] = uint64_t_to_uint2(workgroup_max_addr);

        if (thread_idx.x == 0u)
        {
            uint64_t new_grid_size = workgroup_max_addr + workgroup_total_new_bytes;

            pnanovdb_grid_handle_t grid = { pnanovdb_address_null() };
            pnanovdb_grid_set_grid_size(buf, grid, new_grid_size);
        }
    }
}
