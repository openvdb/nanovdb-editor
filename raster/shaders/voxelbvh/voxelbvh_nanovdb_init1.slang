// voxelbvh_nanovdb_init.slang

#define PNANOVDB_HLSL
#define PNANOVDB_ADDRESS_64
#define PNANOVDB_BUF_HLSL_64
#define PNANOVDB_BUF_HLSL_RW
#include "PNanoVDB.h"

struct constants_t
{
    uint grid_size;
    uint resolution;
    uint pad0;
    uint pad1;
    float4x4 transform;
    float4x4 transform_inv;
};
ConstantBuffer<constants_t> constants;

StructuredBuffer<uint2> upload;

RWStructuredBuffer<uint2> buf;

StructuredBuffer<float> world_bbox_in;

[shader("compute")][numthreads(256, 1, 1)]
void main(uint3 group_idx : SV_GroupID, uint3 thread_idx : SV_GroupThreadID)
{
    uint idx = group_idx.x * 256u + thread_idx.x;

    if (8u * idx < constants.grid_size)
    {
        buf[idx] = upload[idx];
    }
}
