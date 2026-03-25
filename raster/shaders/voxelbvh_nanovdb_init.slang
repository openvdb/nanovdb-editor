// voxelbvh_nanovdb_init.slang

StructuredBuffer<uint2> upload;

RWStructuredBuffer<uint2> buf;

[shader("compute")][numthreads(256, 1, 1)]
void main(uint3 group_idx : SV_GroupID, uint3 thread_idx : SV_GroupThreadID)
{
    uint idx = group_idx.x * 256u + thread_idx.x;

    // grid size in NanoVDB is at 64-bit word index 4
    uint2 grid_size = upload[4u];

    if (8u * idx < grid_size.x)
    {
        buf[idx] = upload[idx];
    }
}
