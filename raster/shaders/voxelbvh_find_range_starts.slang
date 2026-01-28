// voxelbvh_find_range_starts.slang

struct constants_t
{
    uint point_count;
    uint workgroup_count;
    uint voxel_count;
    uint voxel_workgroup_count;
};

ConstantBuffer<constants_t> constants;

StructuredBuffer<uint> keys_low_in;
StructuredBuffer<uint> keys_high_in;

RWStructuredBuffer<uint> range_starts_out;
RWStructuredBuffer<uint> range_headers_out;

[shader("compute")][numthreads(256, 1, 1)]
void main(uint3 group_idx : SV_GroupID, uint3 thread_idx : SV_GroupThreadID)
{
    uint idx = group_idx.x * 256u + thread_idx.x;

    if (idx >= constants.point_count)
    {
        return;
    }

    uint2 key = uint2(keys_low_in[idx], keys_high_in[idx]);
    uint2 key_cmp = key;
    if (idx > 0u)
    {
        key_cmp = uint2(keys_low_in[idx - 1u], keys_high_in[idx - 1u]);
    }

    uint is_first = 0u;
    if (key.x != key_cmp.x || key.y != key_cmp.y)
    {
        is_first = 1u;
    }
    range_starts_out[idx] = is_first;

    range_headers_out[2u * idx + 0u] = 0u;
    range_headers_out[2u * idx + 1u] = 0u;
}
