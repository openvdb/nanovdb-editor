// voxelbvh_lines.slang
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
    float line_radius;
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
float4 intersect_rounded_cone(float3 ro, float3 rd, float3 pa, float3 pb, float ra, float rb)
{
    float3  ba = pb - pa;
    float3  oa = ro - pa;
    float3  ob = ro - pb;
    float rr = ra - rb;
    float m0 = dot(ba,ba);
    float m1 = dot(ba,oa);
    float m2 = dot(ba,rd);
    float m3 = dot(rd,oa);
    float m5 = dot(oa,oa);
    float m6 = dot(ob,rd);
    float m7 = dot(ob,ob);

    // body
    float d2 = m0-rr*rr;
    float k2 = d2    - m2*m2;
    float k1 = d2*m3 - m1*m2 + m2*rr*ra;
    float k0 = d2*m5 - m1*m1 + m1*rr*ra*2.0 - m0*ra*ra;
    float h = k1*k1 - k0*k2;
    if( h<0.0) return float4(-1.0);
    float t = (-sqrt(h)-k1)/k2;
    //if( t<0.0 ) return float4(-1.0);
    float y = m1 - ra*rr + t*m2;
    if( y>0.0 && y<d2 ) return float4(t, normalize(d2*(oa+t*rd)-ba*y));

    // caps
    float h1 = m3*m3 - m5 + ra*ra;
    float h2 = m6*m6 - m7 + rb*rb;
    if( max(h1,h2)<0.0 ) return float4(-1.0);
    float4 r = float4(1e20);
    if( h1>0.0 )
    {
        t = -m3 - sqrt( h1 );
        r = float4( t, (oa+t*rd)/ra );
    }
    if( h2>0.0 )
    {
        t = -m6 - sqrt( h2 );
        if( t<r.x )
        r = float4( t, (ob+t*rd)/rb );
    }
    return r;
}

void intersect_line(
    float3 pos_a,
    float3 pos_b,
    float3 color_a,
    float3 color_b,
    float3 rayOrigin,
    float tmin,
    float3 rayDir,
    float tmax,
    float3 rayDirInv,
    inout float4 sum,
    inout float nominalT)
{
    float4 t_isect_a = intersect_rounded_cone(rayOrigin, rayDir, pos_a, pos_b, shader_params.line_radius, shader_params.line_radius);

    float alpha = 0.f;
    float3 color = {};
    float t = 0.f;
    if (t_isect_a.x > 0.f)
    {
        alpha = 1.f;
        t = t_isect_a.x;
        color = 0.9f * abs(dot(rayDir, t_isect_a.yzw)).xxx + 0.33f;
        color = min(1.f.xxx, color);
        //color = 1.f.xxx;
        color = color * color_a;
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

struct prim_t
{
    float3 pos_a;
    uint id;
    float3 pos_b;
    float3 color_a;
    float3 color_b;
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
            }
        }
        else
        {
            s_voxel_val[thread_idx] = ~uint64_t(0u);
            s_tmin[thread_idx] = sort_asuint(1.f / 0.f);
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
                    pnanovdb_address_t prim_id_addr = pnanovdb_grid_get_gridblindmetadata_value_address(buf, grid, 1u);
                    uint prim_id = pnanovdb_read_uint32(buf, pnanovdb_address_offset_product(prim_id_addr, 4u, list_idx));

                    s_prims[thread_idx].id = prim_id;

                    pnanovdb_address_t index_addr = pnanovdb_grid_get_gridblindmetadata_value_address(buf, grid, 2u);
                    pnanovdb_uint64_t id_raw = pnanovdb_read_uint64(buf, pnanovdb_address_offset_product(index_addr, 8u, prim_id));
                    pnanovdb_uint32_t id_a = pnanovdb_uint32_t(id_raw);
                    pnanovdb_uint32_t id_b = pnanovdb_uint32_t(id_raw >> 32u);

                    pnanovdb_address_t pos_addr = pnanovdb_grid_get_gridblindmetadata_value_address(buf, grid, 3u);
                    float3 pos_a = pnanovdb_read_vec3(buf, pnanovdb_address_offset_product(pos_addr, 12u, id_a));
                    float3 pos_b = pnanovdb_read_vec3(buf, pnanovdb_address_offset_product(pos_addr, 12u, id_b));

                    pnanovdb_address_t color_addr = pnanovdb_grid_get_gridblindmetadata_value_address(buf, grid, 4u);
                    float3 color_a = pnanovdb_read_vec3(buf, pnanovdb_address_offset_product(color_addr, 12u, id_a));
                    float3 color_b = pnanovdb_read_vec3(buf, pnanovdb_address_offset_product(color_addr, 12u, id_b));

                    s_prims[thread_idx].pos_a = pos_a;
                    s_prims[thread_idx].pos_b = pos_b;
                    s_prims[thread_idx].color_a = color_a;
                    s_prims[thread_idx].color_b = color_b;
                }
                if (list_idx == ~0u)
                {
                    s_prims[thread_idx].id = ~0u;
                }

                GroupMemoryBarrierWithGroupSync();

                uint count = total_count - 128u * pass_idx;
                if (count > 128u)
                {
                    count = 128u;
                }

                // only participate if this pixel actually wanted this data
                if (s_ijkl[s_min_hdda_idx] == s_ijkl[thread_idx])
                {
                    for (uint s_idx = 0u; s_idx < count; s_idx++)
                    {
                        intersect_line(
                            s_prims[s_idx].pos_a,
                            s_prims[s_idx].pos_b,
                            s_prims[s_idx].color_a,
                            s_prims[s_idx].color_b,
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
