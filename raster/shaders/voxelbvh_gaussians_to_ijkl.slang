// voxelbvh_gaussians_to_ijkl.slang

struct constants_t
{
    uint point_count;
    uint workgroup_count;
    uint pad2;
    uint pad3;
};

ConstantBuffer<constants_t> constants;

StructuredBuffer<float> means_in;
StructuredBuffer<float> opacities_in;
StructuredBuffer<float> quats_in;
StructuredBuffer<float> scales_in;

StructuredBuffer<float> bbox_in;

RWStructuredBuffer<uint> keys_low_out;
RWStructuredBuffer<uint> keys_high_out;
RWStructuredBuffer<uint> bbox_ids_out;

bool voxel_ellipsoid_overlap(uint idx, float scale, float3 offset, int3 ijk, uint level)
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

    float3 voxel_min;
    float3 voxel_max;
    for (uint vert_idx = 0u; vert_idx < 8u; vert_idx++)
    {
        float3 voxel_pos = float3(ijk);
        if ((vert_idx & 1u) != 0u) { voxel_pos.x += float(1u << level);}
        if ((vert_idx & 2u) != 0u) { voxel_pos.y += float(1u << level);}
        if ((vert_idx & 4u) != 0u) { voxel_pos.z += float(1u << level);}

        float3 local_pos = (voxel_pos - offset) / scale;
        local_pos -= meansf;

        float3 rot0_inv = float3(
            1.f - 2.f * (quatf.y * quatf.y + quatf.z * quatf.z),
            2.f * (quatf.x * quatf.y + quatf.z * quatf[3]),
            2.f * (quatf.x * quatf.z - quatf.y * quatf[3]));
        float3 rot1_inv = float3(
            2.f * (quatf.x * quatf.y - quatf.z * quatf[3]),
            1.f - 2.f * (quatf.x * quatf.x + quatf.z * quatf.z),
            2.f * (quatf.y * quatf.z + quatf.x * quatf[3]));
        float3 rot2_inv = float3(
            2.f * (quatf.x * quatf.z + quatf.y * quatf[3]),
            2.f * (quatf.y * quatf.z - quatf.x * quatf[3]),
            1.f - 2.f * (quatf.x * quatf.x + quatf.y * quatf.y));

        float3 local_pos_rot;
        local_pos_rot.x = local_pos.x * rot0_inv.x + local_pos.y * rot0_inv.y + local_pos.z * rot0_inv.z;
        local_pos_rot.y = local_pos.x * rot1_inv.x + local_pos.y * rot1_inv.y + local_pos.z * rot1_inv.z;
        local_pos_rot.z = local_pos.x * rot2_inv.x + local_pos.y * rot2_inv.y + local_pos.z * rot2_inv.z;

        local_pos_rot *= (float3(1.f, 1.f, 1.f) / (local_scale * scalef));

        if (vert_idx == 0u)
        {
            voxel_min = local_pos_rot;
            voxel_max = local_pos_rot;
        }
        else
        {
            voxel_min = min(voxel_min, local_pos_rot);
            voxel_max = max(voxel_max, local_pos_rot);
        }
    }
    float dist2 = 1.f;
    if (0.f < voxel_min.x) { dist2 -= voxel_min.x * voxel_min.x;}
    else if (0.f > voxel_max.x) { dist2 -= voxel_max.x * voxel_max.x;}
    if (0.f < voxel_min.y) { dist2 -= voxel_min.y * voxel_min.y;}
    else if (0.f > voxel_max.y) { dist2 -= voxel_max.y * voxel_max.y;}
    if (0.f < voxel_min.z) { dist2 -= voxel_min.z * voxel_min.z;}
    else if (0.f > voxel_max.z) { dist2 -= voxel_max.z * voxel_max.z;}
    return dist2 > 0.f;
}

[shader("compute")][numthreads(256, 1, 1)]
void main(uint3 group_idx : SV_GroupID, uint3 thread_idx : SV_GroupThreadID)
{
    uint idx = group_idx.x * 256u + thread_idx.x;

    if (idx >= constants.point_count)
    {
        return;
    }

    float scale = 1.f;
    float3 offset = float3(0.f, 0.f, 0.f);

    const uint integer_space_max = 4095;
    float3 g_aabb_min = float3(bbox_in[0], bbox_in[1], bbox_in[2]);
    float3 g_aabb_max = float3(bbox_in[3], bbox_in[4], bbox_in[5]);
    float3 g_aabb_delta = g_aabb_max - g_aabb_min;
    float g_aabb_delta_max = max(g_aabb_delta.x, max(g_aabb_delta.y, g_aabb_delta.z));
    float scale = float(integer_space_max) / (g_aabb_delta_max);
    float offset[3] = {
        -world_bbox_min[0] * scale,
        -world_bbox_min[1] * scale,
        -world_bbox_min[2] * scale
    };

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

    float3 aabb_min;
    float3 aabb_max;
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

    // transform world space to voxel space
    int3 ijk_min = int3(floor(scale * aabb_min + offset));
    int3 ijk_max = int3(floor(scale * aabb_max + offset));

    uint write_subidx = 0u;
    bool hit_max = false;
    for (uint level = 15u; level < 16u; level--)
    {
        int3 ijk_min_level = ijk_min & ~((1 << level) - 1);
        int3 ijk_max_level = ijk_max & ~((1 << level) - 1);
        uint overlap_count = 0u;
        for (int i = ijk_min_level.x; i <= ijk_max_level.x; i += (1u << level))
        {
            for (int j = ijk_min_level.y; j <= ijk_max_level.y; j += (1u << level))
            {
                for (int k = ijk_min_level.z; k <= ijk_max_level.z; k += (1u << level))
                {
                    int3 ijk = int3(i,j,k);
                    bool is_overlap = voxel_ellipsoid_overlap(idx, ijk, level);
                    if (is_overlap)
                    {
                        overlap_count++;
                        if (hit_max && write_subidx < 8u)
                        {
                            uint64_t key = (uint64_t(level) << 48u) | (uint64_t(i) << 32u) |
                                (uint64_t(j) << 16u) | uint64_t(k);
                            keys_low_out[8u * idx + write_subidx] = uint(j) << 16u) | uint(k);
                            keys_high_out[8u * idx + write_subidx] = uint(level) << 16u) | uint(i);
                            bbox_ids_out[8u * idx + write_subidx] = idx;
                            write_subidx++;
                        }
                    }
                }
            }
        }
        if (!hit_max)
        {
            if (overlap_count > 8u)
            {
                level += 2u;
                hit_max = true;
            }
            continue;
        }
        break;
    }

    // set unused to last key and invalid id
    for (; write_subidx < 8u; write_subidx++)
    {
        keys_low_out[8u * idx + write_subidx] = ~0u;
        keys_high_out[8u * idx + write_subidx] = ~0u;
        bbox_ids_out[8u * idx + write_subidx] = ~0u;
    }
}
