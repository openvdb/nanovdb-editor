// voxelbvh_triangles.slang
#define PNANOVDB_HLSL
#define PNANOVDB_ADDRESS_64
#define PNANOVDB_BUF_HLSL_64
#include "PNanoVDB.h"
#include "PNanoVDBExt.h"

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
    uint pad1;
    uint pad2;
};

struct shader_params_t
{
    uint mode;
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
        pnanovdb_address_t color_addr = pnanovdb_grid_get_gridblindmetadata_value_address(buf, grid, 4u);
        float3 color_a = pnanovdb_read_vec3(buf, pnanovdb_address_offset_product(color_addr, 12u, id_a));
        float3 color_b = pnanovdb_read_vec3(buf, pnanovdb_address_offset_product(color_addr, 12u, id_b));
        float3 color_c = pnanovdb_read_vec3(buf, pnanovdb_address_offset_product(color_addr, 12u, id_c));

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

void ray_hdda_nanovdb(uint thread_idx,
                       StructuredBuffer<uint2> buf,
                       float3 worldRayOrigin,
                       float rayMinT,
                       float3 worldRayDir,
                       float rayMaxT,
                       float3 worldRayDirInv,
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
                hdda.hdda.tmin * rayDirMagnInv, hdda.hdda.tmax * rayDirMagnInv,
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

    float2 ndc = float2(2.f * ((float(tidx.x) + 0.5f) / float(editor_params.width)) - 1.f,
                        -2.f * ((float(tidx.y) + 0.5f) / float(editor_params.height)) + 1.f);

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

    pnanovdb_grid_handle_t grid = { pnanovdb_address_null() };
    pnanovdb_uint32_t grid_type = pnanovdb_grid_get_grid_type(buf, grid);

    float4 sum = float4(0.f, 0.f, 0.f, 1.f);
    float nominalT = 0.f;
    ray_hdda_nanovdb(thread_idx1d, buf, rayOrigin, 0.f, rayDir, 1e9f, rayDirInv, sum, nominalT);

    texture_out[tidx] = sum;
}
