// voxelbvh_nanovdb_rgba8_from_voxelbvh.slang

struct user_state_t
{
    int unused;
}

#define PNANOVDB_ITERATE_WORKGROUP_THREADS 128
#define PNANOVDB_ITERATE_WORKGROUP_COUNT 256
#define WORKGROUP_VOXELBVH_THREADS 128

#include "voxelbvh_nanovdb_iterate.slang"
#include "hdda.slang"
#include "workgroup_voxelbvh.slang"
#include "voxelbvh_common.h"

struct constants2_t
{
    uint pass_id;
};

ConstantBuffer<constants2_t> constants2;

// based on: https://iquilezles.org/articles/intersectors/
float3 intersect_triangle( in float3 ro, in float3 rd, in float3 v0, in float3 v1, in float3 v2 )
{
    float3 v1v0 = v1 - v0;
    float3 v2v0 = v2 - v0;
    float3 rov0 = ro - v0;
    float3  n = cross_product( v1v0, v2v0 );
    float3  q = cross_product( rov0, rd );
    float d = 1.0f/dot( rd, n );
    float u = d*dot( -q, v2v0 );
    float v = d*dot(  q, v1v0 );
    float t = d*dot( -n, rov0 );
    if( u<0.0f || v<0.0f || (u+v)>1.0f ) t = -1.0f;
    return float3( t, u, v );
}

void intersect_triangle(
    pnanovdb_grid_handle_t grid,
    uint id_a,
    uint id_b,
    uint id_c,
    float3 rayOrigin,
    float tmin,
    float3 rayDir,
    float tmax,
    float3 rayDirInv,
    inout float4 sum,
    inout float nominalT)
{
    pnanovdb_address_t pos_addr = pnanovdb_grid_get_gridblindmetadata_value_address(buf_scratch, grid, 3u);
    float3 pos_a = pnanovdb_read_vec3(buf_scratch, pnanovdb_address_offset_product(pos_addr, 12u, id_a));
    float3 pos_b = pnanovdb_read_vec3(buf_scratch, pnanovdb_address_offset_product(pos_addr, 12u, id_b));
    float3 pos_c = pnanovdb_read_vec3(buf_scratch, pnanovdb_address_offset_product(pos_addr, 12u, id_c));

    float3 t_isect = intersect_triangle(rayOrigin, rayDir, pos_a, pos_b, pos_c);

    float alpha = 0.f;
    float3 color = {};
    float t = 0.f;
    if (t_isect.x > 0.f)
    {
        pnanovdb_address_t color_addr = pnanovdb_grid_get_gridblindmetadata_value_address(buf_scratch, grid, 4u);
        float3 color_a = pnanovdb_read_vec3(buf_scratch, pnanovdb_address_offset_product(color_addr, 12u, id_a));
        float3 color_b = pnanovdb_read_vec3(buf_scratch, pnanovdb_address_offset_product(color_addr, 12u, id_b));
        float3 color_c = pnanovdb_read_vec3(buf_scratch, pnanovdb_address_offset_product(color_addr, 12u, id_c));

        const uint mode = 0u;

        alpha = 1.f;
        t = t_isect.x;
        if (mode == 1)
        {
            float3 n = normalize(cross_product( pos_b - pos_a, pos_c - pos_a));
            color.rgb = abs(dot(n, rayDir)).xxx;
            color.rgb *= 1.5f;
            color.rgb *= (1.f - t_isect.y - t_isect.z) * color_a + t_isect.y * color_b + t_isect.z * color_c;
        }
        else if (mode == 2)
        {
            color = float3(1.f - t_isect.y - t_isect.z, t_isect.y, t_isect.z);
        }
        else
        {
            color = (1.f - t_isect.y - t_isect.z) * color_a + t_isect.y * color_b + t_isect.z * color_c;
        }
    }
    if (t > 0.f && t >= tmin && t < tmax)
    {
        if (t < nominalT || nominalT == 0.f)
        {
            nominalT = t;
            sum.rgb = alpha * color;
            sum.a = 0.f;
        }
    }
}

