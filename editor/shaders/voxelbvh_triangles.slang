// editor.slang
#define PNANOVDB_HLSL
#define PNANOVDB_ADDRESS_64
#define PNANOVDB_BUF_HLSL_64
#include "PNanoVDB.h"
#include "PNanoVDBExt.h"

struct EditorParams
{
    float4 view_inv_x;
    float4 view_inv_y;
    float4 view_inv_z;
    float4 view_inv_w;
    float4 projection_inv_x;
    float4 projection_inv_y;
    float4 projection_inv_z;
    float4 projection_inv_w;

    float4 view_x;
    float4 view_y;
    float4 view_z;
    float4 view_w;
    float4 projection_x;
    float4 projection_y;
    float4 projection_z;
    float4 projection_w;

    uint width;
    uint height;
    uint pad1;
    uint pad2;
};

struct shader_params_t
{
    uint mode;
};

StructuredBuffer<uint2> buf;
RWStructuredBuffer<uint> image_out;
RWTexture2D<float4> texture_out;
ConstantBuffer<EditorParams> editor_params;
ConstantBuffer<shader_params_t> shader_params;

float3 cross_product(float3 a, float3 b)
{
    return float3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}

// ray origin is implied zero
bool intersect_box(float3 rayDir, float3 rayDirInv, float3 boxMin, float3 boxMax, out float tnear, out float tfar)
{
    // compute intersection of ray with all six bbox planes
    float3 tbot = boxMin * rayDirInv;
    float3 ttop = boxMax * rayDirInv;

    // re-order intersections to find smallest and largest on each axis
    float3 tmin = min(ttop, tbot);
    float3 tmax = max(ttop, tbot);

    // find the largest tmin and the smallest tmax
    tnear = max(max(tmin.x, tmin.y), max(tmin.x, tmin.z));
    tfar = min(min(tmax.x, tmax.y), min(tmax.x, tmax.z));

    return tfar > tnear;
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
    float3 pos_a,
    float3 pos_b,
    float3 pos_c,
    float3 color_a,
    float3 color_b,
    float3 color_c,
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
        alpha = 1.f;
        t = t_isect.x;
        if (shader_params.mode == 1)
        {
            float3 n = normalize(cross_product( pos_b - pos_a, pos_c - pos_a));
            color.rgb = abs(dot(n, rayDir)).xxx;
            color.rgb *= 1.5f;
            color.rgb *= (1.f - t_isect.y - t_isect.z) * color_a + t_isect.y * color_b + t_isect.z * color_c;
        }
        else if (shader_params.mode == 2)
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

uint sort_asuint(float f)
{
    uint f_uint = asuint(f);
    uint mask = -int(f_uint >> 31) | 0x80000000;
    return f_uint ^ mask;
}

float2 compute_hdda_culling_range(StructuredBuffer<uint2> buf,
                                  pnanovdb_grid_handle_t grid,
                                  float3 worldRayDirCenter,
                                  int3 ijk,
                                  int dim)
{
    int dim_mask = dim - 1;
    int3 ijk_min = ijk & ~dim_mask;
    int3 ijk_max = ijk_min + int3(dim);
    float2 range = float2(1.f / 0.f, -1.f / 0.f);
    for (int i = 0u; i < 8; i++)
    {
        int3 cmp_ijk = int3(
            (i & 1) == 0 ? ijk_min.x : ijk_max.x,
            (i & 2) == 0 ? ijk_min.y : ijk_max.y,
            (i & 4) == 0 ? ijk_min.z : ijk_max.z
        );
        float3 pos = pnanovdb_grid_index_to_worldf(buf, grid, float3(cmp_ijk));
        float t = dot(pos, worldRayDirCenter);
        range.x = min(range.x, t);
        range.y = max(range.y, t);
    }
    return range;
}

struct hdda_t
{
    int3 ijk;
    float tmin;
    float3 ijk_sub;
    float tmax;
};

void hdda_init(inout hdda_t hdda, float3 ray_origin, float3 ray_dir, float3 ray_dir_inv, float tmin)
{
    float3 ray_pos = ray_dir * tmin + ray_origin;

    hdda.ijk = int3(floor(ray_pos));
    hdda.ijk_sub = ray_pos - float3(hdda.ijk);
    hdda.tmin = tmin;
    hdda.tmax = tmin;
}

void hdda_update(inout hdda_t hdda, float3 ray_origin, float3 ray_dir, float3 ray_dir_inv, int dim)
{
    // based on dim, put everything in a local space
    int3 offset_to_local = hdda.ijk & ~(dim - 1);

    float3 box_min = float3(offset_to_local) - ray_origin;
    float3 box_max = float3(offset_to_local + int3(dim, dim, dim)) - ray_origin;
    intersect_box(ray_dir, ray_dir_inv, box_min, box_max, hdda.tmin, hdda.tmax);
}

void hdda_step(inout hdda_t hdda, float3 ray_origin, float3 ray_dir, float3 ray_dir_inv, int dim)
{
    // based on dim, put everything in a local space
    int3 offset_to_local = hdda.ijk & ~(dim - 1);

    float3 local_pos = float3(hdda.ijk - offset_to_local) + hdda.ijk_sub;
    float3 local_max = float3(
        (ray_dir.x > 0.f ? float(dim) : 0.f),
        (ray_dir.y > 0.f ? float(dim) : 0.f),
        (ray_dir.z > 0.f ? float(dim) : 0.f)
    );

    float3 hit_ts = float3(local_max - local_pos) * ray_dir_inv;
    float hit_t;
    int axis;
    if (ray_dir.x != 0.f && (hit_ts.x <= hit_ts.y || ray_dir.y == 0.f) && (hit_ts.x <= hit_ts.z || ray_dir.z == 0.f))
    {
        hit_t = hit_ts.x;
        axis = 0;
    }
    else if (ray_dir.y != 0.f && (hit_ts.y <= hit_ts.x || ray_dir.x == 0.f) && (hit_ts.y <= hit_ts.z || ray_dir.z == 0.f))
    {
        hit_t = hit_ts.y;
        axis = 1;
    }
    else
    {
        hit_t = hit_ts.z;
        axis = 2;
    }

    float3 local_pos_new = local_pos + hit_t * ray_dir;
    hdda.ijk = max(0, min(dim - 1, int3(floor(local_pos_new)))) + offset_to_local;
    hdda.ijk_sub = local_pos_new - float3(hdda.ijk - offset_to_local);
    if (axis == 0)
    {
        hdda.ijk.x = offset_to_local.x + (ray_dir.x > 0.f ? dim : -1);
        hdda.ijk_sub.x = ray_dir.x > 0.f ? 0.f : 1.f;
    }
    if (axis == 1)
    {
        hdda.ijk.y = offset_to_local.y + (ray_dir.y > 0.f ? dim : -1);
        hdda.ijk_sub.y = ray_dir.y > 0.f ? 0.f : 1.f;
    }
    if (axis == 2)
    {
        hdda.ijk.z = offset_to_local.z + (ray_dir.z > 0.f ? dim : -1);
        hdda.ijk_sub.z = ray_dir.z > 0.f ? 0.f : 1.f;
    }

    hdda.tmin = hdda.tmax;
}

groupshared uint s_active[4u];
groupshared uint s_tmin[128u];
groupshared uint64_t s_voxel_val[128u];
groupshared uint64_t s_ijkl[128u];
groupshared uint s_min_tmin;
groupshared uint s_min_hdda_idx;

groupshared uint64_t s_range[12u];

groupshared float2 s_cull_range[128u];

groupshared uint s_prim_keys[512u];

groupshared uint smem[256u];
uint read_smem_idx(uint addr, uint idx)
{
    return smem[addr + idx];
}
void write_smem_idx(uint addr, uint idx, uint val)
{
    smem[addr + idx] = val;
}
void workgroup_scan(uint thread_idx, uint val, inout uint result, inout uint total_count)
{
    uint sdata0_addr = 0;
    uint sdata1_addr = 128u;

    uint local_val = val;
    write_smem_idx(sdata0_addr, thread_idx, local_val);

    GroupMemoryBarrierWithGroupSync();

    if (thread_idx >= 1)
        local_val += read_smem_idx(sdata0_addr, thread_idx - 1);
    if (thread_idx >= 2)
        local_val += read_smem_idx(sdata0_addr, thread_idx - 2);
    if (thread_idx >= 3)
        local_val += read_smem_idx(sdata0_addr, thread_idx - 3);
    write_smem_idx(sdata1_addr, thread_idx, local_val);

    GroupMemoryBarrierWithGroupSync();

    if (thread_idx >= 4)
        local_val += read_smem_idx(sdata1_addr, thread_idx - 4);
    if (thread_idx >= 8)
        local_val += read_smem_idx(sdata1_addr, thread_idx - 8);
    if (thread_idx >= 12)
        local_val += read_smem_idx(sdata1_addr, thread_idx - 12);
    write_smem_idx(sdata0_addr, thread_idx, local_val);

    GroupMemoryBarrierWithGroupSync();

    if (thread_idx >= 16)
        local_val += read_smem_idx(sdata0_addr, thread_idx - 16);
    if (thread_idx >= 32)
        local_val += read_smem_idx(sdata0_addr, thread_idx - 32);
    if (thread_idx >= 48)
        local_val += read_smem_idx(sdata0_addr, thread_idx - 48);
    write_smem_idx(sdata1_addr, thread_idx, local_val);

    GroupMemoryBarrierWithGroupSync();

    if (thread_idx >= 64)
        local_val += read_smem_idx(sdata1_addr, thread_idx - 64);

    result = local_val;

    // compute total_count
    total_count = read_smem_idx(sdata1_addr, 63) + read_smem_idx(sdata1_addr, 127);
}

struct prim_t
{
    float3 pos_a;
    uint id;
    float3 pos_b;
    float3 pos_c;
    float3 color_a;
    float3 color_b;
    float3 color_c;
};
groupshared prim_t s_prims[128u];

void ray_march_nanovdb(uint thread_idx,
                       StructuredBuffer<uint2> buf,
                       float3 worldRayOrigin,
                       float rayMinT,
                       float3 worldRayDir,
                       float rayMaxT,
                       float3 worldRayDirInv,
                       float3 worldRayDirCenter,
                       inout float4 sum,
                       inout float nominalT)
{
    pnanovdb_grid_handle_t grid = { pnanovdb_address_null() };
    pnanovdb_tree_handle_t tree = pnanovdb_grid_get_tree(buf, grid);
    pnanovdb_root_handle_t root = pnanovdb_tree_get_root(buf, tree);
    pnanovdb_grid_type_t grid_type = pnanovdb_grid_get_grid_type(buf, grid);

    pnanovdb_readaccessor_t acc;
    pnanovdb_readaccessor_init(PNANOVDB_REF(acc), root);

    // transform ray from world to index space
    float3 rayOrigin = pnanovdb_grid_world_to_indexf(buf, grid, worldRayOrigin);
    float3 rayDir = pnanovdb_grid_world_to_index_dirf(buf, grid, worldRayDir);
    float rayDirMagn = length(rayDir);
    float rayDirMagnInv = 1.f;
    if (rayDirMagn > 0.f)
    {
        rayDirMagnInv = 1.f / rayDirMagn;
        rayDir *= rayDirMagnInv;
        rayMinT *= rayDirMagn;
        rayMaxT *= rayDirMagn;
    }
    float3 rayDirInv = float3(1.f, 1.f, 1.f) / rayDir;

    // intersect local ray with local bbox
    int3 bbox_min = pnanovdb_root_get_bbox_min(buf, root);
    int3 bbox_max = pnanovdb_root_get_bbox_max(buf, root);

    float3 bbox_minf = float3(bbox_min - int3(1,1,1));
    float3 bbox_maxf = float3(bbox_max + int3(2,2,2));

    float boxMinT;
    float boxMaxT;
    bool isHit = intersect_box(rayDir, rayDirInv, bbox_minf - rayOrigin, bbox_maxf - rayOrigin, boxMinT, boxMaxT);

    boxMinT = max(rayMinT, boxMinT);
    if (boxMinT > boxMaxT)
    {
        isHit = false;
    }

    bool is_active = isHit;
    hdda_t hdda = {};
    if (is_active)
    {
        hdda_init(hdda, rayOrigin, rayDir, rayDirInv, boxMinT);
        hdda_update(hdda, rayOrigin, rayDir, rayDirInv, 1);
    }
    bool hdda_needs_update = true;

    bool has_set_inactive = false;
    do
    {
        GroupMemoryBarrierWithGroupSync();

        // update HDDA state as needed
        if (is_active)
        {
            if (hdda_needs_update)
            {
                hdda_needs_update = false;

                pnanovdb_address_t addr = pnanovdb_readaccessor_get_value_address(grid_type, buf, acc, hdda.ijk);
                uint64_t val_raw = pnanovdb_read_uint64(buf, addr);
                uint active_level_mask = uint(val_raw) & 0xFFFF;
                uint skip_level = (uint(val_raw) >> 16u) & 0xFFFF;
                uint list_begin_idx = uint(val_raw >> 32u);

                // align HDDA tmin/tmax to current voxel size
                int dim = 1 << skip_level;
                hdda_update(hdda, rayOrigin, rayDir, rayDirInv, dim);

                int4 ijkl = int4(
                  hdda.ijk.x & ~(dim - 1),
                  hdda.ijk.y & ~(dim - 1),
                  hdda.ijk.z & ~(dim - 1),
                  skip_level > 0u ? skip_level - 1u : 0u
                );
                uint64_t ijkl_64 = (uint64_t(ijkl.x) << 48u) | (uint64_t(ijkl.y) << 32u) |
                                   (uint64_t(ijkl.z) << 16u) | uint64_t(ijkl.w);

                s_voxel_val[thread_idx] = val_raw;
                s_ijkl[thread_idx] = ijkl_64;
                s_tmin[thread_idx] = sort_asuint(hdda.tmin);
                s_cull_range[thread_idx] = compute_hdda_culling_range(buf, grid, worldRayDirCenter, hdda.ijk, dim);
            }
        }
        else
        {
            s_voxel_val[thread_idx] = ~uint64_t(0u);
            s_tmin[thread_idx] = sort_asuint(1.f / 0.f);
            s_cull_range[thread_idx] = float2(1.f / 0.f, 1.f / 0.f);
        }

        // vote for voxel val to update cooperatively
        if (thread_idx == 0u)
        {
            s_min_tmin = sort_asuint(1.f / 0.f);
            s_min_hdda_idx = ~0u;
        }
        GroupMemoryBarrierWithGroupSync();
        if (is_active)
        {
            InterlockedMin(s_min_tmin, s_tmin[thread_idx]);
        }
        GroupMemoryBarrierWithGroupSync();
        if (is_active && s_min_tmin == s_tmin[thread_idx])
        {
            InterlockedMin(s_min_hdda_idx, thread_idx);
        }
        GroupMemoryBarrierWithGroupSync();

        // cooperatively process min_voxel_val
        if (s_voxel_val[s_min_hdda_idx] != ~uint64_t(0u))
        {
            uint active_level_mask = uint(s_voxel_val[s_min_hdda_idx]) & 0xFFFF;
            uint list_begin_idx = uint(s_voxel_val[s_min_hdda_idx] >> 32u);

            // show only smallest three voxel sizes for now
            uint filter_mask = 0xFFF;
            uint active_level_count = countbits(active_level_mask & filter_mask);

            if (thread_idx < 12u)
            {
                pnanovdb_uint64_t range = {};
                if (thread_idx < active_level_count)
                {
                    pnanovdb_address_t range_addr = pnanovdb_grid_get_gridblindmetadata_value_address(buf, grid, 0u);
                    range_addr = pnanovdb_address_offset_product(range_addr, 8u, list_begin_idx);
                    range = pnanovdb_read_uint64(buf, pnanovdb_address_offset(range_addr, 8u * thread_idx));
                }
                s_range[thread_idx] = range;
            }
            GroupMemoryBarrierWithGroupSync();
            uint total_count = 0u;
            for (uint active_level_idx = 0u; active_level_idx < active_level_count; active_level_idx++)
            {
                pnanovdb_uint64_t range = s_range[active_level_idx];
                total_count += uint(range >> 32u) - uint(range);
            }

            uint workgroup_scan_accum = 0u;
            uint pass_count = (total_count + 127u) / 128u;
            for (uint pass_idx = 0u; pass_idx < pass_count; pass_idx++)
            {
                uint flat_list_idx = 128u * pass_idx + thread_idx;

                uint list_accum = 0u;
                uint list_idx = ~0u;
                for (uint active_level_idx = 0u; active_level_idx < active_level_count; active_level_idx++)
                {
                    uint range_begin = uint(s_range[active_level_idx]);
                    uint range_end = uint(s_range[active_level_idx] >> 32u);
                    uint list_idx_cmp = range_begin + flat_list_idx - list_accum;
                    if (list_idx_cmp < range_end)
                    {
                        list_idx = list_idx_cmp;
                        break;
                    }
                    list_accum += (range_end - range_begin);
                }

                uint prim_key = ~0u;
                if (list_idx != ~0u)
                {
                    // culling stage
                    pnanovdb_address_t prim_id_addr = pnanovdb_grid_get_gridblindmetadata_value_address(buf, grid, 1u);
                    uint prim_id = pnanovdb_read_uint32(buf, pnanovdb_address_offset_product(prim_id_addr, 4u, list_idx));

                    pnanovdb_address_t index_addr = pnanovdb_grid_get_gridblindmetadata_value_address(buf, grid, 2u);
                    pnanovdb_uint32_t id_a = pnanovdb_read_uint32(buf, pnanovdb_address_offset(index_addr, 12u * prim_id + 0u));
                    pnanovdb_uint32_t id_b = pnanovdb_read_uint32(buf, pnanovdb_address_offset(index_addr, 12u * prim_id + 4u));
                    pnanovdb_uint32_t id_c = pnanovdb_read_uint32(buf, pnanovdb_address_offset(index_addr, 12u * prim_id + 8u));

                    pnanovdb_address_t pos_addr = pnanovdb_grid_get_gridblindmetadata_value_address(buf, grid, 3u);
                    float3 pos_a = pnanovdb_read_vec3(buf, pnanovdb_address_offset_product(pos_addr, 12u, id_a));
                    float3 pos_b = pnanovdb_read_vec3(buf, pnanovdb_address_offset_product(pos_addr, 12u, id_b));
                    float3 pos_c = pnanovdb_read_vec3(buf, pnanovdb_address_offset_product(pos_addr, 12u, id_c));

                    // see if mean falls within cull range
                    float cull_t_a = dot(pos_a, worldRayDirCenter);
                    float cull_t_b = dot(pos_b, worldRayDirCenter);
                    float cull_t_c = dot(pos_c, worldRayDirCenter);

                    prim_key = prim_id;

                    bool should_cull_t =
                        (cull_t_a < s_cull_range[s_min_hdda_idx].x && cull_t_b < s_cull_range[s_min_hdda_idx].x && cull_t_c < s_cull_range[s_min_hdda_idx].x) ||
                        (cull_t_a > s_cull_range[s_min_hdda_idx].y && cull_t_b > s_cull_range[s_min_hdda_idx].y && cull_t_c > s_cull_range[s_min_hdda_idx].y);
                    if (should_cull_t)
                    {
                        //prim_key = ~0u;
                    }
                }

                uint scan_val = 0u;
                uint scan_total = 0u;
                workgroup_scan(thread_idx, prim_key != ~0u ? 1u : 0u, scan_val, scan_total);
                if (prim_key != ~0u)
                {
                    uint write_idx = scan_val - 1u + workgroup_scan_accum;
                    if (write_idx < 512u)
                    {
                        s_prim_keys[write_idx] = prim_key;
                    }
                }
                workgroup_scan_accum += scan_total;
                if (workgroup_scan_accum > 512u)
                {
                    break;
                }
            }

            if (workgroup_scan_accum > 512u)
            {
                workgroup_scan_accum = 512u;
            }

            for (uint write_idx = thread_idx; write_idx < 512u; write_idx += 128u)
            {
                if (write_idx >= workgroup_scan_accum)
                {
                    s_prim_keys[write_idx] = ~0u;
                }
            }

            GroupMemoryBarrierWithGroupSync();

            pass_count = (workgroup_scan_accum + 127u) / 128u;
            for (uint pass_idx = 0u; pass_idx < pass_count; pass_idx++)
            {
                uint list_idx = 128u * pass_idx + thread_idx;

                if (list_idx < workgroup_scan_accum)
                {
                    // load lines into shared memory
                    uint prim_id = s_prim_keys[list_idx];

                    s_prims[thread_idx].id = prim_id;

                    pnanovdb_address_t index_addr = pnanovdb_grid_get_gridblindmetadata_value_address(buf, grid, 2u);
                    pnanovdb_uint32_t id_a = pnanovdb_read_uint32(buf, pnanovdb_address_offset(index_addr, 12u * prim_id + 0u));
                    pnanovdb_uint32_t id_b = pnanovdb_read_uint32(buf, pnanovdb_address_offset(index_addr, 12u * prim_id + 4u));
                    pnanovdb_uint32_t id_c = pnanovdb_read_uint32(buf, pnanovdb_address_offset(index_addr, 12u * prim_id + 8u));

                    pnanovdb_address_t pos_addr = pnanovdb_grid_get_gridblindmetadata_value_address(buf, grid, 3u);
                    float3 pos_a = pnanovdb_read_vec3(buf, pnanovdb_address_offset_product(pos_addr, 12u, id_a));
                    float3 pos_b = pnanovdb_read_vec3(buf, pnanovdb_address_offset_product(pos_addr, 12u, id_b));
                    float3 pos_c = pnanovdb_read_vec3(buf, pnanovdb_address_offset_product(pos_addr, 12u, id_c));

                    pnanovdb_address_t color_addr = pnanovdb_grid_get_gridblindmetadata_value_address(buf, grid, 4u);
                    float3 color_a = pnanovdb_read_vec3(buf, pnanovdb_address_offset_product(color_addr, 12u, id_a));
                    float3 color_b = pnanovdb_read_vec3(buf, pnanovdb_address_offset_product(color_addr, 12u, id_b));
                    float3 color_c = pnanovdb_read_vec3(buf, pnanovdb_address_offset_product(color_addr, 12u, id_c));

                    s_prims[thread_idx].pos_a = pos_a;
                    s_prims[thread_idx].pos_b = pos_b;
                    s_prims[thread_idx].pos_c = pos_c;
                    s_prims[thread_idx].color_a = color_a;
                    s_prims[thread_idx].color_b = color_b;
                    s_prims[thread_idx].color_c = color_c;
                }

                GroupMemoryBarrierWithGroupSync();

                uint prim_count = workgroup_scan_accum - 128u * pass_idx;
                if (prim_count > 128u)
                {
                    prim_count = 128u;
                }

                // only participate if this pixel actually wanted this data
                if (s_ijkl[s_min_hdda_idx] == s_ijkl[thread_idx])
                {
                    for (uint s_idx = 0u; s_idx < prim_count; s_idx++)
                    {
                        if (s_prims[s_idx].id != ~0u)
                        {
                            intersect_triangle(
                                s_prims[s_idx].pos_a,
                                s_prims[s_idx].pos_b,
                                s_prims[s_idx].pos_c,
                                s_prims[s_idx].color_a,
                                s_prims[s_idx].color_b,
                                s_prims[s_idx].color_c,
                                worldRayOrigin,
                                hdda.tmin * rayDirMagnInv,
                                worldRayDir,
                                hdda.tmax * rayDirMagnInv,
                                worldRayDirInv,
                                sum,
                                nominalT
                            );
                        }
                    }
                }

                GroupMemoryBarrierWithGroupSync();
            }
        }

        if (is_active)
        {
            // only step if target voxel was processed
            if (s_ijkl[s_min_hdda_idx] == s_ijkl[thread_idx])
            {
                hdda_needs_update = true;

                uint skip_level = (uint(s_voxel_val[s_min_hdda_idx]) >> 16u) & 0xFFFF;
                // align HDDA tmin/tmax to current voxel size
                int dim = 1 << skip_level;
                hdda_step(hdda, rayOrigin, rayDir, rayDirInv, dim);
            }
        }

        if (!(hdda.tmin < boxMaxT && sum.a > 0.0001f))
        {
            is_active = false;
        }
        // failed to excute anything meaningful, exit due to lack of progress
        if (s_voxel_val[s_min_hdda_idx] == ~uint64_t(0u))
        {
            is_active = false;
            //sum.r = 1.f;
        }

        if (!is_active && !has_set_inactive)
        {
            has_set_inactive = true;
            InterlockedAnd(s_active[thread_idx >> 5u], ~(1u << (thread_idx & 31)));
        }
        GroupMemoryBarrierWithGroupSync();

    } while (s_active[0] != 0u || s_active[1] != 0u || s_active[2] != 0u || s_active[3] != 0u);
}

float4 mat_mul(float4 v, float4 x, float4 y, float4 z, float4 w)
{
    return float4(dot(v, x), dot(v, y), dot(v, z), dot(v, w));
}

void get_center_ray_dir(out float3 rayDir)
{
    float2 ndc = float2(0.f, 0.f);
    float4 pos_d0 = mat_mul(float4(ndc.xy, 0.f, 1.f),
                            editor_params.projection_inv_x, editor_params.projection_inv_y,
                            editor_params.projection_inv_z, editor_params.projection_inv_w);
    float4 pos_d1 = mat_mul(float4(ndc.xy, 1.f, 1.f),
                            editor_params.projection_inv_x, editor_params.projection_inv_y,
                            editor_params.projection_inv_z, editor_params.projection_inv_w);

    float z_d0 = pos_d0.z * (1.f / pos_d0.w);
    float z_d1 = pos_d1.z * (1.f / pos_d1.w);
    bool is_reverse_z = abs(z_d0) > abs(z_d1);
    float4 ray_dir_near = is_reverse_z ? pos_d1 : pos_d0;

    float4 ray_dir_far = ray_dir_near + mat_mul(float4(0.f, 0.f, 1.f, 0.f),
                                                editor_params.projection_inv_x, editor_params.projection_inv_y,
                                                editor_params.projection_inv_z, editor_params.projection_inv_w);
    rayDir = normalize((ray_dir_far.xyz / ray_dir_far.w) - (ray_dir_near.xyz / ray_dir_near.w));
    if (is_reverse_z)
    {
        rayDir = -rayDir;
    }

    rayDir = mat_mul(float4(rayDir, 0.f),
                     editor_params.view_inv_x, editor_params.view_inv_y,
                     editor_params.view_inv_z, editor_params.view_inv_w) .xyz;
    rayDir = normalize(rayDir);
}

[shader("compute")][numthreads(16, 8, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID, uint3 thread_idx : SV_GroupThreadID)
{
    int2 tidx = int2(dispatchThreadID.xy);
    if (tidx.y >= editor_params.height)
    {
        tidx.y -= editor_params.height;
        tidx.x += editor_params.width / 2u;
    }
    uint thread_idx1d = thread_idx.x | (thread_idx.y << 4);

    // initialize active mask
    if (thread_idx1d < 4u)
    {
        s_active[thread_idx1d] = ~0u;
    }
    GroupMemoryBarrierWithGroupSync();

    float2 ndc = float2(2.f * ((float(tidx.x) + 0.5f) / float(editor_params.width)) - 1.f,
                        -2.f * ((float(tidx.y) + 0.5f) / float(editor_params.height)) + 1.f);

    float4 pos_d0 = mat_mul(float4(ndc.xy, 0.f, 1.f), editor_params.projection_inv_x, editor_params.projection_inv_y,
                            editor_params.projection_inv_z, editor_params.projection_inv_w);
    float4 pos_d1 = mat_mul(float4(ndc.xy, 1.f, 1.f), editor_params.projection_inv_x, editor_params.projection_inv_y,
                            editor_params.projection_inv_z, editor_params.projection_inv_w);

    float z_d0 = pos_d0.z * (1.f / pos_d0.w);
    float z_d1 = pos_d1.z * (1.f / pos_d1.w);
    bool is_reverse_z = abs(z_d0) > abs(z_d1);
    float4 ray_dir_near = is_reverse_z ? pos_d1 : pos_d0;

    float4 ray_dir_far = ray_dir_near + mat_mul(float4(0.f, 0.f, 1.f, 0.f), editor_params.projection_inv_x,
                                                editor_params.projection_inv_y, editor_params.projection_inv_z,
                                                editor_params.projection_inv_w);
    float3 rayDir = normalize((ray_dir_far.xyz / ray_dir_far.w) - (ray_dir_near.xyz / ray_dir_near.w));
    if (is_reverse_z)
    {
        rayDir = -rayDir;
    }

    rayDir = mat_mul(float4(rayDir, 0.f), editor_params.view_inv_x, editor_params.view_inv_y, editor_params.view_inv_z,
                     editor_params.view_inv_w)
                 .xyz;
    float3 rayDirInv = float3(1.f, 1.f, 1.f) / rayDir;

    float4 rayOrigin4 = is_reverse_z ? pos_d1 : pos_d0;
    rayOrigin4 = mat_mul(rayOrigin4, editor_params.view_inv_x, editor_params.view_inv_y, editor_params.view_inv_z,
                         editor_params.view_inv_w);
    float3 rayOrigin = rayOrigin4.xyz / rayOrigin4.w;

    float3 rayDirCenter = {};
    get_center_ray_dir(rayDirCenter);

    pnanovdb_grid_handle_t grid = { pnanovdb_address_null() };
    pnanovdb_uint32_t grid_type = pnanovdb_grid_get_grid_type(buf, grid);

    float4 sum = float4(0.f, 0.f, 0.f, 1.f);
    float nominalT = 0.f;
    ray_march_nanovdb(thread_idx1d, buf, rayOrigin, 0.f, rayDir, 1e9f, rayDirInv, rayDirCenter, sum, nominalT);

    float4 value = sum;
    // Note: BGRA target
    uint rawValue = uint(255.f * max(0.f, min(1.f, value.z))) | (uint(255.f * max(0.f, min(1.f, value.y))) << 8u) |
                    (uint(255.f * max(0.f, min(1.f, value.x))) << 16u) |
                    (uint(255.f * max(0.f, min(1.f, value.w))) << 24u);
    image_out[(editor_params.height - 1 - tidx.y) * editor_params.width + tidx.x] = rawValue;

    texture_out[tidx] = value;
}
