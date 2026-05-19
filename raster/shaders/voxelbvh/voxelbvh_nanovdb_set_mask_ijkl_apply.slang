// voxelbvh_nanovdb_set_mask_ijkl.slang
#define PNANOVDB_HLSL
#define PNANOVDB_ADDRESS_64
#define PNANOVDB_BUF_HLSL_64
#define PNANOVDB_BUF_HLSL_RW
#include "PNanoVDB.h"

struct constants_t
{
    uint nanovdb_word_count;
    uint ijkl_count;
    uint nanovdb_chunk_count;
    uint node_mask_uint64_count;
    uint range_count;
};

ConstantBuffer<constants_t> constants;

StructuredBuffer<uint2> ijkl_in;
StructuredBuffer<uint2> range_in;

RWStructuredBuffer<uint2> buf;
RWStructuredBuffer<uint2> range_scratch;

void nanovdb_set_bit(pnanovdb_address_t addr, uint bit_idx)
{
    uint64_t word_idx = (addr.byte_offset >> 3u) + (bit_idx >> 6u);
    uint64_t val = uint64_t(1u) << (bit_idx & 63u);
    uint2 val2 = uint2(uint(val), uint(val >> 32u));
    if (val2.x != 0u) { InterlockedOr(buf[word_idx].x, val2.x); };
    if (val2.y != 0u) { InterlockedOr(buf[word_idx].y, val2.y); };
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
    uint idx = group_idx.x * 256u + thread_idx.x;

    if (idx >= constants.range_count)
    {
        return;
    }
    uint2 range = range_in[idx];
    if (range.x >= range.y || range.x >= constants.ijkl_count)
    {
        return;
    }
    uint2 ijkl_raw = ijkl_in[range.x];
    if (ijkl_raw.x == ~0u && ijkl_raw.y == ~0u)
    {
        return;
    }
    int4 ijkl = int4(
        int(ijkl_raw.y) >> 16,
        int(ijkl_raw.y << 16) >> 16,
        int(ijkl_raw.x) >> 16,
        int(ijkl_raw.x << 16) >> 16
    );
    if (ijkl.w == 65535)
    {
        return;
    }
    // squash lsbs to zero for higher levels
    uint lmask = ~((1u << ijkl.w) - 1u);
    ijkl.xyz = ijkl.xyz & lmask;

    uint64_t range_scratch_val = uint2_to_uint64_t(range_scratch[idx]);
    if (range_scratch_val.x != 0u)
    {
        pnanovdb_address_t addr = {range_scratch_val & ((uint64_t(1u) << 48u) - 1u)};
        uint n = (range_scratch_val >> 48u);
        nanovdb_set_bit(addr, n);
    }
}
