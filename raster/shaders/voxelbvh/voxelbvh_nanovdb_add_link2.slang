// voxelbvh_nanovdb_add_link.slang
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
StructuredBuffer<uint2> workgroup_counters_in;
StructuredBuffer<uint2> total_new_bytes_inout;
RWStructuredBuffer<uint2> allocation_offset_inout;

uint64_t uint2_to_uint64_t(uint2 v)
{
    return (uint64_t(v.y) << 32u) | uint64_t(v.x);
}

uint2 uint64_t_to_uint2(uint64_t v)
{
    return uint2(uint(v), uint(v >> 32u));
}

pnanovdb_coord_t key_to_coord(pnanovdb_uint64_t key)
{
    pnanovdb_coord_t ijk = { pnanovdb_uint32_as_int32(pnanovdb_uint64_to_uint32_lsr(key, 42u) << 12u),
                             pnanovdb_uint32_as_int32(pnanovdb_uint64_to_uint32_lsr(key, 21u) << 12u),
                             pnanovdb_uint32_as_int32(pnanovdb_uint64_to_uint32_lsr(key, 0u) << 12u) };
    return ijk;
}

groupshared uint s_types[256u];

#include <workgroup_scan_uint64.slang>
#include <workgroup_scan.slang>

[shader("compute")][numthreads(256, 1, 1)]
void main(uint3 group_idx : SV_GroupID, uint3 thread_idx : SV_GroupThreadID)
{
    uint64_t total_new_bytes = uint2_to_uint64_t(total_new_bytes_inout[group_idx.x * 256u + thread_idx.x]);

    // scan all the memory requests for this workgroup
    vector<uint64_t, 4> total_new_bytes4 = vector<uint64_t, 4>(total_new_bytes, 0llu, 0llu, 0llu);
    vector<uint64_t, 4> scan_total_new_bytes4 = vector<uint64_t, 4>(0llu, 0llu, 0llu, 0llu);
    uint64_t workgroup_total_new_bytes = 0llu;
    workgroup_scan_uint64(thread_idx.x, total_new_bytes4, scan_total_new_bytes4, workgroup_total_new_bytes);

    uint64_t global_total_new_bytes = uint2_to_uint64_t(workgroup_counters_in[group_idx.x]);
    uint64_t global_scan_total_new_bytes = uint2_to_uint64_t(workgroup_counters_in[512u + group_idx.x]);
    uint64_t global_begin_offset = global_scan_total_new_bytes - global_total_new_bytes;

    uint64_t allocation_offset = global_begin_offset + scan_total_new_bytes4.x - total_new_bytes4.x;

    // place allocation after existing
    allocation_offset += uint2_to_uint64_t(workgroup_counters_in[768u]);

    allocation_offset_inout[group_idx.x * 256u + thread_idx.x] = uint64_t_to_uint2(allocation_offset);
}
