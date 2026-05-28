// voxelbvh_nanovdb_rgba8_from_voxelbvh.slang

struct user_state_t
{
    int unused;
}

#include "voxelbvh_nanovdb_iterate.slang"

float3 cross_product(float3 a, float3 b)
{
    return float3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}

groupshared uint64_t s_range[12u];

struct workgroup_voxelbvh_iterator_t
{
    uint active_level_count;
    uint total_count;
    uint pass_idx;
    uint pass_count;
};

void workgroup_voxelbvh_iterator_init(
    inout workgroup_voxelbvh_iterator_t iter,
    uint thread_idx,
    uint max_pass_count,
    pnanovdb_grid_handle_t grid,
    uint64_t voxel_val_raw)
{
    uint active_level_mask = uint(voxel_val_raw) & 0xFFFF;
    uint list_begin_idx = uint(voxel_val_raw >> 32u);

    iter.pass_idx = 0u;
    iter.active_level_count = countbits(active_level_mask);
    if (thread_idx < 12u)
    {
        pnanovdb_uint64_t range = {};
        if (thread_idx < iter.active_level_count)
        {
            pnanovdb_address_t range_addr = pnanovdb_grid_get_gridblindmetadata_value_address(buf, grid, 0u);
            range_addr = pnanovdb_address_offset_product(range_addr, 8u, list_begin_idx);
            range = pnanovdb_read_uint64(buf, pnanovdb_address_offset(range_addr, 8u * thread_idx));
        }
        s_range[thread_idx] = range;
    }
    GroupMemoryBarrierWithGroupSync();
    iter.total_count = 0u;
    for (uint active_level_idx = 0u; active_level_idx < iter.active_level_count; active_level_idx++)
    {
        pnanovdb_uint64_t range = s_range[active_level_idx];
        iter.total_count += uint(range >> 32u) - uint(range);
    }
    iter.pass_count = (iter.total_count + 255u) / 256u;
    if (iter.pass_count > max_pass_count)
    {
        iter.pass_count = max_pass_count;
    }
}

bool workgroup_voxelbvh_iterator_get(
    inout workgroup_voxelbvh_iterator_t iter,
    uint thread_idx,
    inout uint prim_id_index)
{
    if (iter.pass_idx >= iter.pass_count)
    {
        return false;
    }
    uint flat_prim_id_index = 256u * iter.pass_idx + thread_idx;
    uint flat_accum = 0u;
    prim_id_index = ~0u;
    for (uint active_level_idx = 0u; active_level_idx < iter.active_level_count; active_level_idx++)
    {
        uint range_begin = uint(s_range[active_level_idx]);
        uint range_end = uint(s_range[active_level_idx] >> 32u);
        uint prim_id_index_cmp = range_begin + flat_prim_id_index - flat_accum;
        if (prim_id_index_cmp < range_end)
        {
            prim_id_index = prim_id_index_cmp;
            break;
        }
        flat_accum += (range_end - range_begin);
    }
    return true;
}

void workgroup_voxelbvh_iterator_step(
    inout workgroup_voxelbvh_iterator_t iter)
{
    iter.pass_idx++;
}

void workgroup_voxelbvh_iterator_reset(
    inout workgroup_voxelbvh_iterator_t iter)
{
    iter.pass_idx = 0u;
}

