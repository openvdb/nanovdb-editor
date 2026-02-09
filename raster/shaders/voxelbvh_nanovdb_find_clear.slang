// voxelbvh_nanovdb_find_root.slang
#define PNANOVDB_HLSL
#define PNANOVDB_ADDRESS_64
#define PNANOVDB_BUF_HLSL_64
// #define PNANOVDB_BUF_HLSL_RW
#include "PNanoVDB.h"

struct constants_t
{
    uint nanovdb_word_count;
    uint pad0;
    uint nanovdb_chunk_count;
    uint node_mask_uint64_count;
};

ConstantBuffer<constants_t> constants;
StructuredBuffer<uint2> buf;

RWStructuredBuffer<uint2> node_mask_inout;

[shader("compute")][numthreads(256, 1, 1)]
void main(uint3 group_idx : SV_GroupID, uint3 thread_idx : SV_GroupThreadID)
{
    uint tidx = group_idx.x * 256u + thread_idx.x;

    for (uint idx = tidx; idx < constants.node_mask_uint64_count; idx += 65536u)
    {
        node_mask_inout[idx] = uint2(0u, 0u);
    }
}