groupshared uint64_t s_voxel_val[PNANOVDB_ITERATE_WORKGROUP_THREADS];

struct prim_t
{
    uint id_a;
    uint id_b;
    uint id_c;
};
groupshared prim_t s_prims[PNANOVDB_ITERATE_WORKGROUP_THREADS];

void workgroup_triangle_process(uint thread_idx,
                                bool this_thread_active,
                                pnanovdb_grid_handle_t grid,
                                float3 worldRayOrigin,
                                float3 worldRayDir,
                                float3 worldRayDirInv,
                                float worldTmin,
                                float worldTmax,
                                uint64_t voxel_val_raw,
                                inout float4 sum,
                                inout float nominalT)
{
    workgroup_voxelbvh_iterator_t iter = {};
    workgroup_voxelbvh_iterator_init(buf_scratch, iter, thread_idx, 32u, grid, voxel_val_raw);

    uint prim_id_index = ~0u;
    while (workgroup_voxelbvh_iterator_get(iter, thread_idx, prim_id_index))
    {
        if (prim_id_index != ~0u)
        {
            pnanovdb_address_t prim_id_addr = pnanovdb_grid_get_gridblindmetadata_value_address(buf_scratch, grid, 1u);
            uint prim_id = pnanovdb_read_uint32(buf_scratch, pnanovdb_address_offset_product(prim_id_addr, 4u, prim_id_index));

            pnanovdb_address_t index_addr = pnanovdb_grid_get_gridblindmetadata_value_address(buf_scratch, grid, 2u);
            pnanovdb_uint32_t id_a = pnanovdb_read_uint32(buf_scratch, pnanovdb_address_offset(index_addr, 12u * prim_id + 0u));
            pnanovdb_uint32_t id_b = pnanovdb_read_uint32(buf_scratch, pnanovdb_address_offset(index_addr, 12u * prim_id + 4u));
            pnanovdb_uint32_t id_c = pnanovdb_read_uint32(buf_scratch, pnanovdb_address_offset(index_addr, 12u * prim_id + 8u));

            s_prims[thread_idx].id_a = id_a;
            s_prims[thread_idx].id_b = id_b;
            s_prims[thread_idx].id_c = id_c;
        }

        GroupMemoryBarrierWithGroupSync();

        uint count = workgroup_voxelbvh_iterator_count_in_pass(iter);

        // only participate if this pixel actually wanted this data
        if (this_thread_active)
        {
            for (uint s_idx = 0u; s_idx < count; s_idx++)
            {
                intersect_triangle(
                    grid,
                    s_prims[s_idx].id_a,
                    s_prims[s_idx].id_b,
                    s_prims[s_idx].id_c,
                    worldRayOrigin,
                    worldTmin,
                    worldRayDir,
                    worldTmax,
                    worldRayDirInv,
                    sum,
                    nominalT
                );
            }
        }

        GroupMemoryBarrierWithGroupSync();

        workgroup_voxelbvh_iterator_step(iter);
    }
}

groupshared int4 s_ijkw[PNANOVDB_ITERATE_WORKGROUP_THREADS];

groupshared float4 s_sum[PNANOVDB_ITERATE_WORKGROUP_THREADS];

