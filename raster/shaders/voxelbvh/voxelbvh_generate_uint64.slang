// voxelbvh_generate_uint64.slang

struct constants_t
{
    uint64_t element_count;
};

ConstantBuffer<constants_t> constants;

RWStructuredBuffer<uint64_t> val_out;

[shader("compute")][numthreads(256, 1, 1)]
void main(uint3 group_idx : SV_GroupID, uint3 thread_idx : SV_GroupThreadID)
{
    uint64_t idx = group_idx.x * 256u + thread_idx.x;
    for (;idx < constants.element_count; idx += 65536u)
    {
        val_out[idx] = idx;
    }
}
