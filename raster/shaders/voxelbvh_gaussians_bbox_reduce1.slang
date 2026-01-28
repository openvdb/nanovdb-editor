// voxelbvh_gaussians_bbox_reduce1.slang

struct constants_t
{
    uint point_count;
    uint workgroup_count;
    uint voxel_count;
    uint voxel_workgroup_count;
};

ConstantBuffer<constants_t> constants;

StructuredBuffer<float> means_in;
StructuredBuffer<float> opacities_in;
StructuredBuffer<float> quats_in;
StructuredBuffer<float> scales_in;

RWStructuredBuffer<float> bbox_out;

groupshared float3 s_aabb_min[256u];
groupshared float3 s_aabb_max[256u];

[shader("compute")][numthreads(256, 1, 1)]
void main(uint3 group_idx : SV_GroupID, uint3 thread_idx : SV_GroupThreadID)
{
    uint idx = group_idx.x * 256u + thread_idx.x;

    float3 aabb_min = float3(1.f / 0.f, 1.f / 0.f, 1.f / 0.f);
    float3 aabb_max = float3(-1.f / 0.f, -1.f / 0.f, -1.f / 0.f);
    if (idx < constants.point_count)
    {
        float3 meansf = float3(
            means_in[3u * idx + 0u],
            means_in[3u * idx + 1u],
            means_in[3u * idx + 2u]
        );
        float4 quatf = float4(
            quats_in[4u * idx + 1u],
            quats_in[4u * idx + 2u],
            quats_in[4u * idx + 3u],
            quats_in[4u * idx + 0u]
        );
        float3 scalef = float3(
            scales_in[3u * idx + 0u],
            scales_in[3u * idx + 1u],
            scales_in[3u * idx + 2u]
        );
        float opacity = opacities_in[idx];

        float local_scale = sqrt(2.f * log(max(0.01f, opacity) / 0.01f));

        for (uint vert_idx = 0u; vert_idx < 8u; vert_idx++)
        {
            float3 cube_pos = float3(
                (vert_idx & 1u) == 0 ? -1.f : 1.f,
                (vert_idx & 2u) == 0 ? -1.f : 1.f,
                (vert_idx & 4u) == 0 ? -1.f : 1.f
            );
            cube_pos *= scalef * local_scale;

            float3 rot0 = float3(
                1.f - 2.f * (quatf.y * quatf.y + quatf.z * quatf.z),
                2.f * (quatf.x * quatf.y - quatf.z * quatf.w),
                2.f * (quatf.x * quatf.z + quatf.y * quatf.w));
            float3 rot1 = float3(
                2.f * (quatf.x * quatf.y + quatf.z * quatf.w),
                1.f - 2.f * (quatf.x * quatf.x + quatf.z * quatf.z),
                2.f * (quatf.y * quatf.z - quatf.x * quatf.w));
            float3 rot2 = float3(
                2.f * (quatf.x * quatf.z - quatf.y * quatf.w),
                2.f * (quatf.y * quatf.z + quatf.x * quatf.w),
                1.f - 2.f * (quatf.x * quatf.x + quatf.y * quatf.y));

            float3 cube_pos_world;
            cube_pos_world.x = cube_pos.x * rot0.x + cube_pos.y * rot0.y + cube_pos.z * rot0.z + meansf.x;
            cube_pos_world.y = cube_pos.x * rot1.x + cube_pos.y * rot1.y + cube_pos.z * rot1.z + meansf.y;
            cube_pos_world.z = cube_pos.x * rot2.x + cube_pos.y * rot2.y + cube_pos.z * rot2.z + meansf.z;

            if (vert_idx == 0u)
            {
                aabb_min = cube_pos_world;
                aabb_max = cube_pos_world;
            }
            else
            {
                aabb_min = min(aabb_min, cube_pos_world);
                aabb_max = max(aabb_max, cube_pos_world);
            }
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
