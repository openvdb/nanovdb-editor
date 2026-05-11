// voxelbvh_nanovdb_clear.slang
#define PNANOVDB_HLSL
#define PNANOVDB_ADDRESS_64
#define PNANOVDB_BUF_HLSL_64
#define PNANOVDB_BUF_HLSL_RW
#include "PNanoVDB.h"

struct constants_t
{
    uint64_t nanovdb_uint64_count;
};

ConstantBuffer<constants_t> constants;

RWStructuredBuffer<uint2> buf;

[shader("compute")][numthreads(256, 1, 1)]
void main(uint3 group_idx : SV_GroupID, uint3 thread_idx : SV_GroupThreadID)
{
    for (uint64_t idx = group_idx.x * 256u + thread_idx.x; idx < constants.nanovdb_uint64_count; idx += 65536u)
    {
        buf[idx] = uint2(0u, 0u);
    }
}
