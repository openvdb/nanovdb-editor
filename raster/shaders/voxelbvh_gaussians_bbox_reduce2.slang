// voxelbvh_gaussians_bbox_reduce2.slang

struct constants_t
{
    uint point_count;
    uint workgroup_count;
    uint voxel_count;
    uint voxel_workgroup_count;
};

ConstantBuffer<constants_t> constants;

StructuredBuffer<float> bbox_in;

RWStructuredBuffer<float> bbox_out;

groupshared float3 s_aabb_min[256u];
groupshared float3 s_aabb_max[256u];

[shader("compute")][numthreads(256, 1, 1)]
void main(uint3 group_idx : SV_GroupID, uint3 thread_idx : SV_GroupThreadID)
{
    if (group_idx.x != 0u)
    {
        return;
    }

    uint group_count = (constants.point_count + 255u) / 256u;
    uint pass_count = (group_count + 255u) / 256u;

    float3 aabb_min = float3(1.f / 0.f, 1.f / 0.f, 1.f / 0.f);
    float3 aabb_max = float3(-1.f / 0.f, -1.f / 0.f, -1.f / 0.f);
    for (uint pass_id = 0u; pass_id < pass_count; pass_id++)
    {
        uint bbox_idx = 256u * pass_id + thread_idx.x;
        if (bbox_idx < constants.workgroup_count)
        {
            float3 local_aabb_min = float3(
                bbox_in[6u * bbox_idx + 0u],
                bbox_in[6u * bbox_idx + 1u],
                bbox_in[6u * bbox_idx + 2u]);
            float3 local_aabb_max = float3(
                bbox_in[6u * bbox_idx + 3u],
                bbox_in[6u * bbox_idx + 4u],
                bbox_in[6u * bbox_idx + 5u]);

            aabb_min = min(aabb_min, local_aabb_min);
            aabb_max = min(aabb_max, local_aabb_max);
        }
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
