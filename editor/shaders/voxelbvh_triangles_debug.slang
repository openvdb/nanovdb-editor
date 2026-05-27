// voxelbvh_triangle_lines.slang
#define PNANOVDB_HLSL
#define PNANOVDB_ADDRESS_64
#define PNANOVDB_BUF_HLSL_64
#include "PNanoVDB.h"

#include "workgroup_hdda.slang"
#include "workgroup_voxelbvh.slang"

struct EditorParams
{
    float4x4 view_inv;
    float4x4 projection_inv;

    float4x4 view;
    float4x4 projection;

    uint width;
    uint height;
    uint frame_id;
    uint pad2;
};

struct shader_params_t
{
    uint mode;
    float line_radius;
    uint enable_slice_plane;
    float slice_plane_z;
    uint slice_plane_axis;
    uint show_first_active_level;
    uint show_list_ids;
    uint show_nanovdb_level;
    uint show_inactive;
    uint supersample;
};

StructuredBuffer<uint2> buf;
RWStructuredBuffer<uint> image_out_deprecated;
RWTexture2D<float4> texture_out;
ConstantBuffer<EditorParams> editor_params;
ConstantBuffer<shader_params_t> shader_params;

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
    float alpha = 0.f;
    float3 color = {};
    float3 n = float3(0.f, 0.f, 0.f);
    float t = 0.f;

    float3 t_isect = intersect_triangle(rayOrigin, rayDir, pos_a, pos_b, pos_c);
    if (t_isect.x > 0.f)
    {
        alpha = 1.f;
        t = t_isect.x;
        n = normalize(cross_product( pos_b - pos_a, pos_c - pos_a));
    }
    if (shader_params.line_radius != 0.f)
    {
        float4 t_isect_a = intersect_rounded_cone(rayOrigin, rayDir, pos_a, pos_b, shader_params.line_radius, shader_params.line_radius);
        float4 t_isect_b = intersect_rounded_cone(rayOrigin, rayDir, pos_b, pos_c, shader_params.line_radius, shader_params.line_radius);
        float4 t_isect_c = intersect_rounded_cone(rayOrigin, rayDir, pos_c, pos_a, shader_params.line_radius, shader_params.line_radius);
        if (t_isect_a.x > 0.f && (t == 0.f || t_isect_a.x < t))
        {
            alpha = 1.f;
            t = t_isect_a.x;
            n = t_isect_a.yzw;
        }
        if (t_isect_b.x > 0.f && (t == 0.f || t_isect_b.x < t))
        {
            alpha = 1.f;
            t = t_isect_b.x;
            n = t_isect_b.yzw;
        }
        if (t_isect_c.x > 0.f && (t == 0.f || t_isect_c.x < t))
        {
            alpha = 1.f;
            t = t_isect_c.x;
            n = t_isect_c.yzw;
        }
    }
    if (t > 0.f)
    {
        pnanovdb_address_t color_addr = pnanovdb_grid_get_gridblindmetadata_value_address(buf, grid, 4u);
        float3 color_a = pnanovdb_read_vec3(buf, pnanovdb_address_offset_product(color_addr, 12u, id_a));
        float3 color_b = pnanovdb_read_vec3(buf, pnanovdb_address_offset_product(color_addr, 12u, id_b));
        float3 color_c = pnanovdb_read_vec3(buf, pnanovdb_address_offset_product(color_addr, 12u, id_c));

        if (shader_params.mode == 1)
        {
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

groupshared uint64_t s_voxel_val[128u];

struct prim_t
{
    float3 pos_a;
    uint id_a;
    float3 pos_b;
    uint id_b;
    float3 pos_c;
    uint id_c;
};
groupshared prim_t s_prims[128u];

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
            pnanovdb_address_t prim_id_addr = pnanovdb_grid_get_gridblindmetadata_value_address(buf, grid, 1u);
            uint prim_id = pnanovdb_read_uint32(buf, pnanovdb_address_offset_product(prim_id_addr, 4u, prim_id_index));

            pnanovdb_address_t index_addr = pnanovdb_grid_get_gridblindmetadata_value_address(buf, grid, 2u);
            pnanovdb_uint32_t id_a = pnanovdb_read_uint32(buf, pnanovdb_address_offset(index_addr, 12u * prim_id + 0u));
            pnanovdb_uint32_t id_b = pnanovdb_read_uint32(buf, pnanovdb_address_offset(index_addr, 12u * prim_id + 4u));
            pnanovdb_uint32_t id_c = pnanovdb_read_uint32(buf, pnanovdb_address_offset(index_addr, 12u * prim_id + 8u));

            pnanovdb_address_t pos_addr = pnanovdb_grid_get_gridblindmetadata_value_address(buf, grid, 3u);
            float3 pos_a = pnanovdb_read_vec3(buf, pnanovdb_address_offset_product(pos_addr, 12u, id_a));
            float3 pos_b = pnanovdb_read_vec3(buf, pnanovdb_address_offset_product(pos_addr, 12u, id_b));
            float3 pos_c = pnanovdb_read_vec3(buf, pnanovdb_address_offset_product(pos_addr, 12u, id_c));

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

float3 color_idx_lookup(uint color_idx)
{
    float3 value = float3(0.f, 0.f, 0.f);
    if (color_idx == 0u) { value = float3(1.f, 0.f, 0.f);}
    if (color_idx == 1u) { value = float3(1.f, 1.f, 0.f);}
    if (color_idx == 2u) { value = float3(0.f, 1.f, 0.f);}
    if (color_idx == 3u) { value = float3(0.f, 1.f, 1.f);}
    if (color_idx == 4u) { value = float3(0.f, 0.f, 1.f);}
    if (color_idx == 5u) { value = float3(1.f, 0.f, 1.f);}
    if (color_idx == 6u) { value = float3(0.5f, 0.f, 0.f);}
    if (color_idx == 7u) { value = float3(0.5f, 0.5f, 0.f);}
    if (color_idx == 8u) { value = float3(0.f, 0.5f, 0.f);}
    if (color_idx == 9u) { value = float3(0.f, 0.5f, 0.5f);}
    if (color_idx == 10u) { value = float3(0.0f, 0.f, 0.5f);}
    if (color_idx == 11u) { value = float3(0.5f, 0.f, 0.5f);}
    if (color_idx == 12u) { value = float3(0.33f, 0.33f, 0.33f);}
    if (color_idx == 13u) { value = float3(1.f, 0.5f, 0.f);}
    return value;
}

bool grid_border_test(float3 ijkf, int width_level, uint border_level)
{
    float widthf = float(1u << width_level);
    int3 ijk_fine = int3(floor(widthf * ijkf));
    uint border_mask = (1u << (border_level + width_level)) - 1u;
    bool is_x_border = (ijk_fine.x & ~border_mask) == ijk_fine.x || (ijk_fine.x | border_mask) == ijk_fine.x;
    bool is_y_border = (ijk_fine.y & ~border_mask) == ijk_fine.y || (ijk_fine.y | border_mask) == ijk_fine.y;
    bool is_z_border = (ijk_fine.z & ~border_mask) == ijk_fine.z || (ijk_fine.z | border_mask) == ijk_fine.z;
    is_x_border = is_x_border && !(shader_params.slice_plane_axis == 0);
    is_y_border = is_y_border && !(shader_params.slice_plane_axis == 1);
    is_z_border = is_z_border && !(shader_params.slice_plane_axis == 2);
    return is_x_border || is_y_border || is_z_border;
}

void ray_debug_plane(
    StructuredBuffer<uint2> buf,
    uint grid_type,
    inout pnanovdb_readaccessor_t acc,
    inout float3 bbox_minf,
    inout float3 bbox_maxf,
    float3 rayOrigin,
    float3 rayDir,
    float3 rayDirCenter,
    inout float4 sum,
    inout float nominalT)
{
    float slice_plane_z = shader_params.slice_plane_z;
    float anim_t = (1.f / 900.f) * float(editor_params.frame_id % 900);
    slice_plane_z += 0.1f * (1.f - 2.f * abs(anim_t - 0.5f));
    float3 slice_point = (1.f - slice_plane_z) * bbox_minf + slice_plane_z * bbox_maxf;
    float3 normal = float3(
        shader_params.slice_plane_axis == 0u ? 1.f : 0.f,
        shader_params.slice_plane_axis == 1u ? 1.f : 0.f,
        shader_params.slice_plane_axis == 2u ? 1.f : 0.f);

    bool is_plane_front = dot(normal, rayDirCenter) < 0.f;
    bbox_minf = float3(
        (shader_params.slice_plane_axis == 0u && !is_plane_front) ? slice_point.x : bbox_minf.x,
        (shader_params.slice_plane_axis == 1u && !is_plane_front) ? slice_point.y : bbox_minf.y,
        (shader_params.slice_plane_axis == 2u && !is_plane_front) ? slice_point.z : bbox_minf.z
    );
    bbox_maxf = float3(
        (shader_params.slice_plane_axis == 0u && is_plane_front) ? slice_point.x : bbox_maxf.x,
        (shader_params.slice_plane_axis == 1u && is_plane_front) ? slice_point.y : bbox_maxf.y,
        (shader_params.slice_plane_axis == 2u && is_plane_front) ? slice_point.z : bbox_maxf.z
    );

    float d = -dot(slice_point, normal);
    float plane_t = -(dot(rayOrigin, normal) + d)/dot(rayDir, normal);
    if (plane_t > 0.f)
    {
        float3 ijkf = rayDir * plane_t + rayOrigin;
        int3 ijk = int3(floor(ijkf));

        pnanovdb_address_t addr = pnanovdb_readaccessor_get_value_address(grid_type, buf, acc, ijk);
        uint64_t val_raw = pnanovdb_read_uint64(buf, addr);
        uint active_level_mask = uint(val_raw) & 0xFFFF;
        uint skip_level = (uint(val_raw) >> 16u) & 0xFFFF;
        uint list_begin_idx = uint(val_raw >> 32u);

        uint border_level = skip_level;
        uint color_idx = skip_level;
        if (shader_params.show_first_active_level != 0u)
        {
            color_idx = firstbitlow(active_level_mask);
            border_level = color_idx;
        }
        if (shader_params.show_list_ids != 0u)
        {
            color_idx = list_begin_idx % 12;
        }
        if (shader_params.show_inactive != 0u)
        {
            if (active_level_mask == 0u)
            {
                color_idx = 13u;
            }
        }

        float4 value = float4(0.f, 0.f, 0.f, 0.f);
        if (grid_border_test(ijkf, 2, border_level))
        {
            value.rgb = color_idx_lookup(color_idx);
            value.a = 1.f;
        }

        if (shader_params.show_nanovdb_level != 0u && value.a < 0.5f)
        {
            border_level = 12u;
            if (acc.leaf.address.byte_offset != 0u)
            {
                border_level = 0u;
            }
            else if (acc.lower.address.byte_offset != 0u)
            {
                border_level = 3u;
            }
            else if (acc.upper.address.byte_offset != 0u)
            {
                border_level = 7u;
            }
            if (grid_border_test(ijkf, 3, border_level))
            {
                value.rgb = float3(0.5f, 0.5f, 0.5f);
                value.a = 1.f;
            }
        }

        float4 color = value;
        nominalT = sum.a * (color.a * plane_t) + nominalT;
        sum.rgb = sum.a * (color.a * color.rgb) + sum.rgb;
        sum.a = (1.f - color.a) * sum.a;
    }
}

void ray_hdda_nanovdb(uint thread_idx,
                       StructuredBuffer<uint2> buf,
                       float3 worldRayOrigin,
                       float rayMinT,
                       float3 worldRayDir,
                       float rayMaxT,
                       float3 worldRayDirInv,
                       float3 rayDirCenter,
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

    if (shader_params.enable_slice_plane != 0u)
    {
        ray_debug_plane(
            buf,
            grid_type,
            acc,
            bbox_minf,
            bbox_maxf,
            rayOrigin,
            rayDir,
            rayDirCenter,
            sum,
            nominalT
        );
    }

    float boxMinT;
    float boxMaxT;
    bool isHit = intersect_box(rayDir, rayDirInv, bbox_minf - rayOrigin, bbox_maxf - rayOrigin, boxMinT, boxMaxT);

    boxMinT = max(rayMinT, boxMinT);
    if (boxMinT > boxMaxT)
    {
        isHit = false;
    }

    workgroup_hdda_t hdda = {};
    workgroup_hdda_init(hdda, thread_idx, rayOrigin, rayDir, rayDirInv, isHit, boxMinT, boxMaxT);
    bool should_run = true;
    while(should_run)
    {
        // update HDDA state as needed
        if (workgroup_hdda_should_update(hdda, thread_idx))
        {
            int dim = 1;
            if (hdda.is_active)
            {
                pnanovdb_address_t addr = pnanovdb_readaccessor_get_value_address(grid_type, buf, acc, hdda.hdda.ijk);
                uint64_t val_raw = pnanovdb_read_uint64(buf, addr);
                uint skip_level = (uint(val_raw) >> 16u) & 0xFFFF;

                // align HDDA tmin/tmax to current voxel size
                dim = 1 << skip_level;

                s_voxel_val[thread_idx] = val_raw;
            }
            else
            {
                s_voxel_val[thread_idx] = ~uint64_t(0u);
            }
            workgroup_hdda_update(hdda, thread_idx, rayOrigin, rayDir, rayDirInv, dim);
        }

        uint current_hdda_idx = workgroup_hdda_vote(hdda, thread_idx);
        // cooperatively process min_voxel_val
        if (current_hdda_idx != ~0u)
        {
            uint64_t voxel_val_raw = s_voxel_val[current_hdda_idx];
            bool this_thread_active = workgroup_this_thread_active(hdda, thread_idx);
            workgroup_triangle_process(
                thread_idx, this_thread_active,
                grid,
                worldRayOrigin, worldRayDir, worldRayDirInv,
                max(boxMinT, hdda.hdda.tmin) * rayDirMagnInv,
                min(boxMaxT, hdda.hdda.tmax) * rayDirMagnInv,
                voxel_val_raw,
                sum, nominalT
            );
        }
        if (sum.a <= 0.0001f)
        {
            workgroup_mark_done(hdda, thread_idx);
        }
        should_run = workgroup_hdda_step(hdda, thread_idx, rayOrigin, rayDir, rayDirInv);
    }
}

void get_center_ray_dir(out float3 rayDir)
{
    float2 ndc = float2(0.f, 0.f);
    float4 pos_d0 = mul(float4(ndc.xy, 0.f, 1.f), editor_params.projection_inv);
    float4 pos_d1 = mul(float4(ndc.xy, 1.f, 1.f), editor_params.projection_inv);

    float z_d0 = pos_d0.z * (1.f / pos_d0.w);
    float z_d1 = pos_d1.z * (1.f / pos_d1.w);
    bool is_reverse_z = abs(z_d0) > abs(z_d1);
    float4 ray_dir_near = is_reverse_z ? pos_d1 : pos_d0;

    float4 ray_dir_far = ray_dir_near + mul(float4(0.f, 0.f, 1.f, 0.f), editor_params.projection_inv);
    rayDir = normalize((ray_dir_far.xyz / ray_dir_far.w) - (ray_dir_near.xyz / ray_dir_near.w));
    if (is_reverse_z)
    {
        rayDir = -rayDir;
    }

    rayDir = mul(float4(rayDir, 0.f), editor_params.view_inv).xyz;
    rayDir = normalize(rayDir);
}

float4 sample_subpixel(float2 tidxf, uint thread_idx1d)
{
    float2 ndc = float2(2.f * (tidxf.x / float(editor_params.width)) - 1.f,
                        -2.f * (tidxf.y / float(editor_params.height)) + 1.f);

    float4 pos_d0 = mul(float4(ndc.xy, 0.f, 1.f), editor_params.projection_inv);
    float4 pos_d1 = mul(float4(ndc.xy, 1.f, 1.f), editor_params.projection_inv);

    float z_d0 = pos_d0.z * (1.f / pos_d0.w);
    float z_d1 = pos_d1.z * (1.f / pos_d1.w);
    bool is_reverse_z = abs(z_d0) > abs(z_d1);
    float4 ray_dir_near = is_reverse_z ? pos_d1 : pos_d0;

    float4 ray_dir_far = ray_dir_near + mul(float4(0.f, 0.f, 1.f, 0.f), editor_params.projection_inv);
    float3 rayDir = normalize((ray_dir_far.xyz / ray_dir_far.w) - (ray_dir_near.xyz / ray_dir_near.w));
    if (is_reverse_z)
    {
        rayDir = -rayDir;
    }

    rayDir = mul(float4(rayDir, 0.f), editor_params.view_inv).xyz;
    float3 rayDirInv = float3(1.f, 1.f, 1.f) / rayDir;

    float4 rayOrigin4 = is_reverse_z ? pos_d1 : pos_d0;
    rayOrigin4 = mul(rayOrigin4, editor_params.view_inv);
    float3 rayOrigin = rayOrigin4.xyz / rayOrigin4.w;

    float3 rayDirCenter;
    get_center_ray_dir(rayDirCenter);

    pnanovdb_grid_handle_t grid = { pnanovdb_address_null() };
    pnanovdb_uint32_t grid_type = pnanovdb_grid_get_grid_type(buf, grid);

    float4 sum = float4(0.f, 0.f, 0.f, 1.f);
    float nominalT = 0.f;
    ray_hdda_nanovdb(thread_idx1d, buf, rayOrigin, 0.f, rayDir, 1e9f, rayDirInv, rayDirCenter, sum, nominalT);

    return sum;
}

[shader("compute")][numthreads(16, 8, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID, uint3 thread_idx : SV_GroupThreadID)
{
    int2 tidx = int2(dispatchThreadID.xy);
    uint thread_idx1d = thread_idx.x | (thread_idx.y << 4);

    float4 sum = float4(0.f, 0.f, 0.f, 0.f);
    if (shader_params.supersample != 0u)
    {
        static const int2 pattern[4u] = {int2(-2, -6), int2(6, -2), int2(-6, 2), int2(2, 6)};
        for (uint sample_idx = 0u; sample_idx < 4u; sample_idx++)
        {
            float2 tidxf = float2(tidx) + 0.5f + (1.f / 16.f) * pattern[sample_idx];

            float4 sample_sum = sample_subpixel(tidxf, thread_idx1d);
            sum.rgb += (1.f / 4.f) * sample_sum.rgb;
            sum.a = max(sum.a, sample_sum.a);
        }
    }
    else
    {
        float2 tidxf = float2(tidx) + 0.5f;
        sum = sample_subpixel(tidxf, thread_idx1d);
    }

    texture_out[tidx] = sum;
}