uint workgroup_voxelbvh_iterator_count_in_pass(
    inout workgroup_voxelbvh_iterator_t iter)
{
    uint count = iter.total_count - 256u * iter.pass_idx;
    if (count > 256u)
    {
        count = 256u;
    }
    return count;
}

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
    float3 pos_a,
    uint id_a,
    float3 pos_b,
    uint id_b,
    float3 pos_c,
    uint id_c,
    float3 rayOrigin,
    float tmin,
    float3 rayDir,
    float tmax,
    float3 rayDirInv,
    inout float4 sum,
    inout float nominalT)
{
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

groupshared uint64_t s_voxel_val[256u];

struct prim_t
{
    float3 pos_a;
    uint id_a;
    float3 pos_b;
    uint id_b;
    float3 pos_c;
    uint id_c;
};
groupshared prim_t s_prims[256u];

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
    workgroup_voxelbvh_iterator_init(iter, thread_idx, 32u, grid, voxel_val_raw);

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

            pnanovdb_address_t pos_addr = pnanovdb_grid_get_gridblindmetadata_value_address(buf_scratch, grid, 3u);
            float3 pos_a = pnanovdb_read_vec3(buf_scratch, pnanovdb_address_offset_product(pos_addr, 12u, id_a));
            float3 pos_b = pnanovdb_read_vec3(buf_scratch, pnanovdb_address_offset_product(pos_addr, 12u, id_b));
            float3 pos_c = pnanovdb_read_vec3(buf_scratch, pnanovdb_address_offset_product(pos_addr, 12u, id_c));

            s_prims[thread_idx].pos_a = pos_a;
            s_prims[thread_idx].id_a = id_a;
            s_prims[thread_idx].pos_b = pos_b;
            s_prims[thread_idx].id_b = id_b;
            s_prims[thread_idx].pos_c = pos_c;
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
                    s_prims[s_idx].pos_a,
                    s_prims[s_idx].id_a,
                    s_prims[s_idx].pos_b,
                    s_prims[s_idx].id_b,
                    s_prims[s_idx].pos_c,
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

void iterate_main(pnanovdb_address_t node_arr, pnanovdb_address_t val_addr, uint level, uint n,
                  pnanovdb_coord_t ijk, inout user_state_t user_state)
{
    // assuming single grid for now
    pnanovdb_grid_handle_t grid = {};

    float3 worldRayOrigin = {};
    float3 worldRayDir = {};
    float3 worldRayDirInv = {};
    float worldTmin = 0.f;
    float worldTmax = 0.f;
    uint64_t voxel_val_raw = 0llu;

    bool this_thread_active = !pnanovdb_address_is_null(val_addr);
    if (this_thread_active)
    {
        float4 color = float4(0.f, 0.f, 0.f, 0.f);
        int3 ijk_min = ijk;
        int3 ijk_max = ijk;
        if (level == 3)
        {
            ijk_max += 4096u;
        }
        if (level == 2)
        {
            ijk_max += 128u;
        }
        if (level == 1)
        {
            ijk_max += 8u;
        }
        if (level == 0)
        {
            ijk_max += 1u;
        }

        float3 ijkf = 0.5f * float3(ijk_min) + 0.5f * float3(ijk_max);

        worldRayOrigin = ijkf;
        worldRayOrigin.x = float(ijk_max.x);
        worldRayDir = float3(-1.f, 0.f, 0.f);
        worldTmin = 0.f;
        worldTmax = float(ijk_max.x) - float(ijk_min.x);

        worldRayOrigin = pnanovdb_grid_index_to_worldf(buf, grid, worldRayOrigin);
        worldRayDir = pnanovdb_grid_index_to_world_dirf(buf, grid, worldRayDir);
        float rayDirMagn = length(worldRayDir);
        if (rayDirMagn > 0.f)
        {
            worldRayDir /= rayDirMagn;
            worldTmin *= rayDirMagn;
            worldTmax *= rayDirMagn;
        }
        worldRayDirInv = float3(1.f, 1.f, 1.f) / worldRayDir;

        pnanovdb_grid_handle_t src_grid = {};
        pnanovdb_tree_handle_t src_tree = pnanovdb_grid_get_tree(buf_scratch, src_grid);
        pnanovdb_root_handle_t src_root = pnanovdb_tree_get_root(buf_scratch, src_tree);
        pnanovdb_uint32_t src_grid_type = pnanovdb_grid_get_grid_type(buf_scratch, src_grid);
        int3 ijk_center = int3(floor(ijkf));
        pnanovdb_address_t src_addr = pnanovdb_root_get_value_address(src_grid_type, buf_scratch, src_root, ijk_center);
        voxel_val_raw = pnanovdb_read_uint64(buf_scratch, src_addr);
    }

    s_voxel_val[n & 255] = voxel_val_raw;

    GroupMemoryBarrierWithGroupSync();

    float4 sum = float4(0.f, 0.f, 0.f, 1.f);
    float nominalT = 0.f;
    for (uint s_idx = 0u; s_idx < 256u; s_idx++)
    {
        workgroup_triangle_process(n & 255,
                                this_thread_active && s_idx == (n & 255),
                                grid,
                                worldRayOrigin,
                                worldRayDir,
                                worldRayDirInv,
                                worldTmin,
                                worldTmax,
                                s_voxel_val[s_idx],
                                sum,
                                nominalT);
    }

    if (this_thread_active)
    {
        if (voxel_val_raw != 0)
        {
            sum.b += 0.5f;
            sum.a = 0.5f;
        }

        float4 color = float4(sum.rgb, 1.f - sum.a);
        uint rgba_raw = uint(255.f * max(0.f, min(1.f, color.x))) | (uint(255.f * max(0.f, min(1.f, color.y))) << 8u) |
                        (uint(255.f * max(0.f, min(1.f, color.z))) << 16u) |
                        (uint(255.f * max(0.f, min(1.f, color.w))) << 24u);
        pnanovdb_write_uint32(buf, val_addr, rgba_raw);
    }
}

[shader("compute")][numthreads(256, 1, 1)]
void main(uint3 group_idx : SV_GroupID, uint3 thread_idx : SV_GroupThreadID)
{
    user_state_t user_state = {};
    do_iteration(group_idx.x, thread_idx.x, user_state);
}
