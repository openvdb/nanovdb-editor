// voxelbvh_find_range_starts.slang

struct constants_t
{
    uint point_count;
    uint workgroup_count;
    uint voxel_count;
    uint voxel_workgroup_count;
};

ConstantBuffer<constants_t> constants;

StructuredBuffer<uint64_t> keys_in;

RWStructuredBuffer<uint> range_starts_out;
RWStructuredBuffer<uint> range_headers_out;

[shader("compute")][numthreads(256, 1, 1)]
void main(uint3 group_idx : SV_GroupID, uint3 thread_idx : SV_GroupThreadID)
{
    uint idx = group_idx.x * 256u + thread_idx.x;

    if (idx >= constants.voxel_count)
    {
        return;
    }

    uint64_t key = keys_in[idx];
    uint64_t key_cmp = ~key;
    if (idx > 0u)
    {
        key_cmp = keys_in[idx - 1u];
    }

    uint is_first = 0u;
    if (key != key_cmp)
    {
        is_first = 1u;
    }
    range_starts_out[idx] = is_first;

    range_headers_out[2u * idx + 0u] = 0u;
    range_headers_out[2u * idx + 1u] = 0u;
}
