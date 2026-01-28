// voxelbvh_scatter_range_headers.slang

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
StructuredBuffer<uint> range_starts_in;
StructuredBuffer<uint> range_scan_in;

RWStructuredBuffer<uint> range_headers_out;

[shader("compute")][numthreads(256, 1, 1)]
void main(uint3 group_idx : SV_GroupID, uint3 thread_idx : SV_GroupThreadID)
{
    uint idx = group_idx.x * 256u + thread_idx.x;

    if (idx >= constants.point_count)
    {
        return;
    }

    uint is_start = range_starts_in[idx];
    if (is_start != 0u)
    {
        uint alloc_idx = range_scan_in[idx] - 1u;

        range_headers_out[2u * alloc_idx + 0u] = idx;
    }

    uint2 key = uint2(keys_low_in[idx], keys_high_in[idx]);
    uint2 key_cmp = key;
    if (idx < constants.voxel_count)
    {
        key_cmp = uint2(keys_low_in[idx + 1u], keys_high_in[idx + 1u]);
    }

    uint is_last = 0u;
    if (key.x != key_cmp.x || key.y != key_cmp.y)
    {
        is_last = 1u;
    }
    if (is_last != 0u)
    {
        uint alloc_idx = range_scan_in[idx] - 1u;

        range_headers_out[2u * alloc_idx + 1u] = idx;
    }
}
