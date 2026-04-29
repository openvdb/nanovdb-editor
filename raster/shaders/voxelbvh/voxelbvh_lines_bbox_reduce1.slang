// voxelbvh_lines_bbox_reduce1.slang

struct constants_t
{
    uint line_count;
    uint workgroup_count;
    uint voxel_count;
    uint voxel_workgroup_count;
    uint integer_space_max;
};

ConstantBuffer<constants_t> constants;

StructuredBuffer<uint2> indices_in;
StructuredBuffer<float> positions_in;

RWStructuredBuffer<float> bbox_out;

groupshared float3 s_aabb_min[256u];
groupshared float3 s_aabb_max[256u];

[shader("compute")][numthreads(256, 1, 1)]
void main(uint3 group_idx : SV_GroupID, uint3 thread_idx : SV_GroupThreadID)
{
    uint idx = group_idx.x * 256u + thread_idx.x;

    float3 aabb_min = float3(1.f / 0.f, 1.f / 0.f, 1.f / 0.f);
    float3 aabb_max = float3(-1.f / 0.f, -1.f / 0.f, -1.f / 0.f);
    if (idx < constants.line_count)
    {
        uint2 index = indices_in[idx];

        float3 pos_a = float3(
            positions_in[3u * index.x + 0u],
            positions_in[3u * index.x + 1u],
            positions_in[3u * index.x + 2u]);
        float3 pos_b = float3(
            positions_in[3u * index.y + 0u],
            positions_in[3u * index.y + 1u],
            positions_in[3u * index.y + 2u]);

        aabb_min = min(aabb_min, pos_a);
        aabb_max = max(aabb_max, pos_a);
        aabb_min = min(aabb_min, pos_b);
        aabb_max = max(aabb_max, pos_b);
    }

    // reduce within workgroup
    s_aabb_min[thread_idx.x] = aabb_min;
    s_aabb_max[thread_idx.x] = aabb_max;

    GroupMemoryBarrierWithGroupSync();

    if (thread_idx.x < 64u)
    {
        aabb_min = min(aabb_min, s_aabb_min[thread_idx.x + 64u]);
        aabb_min = min(aabb_min, s_aabb_min[thread_idx.x + 128u]);
        aabb_min = min(aabb_min, s_aabb_min[thread_idx.x + 192u]);
        aabb_max = max(aabb_max, s_aabb_max[thread_idx.x + 64u]);
        aabb_max = max(aabb_max, s_aabb_max[thread_idx.x + 128u]);
        aabb_max = max(aabb_max, s_aabb_max[thread_idx.x + 192u]);
        s_aabb_min[thread_idx.x] = aabb_min;
        s_aabb_max[thread_idx.x] = aabb_max;
    }

    GroupMemoryBarrierWithGroupSync();

    if (thread_idx.x < 16u)
    {
        aabb_min = min(aabb_min, s_aabb_min[thread_idx.x + 16u]);
        aabb_min = min(aabb_min, s_aabb_min[thread_idx.x + 32u]);
        aabb_min = min(aabb_min, s_aabb_min[thread_idx.x + 48u]);
        aabb_max = max(aabb_max, s_aabb_max[thread_idx.x + 16u]);
        aabb_max = max(aabb_max, s_aabb_max[thread_idx.x + 32u]);
        aabb_max = max(aabb_max, s_aabb_max[thread_idx.x + 48u]);
        s_aabb_min[thread_idx.x] = aabb_min;
        s_aabb_max[thread_idx.x] = aabb_max;
    }

    GroupMemoryBarrierWithGroupSync();

    if (thread_idx.x < 4u)
    {
        aabb_min = min(aabb_min, s_aabb_min[thread_idx.x + 4u]);
        aabb_min = min(aabb_min, s_aabb_min[thread_idx.x + 8u]);
        aabb_min = min(aabb_min, s_aabb_min[thread_idx.x + 12u]);
        aabb_max = max(aabb_max, s_aabb_max[thread_idx.x + 4u]);
        aabb_max = max(aabb_max, s_aabb_max[thread_idx.x + 8u]);
        aabb_max = max(aabb_max, s_aabb_max[thread_idx.x + 12u]);
        s_aabb_min[thread_idx.x] = aabb_min;
        s_aabb_max[thread_idx.x] = aabb_max;
    }

    GroupMemoryBarrierWithGroupSync();

    if (thread_idx.x < 1u)
    {
        aabb_min = min(aabb_min, s_aabb_min[thread_idx.x + 1u]);
        aabb_min = min(aabb_min, s_aabb_min[thread_idx.x + 2u]);
        aabb_min = min(aabb_min, s_aabb_min[thread_idx.x + 3u]);
        aabb_max = max(aabb_max, s_aabb_max[thread_idx.x + 1u]);
        aabb_max = max(aabb_max, s_aabb_max[thread_idx.x + 2u]);
        aabb_max = max(aabb_max, s_aabb_max[thread_idx.x + 3u]);

        bbox_out[6u * group_idx.x + 0u] = aabb_min.x;
        bbox_out[6u * group_idx.x + 1u] = aabb_min.y;
        bbox_out[6u * group_idx.x + 2u] = aabb_min.z;
        bbox_out[6u * group_idx.x + 3u] = aabb_max.x;
        bbox_out[6u * group_idx.x + 4u] = aabb_max.y;
        bbox_out[6u * group_idx.x + 5u] = aabb_max.z;
    }
}