void iterate_main(pnanovdb_address_t node_arr, pnanovdb_address_t val_addr, uint level, uint n,
                  pnanovdb_coord_t ijk, inout user_state_t user_state)
{
    uint thread_idx = n & (PNANOVDB_ITERATE_WORKGROUP_THREADS - 1u);

    // assuming single grid for now
    pnanovdb_grid_handle_t grid = {};

    uint64_t voxel_val_raw = 0llu;
    int4 ijkw = int4(ijk, 0);

    bool this_thread_active = !pnanovdb_address_is_null(val_addr);

    // Spreading load over 128 dispatches
    this_thread_active = this_thread_active && thread_idx == constants2.pass_id;

    if (this_thread_active)
    {
        float4 color = float4(0.f, 0.f, 0.f, 0.f);
        if (level == 3)
        {
            ijkw.w = 4096;
        }
        if (level == 2)
        {
            ijkw.w = 128;
        }
        if (level == 1)
        {
            ijkw.w = 8;
        }
        if (level == 0)
        {
            ijkw.w = 1;
        }

        float3 ijkf = 0.5f * float3(ijkw.xyz) + 0.5f * float3(ijkw.xyz + ijkw.w);
        float3 worldPos = pnanovdb_grid_index_to_worldf(buf, grid, ijkf);

        pnanovdb_grid_handle_t src_grid = {};
        float3 bvh_ijkf = pnanovdb_grid_world_to_indexf(buf_scratch, src_grid, worldPos);
        pnanovdb_tree_handle_t src_tree = pnanovdb_grid_get_tree(buf_scratch, src_grid);
        pnanovdb_root_handle_t src_root = pnanovdb_tree_get_root(buf_scratch, src_tree);
        pnanovdb_uint32_t src_grid_type = pnanovdb_grid_get_grid_type(buf_scratch, src_grid);
        int3 bvh_ijk_center = int3(floor(bvh_ijkf));
        pnanovdb_address_t src_addr = pnanovdb_root_get_value_address(src_grid_type, buf_scratch, src_root, bvh_ijk_center);
        voxel_val_raw = pnanovdb_read_uint64(buf_scratch, src_addr);
    }

    s_voxel_val[thread_idx] = voxel_val_raw;
    s_ijkw[thread_idx] = ijkw;

    GroupMemoryBarrierWithGroupSync();

    float4 sum = float4(0.f, 0.f, 0.f, 0.f);
    // Spreading load over 128 dispatches
    uint s_idx = constants2.pass_id;
    if (s_voxel_val[s_idx] != 0u)
    {
        ijkw = s_ijkw[s_idx];

        for (uint face_id = 0u; face_id < 6u; face_id++)
        {
            int3 ijk_min = ijkw.xyz;
            int3 ijk_max = ijkw.xyz + ijkw.w;

            float3 uvw = {};
            float3 worldRayDir = {};
            if (face_id == 0)
            {
                uvw = float3(
                    0.f,
                    (1.f / 7.f) * (thread_idx >> 4),
                    (1.f / 15.f) * (thread_idx & 15));
                worldRayDir = float3(1.f, 0.f, 0.f);
            }
            if (face_id == 1)
            {
                uvw = float3(
                    1.f,
                    (1.f / 7.f) * (thread_idx >> 4),
                    (1.f / 15.f) * (thread_idx & 15));
                worldRayDir = float3(-1.f, 0.f, 0.f);
            }
            if (face_id == 2)
            {
                uvw = float3(
                    (1.f / 7.f) * (thread_idx >> 4),
                    0.f,
                    (1.f / 15.f) * (thread_idx & 15));
                worldRayDir = float3(0.f, 1.f, 0.f);
            }
            if (face_id == 3)
            {
                uvw = float3(
                    (1.f / 7.f) * (thread_idx >> 4),
                    1.f,
                    (1.f / 15.f) * (thread_idx & 15));
                worldRayDir = float3(0.f, -1.f, 0.f);
            }
            if (face_id == 4)
            {
                uvw = float3(
                    (1.f / 7.f) * (thread_idx >> 4),
                    (1.f / 15.f) * (thread_idx & 15),
                    0.f);
                worldRayDir = float3(0.f, 0.f, 1.f);
            }
            if (face_id == 5)
            {
                uvw = float3(
                    (1.f / 7.f) * (thread_idx >> 4),
                    (1.f / 15.f) * (thread_idx & 15),
                    1.f);
                worldRayDir = float3(0.f, 0.f, -1.f);
            }
            float3 ijkf = (1.f - uvw) * float3(ijk_min) + uvw * float3(ijk_max);

            float3 worldRayOrigin = ijkf;
            float worldTmin = 0.f;
            float worldTmax = float(ijkw.w);

            worldRayOrigin = pnanovdb_grid_index_to_worldf(buf, grid, worldRayOrigin);
            worldRayDir = pnanovdb_grid_index_to_world_dirf(buf, grid, worldRayDir);
            float rayDirMagn = length(worldRayDir);
            if (rayDirMagn > 0.f)
            {
                worldRayDir /= rayDirMagn;
                worldTmin *= rayDirMagn;
                worldTmax *= rayDirMagn;
            }
            float3 worldRayDirInv = float3(1.f, 1.f, 1.f) / worldRayDir;

            float4 sum_local = float4(0.f, 0.f, 0.f, 1.f);
            float nominalT_local = 0.f;
            workgroup_triangle_process(thread_idx,
                                    true,
                                    grid,
                                    worldRayOrigin,
                                    worldRayDir,
                                    worldRayDirInv,
                                    worldTmin,
                                    worldTmax,
                                    s_voxel_val[s_idx],
                                    sum_local,
                                    nominalT_local);

            // flip to alpha before averaging
            sum_local.a = 1.f - sum_local.a;

            s_sum[thread_idx] = sum_local;

            GroupMemoryBarrierWithGroupSync();
            if (thread_idx < 64u)
            {
                sum_local += s_sum[thread_idx + 64u];
                //sum_local += s_sum[thread_idx + 128u];
                //sum_local += s_sum[thread_idx + 192u];
                sum_local *= 0.5f;
            }
            GroupMemoryBarrierWithGroupSync();
            s_sum[thread_idx] = sum_local;

            GroupMemoryBarrierWithGroupSync();
            if (thread_idx < 16u)
            {
                sum_local += s_sum[thread_idx + 16u];
                sum_local += s_sum[thread_idx + 32u];
                sum_local += s_sum[thread_idx + 48u];
                sum_local *= 0.25f;
            }
            GroupMemoryBarrierWithGroupSync();
            s_sum[thread_idx] = sum_local;

            GroupMemoryBarrierWithGroupSync();
            if (thread_idx < 4u)
            {
                sum_local += s_sum[thread_idx + 4u];
                sum_local += s_sum[thread_idx + 8u];
                sum_local += s_sum[thread_idx + 12u];
                sum_local *= 0.25f;
            }
            GroupMemoryBarrierWithGroupSync();
            s_sum[thread_idx] = sum_local;

            GroupMemoryBarrierWithGroupSync();
            if (thread_idx == s_idx)
            {
                sum_local = 0.25f * (s_sum[0] + s_sum[1] + s_sum[2] + s_sum[3]);

                // normalize against misses
                if (sum_local.a != 0.f)
                {
                    sum_local.rgb *= (1.f / sum_local.a);
                }

                sum = max(sum, sum_local);
            }
            GroupMemoryBarrierWithGroupSync();
        }
    }

    if (this_thread_active)
    {
        float4 color = float4(sum.rgb, sum.a);
        uint rgba_raw = uint(255.f * max(0.f, min(1.f, color.x))) | (uint(255.f * max(0.f, min(1.f, color.y))) << 8u) |
                        (uint(255.f * max(0.f, min(1.f, color.z))) << 16u) |
                        (uint(255.f * max(0.f, min(1.f, color.w))) << 24u);
        pnanovdb_write_uint32(buf, val_addr, rgba_raw);
    }
}

[shader("compute")][numthreads(128, 1, 1)]
void main(uint3 group_idx : SV_GroupID, uint3 thread_idx : SV_GroupThreadID)
{
    user_state_t user_state = {};
    do_iteration(group_idx.x, thread_idx.x, user_state);
}
