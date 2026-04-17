// voxelbvh_scatter_range_headers.slang

struct constants_t
{
    uint point_count;
    uint workgroup_count;
    uint voxel_count;
    uint voxel_workgroup_count;
};

ConstantBuffer<constants_t> constants;

StructuredBuffer<uint64_t> keys_in;
StructuredBuffer<uint> range_starts_in;
StructuredBuffer<uint> range_scan_in;

RWStructuredBuffer<uint> range_headers_out;

[shader("compute")][numthreads(256, 1, 1)]
void main(uint3 group_idx : SV_GroupID, uint3 thread_idx : SV_GroupThreadID)
{
    uint idx = group_idx.x * 256u + thread_idx.x;

    if (idx >= constants.voxel_count)
    {
        return;
    }

    uint is_start = range_starts_in[idx];
    if (is_start != 0u)
    {
        uint alloc_idx = range_scan_in[idx] - 1u;

        range_headers_out[2u * alloc_idx + 0u] = idx;
    }

    uint64_t key = keys_in[idx];
    uint64_t key_cmp = ~key;
    if (idx + 1u < constants.voxel_count)
    {
        key_cmp = keys_in[idx + 1u];
    }

    uint is_last = 0u;
    if (key != key_cmp)
    {
        is_last = 1u;
    }
    if (is_last != 0u)
    {
        uint alloc_idx = range_scan_in[idx] - 1u;

        range_headers_out[2u * alloc_idx + 1u] = idx + 1u;
    }
}
