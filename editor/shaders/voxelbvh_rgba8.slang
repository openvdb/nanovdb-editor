// voxelbvh_triangles.slang
#define PNANOVDB_HLSL
#define PNANOVDB_ADDRESS_64
#define PNANOVDB_BUF_HLSL_64
#include "PNanoVDB.h"

#include "hdda.slang"

#include "editor_params.slang"

struct shader_params_t
{
    uint mode;
    int grid_idx;
};

StructuredBuffer<uint2> buf;
RWStructuredBuffer<uint> image_out_deprecated;
RWTexture2D<float4> texture_out;
ConstantBuffer<EditorParams> editor_params;
ConstantBuffer<shader_params_t> shader_params;

#if 0
static const uint vert_count = 12u;
static const float g = (1.f + sqrt(5.f)) / 2.f;
static const float3 verts[12u] = {
    {0.f, -1.f, -g},
    {0.f, 1.f, -g},
    {0.f, -1.f, g},
    {0.f, 1.f, g},
    {-1.f, -g, 0.f},
    {1.f, -g, 0.f},
    {-1.f, g, 0.f},
    {1.f, g, 0.f},
    {-g, 0.f, -1.f},
    {-g, 0.f, 1.f},
    {g, 0.f, -1.f},
    {g, 0.f, 1.f}
};
static const float sphere_radius = sqrt(g*g + 1.f);
#else
static const uint vert_count = 8u;
static const pnanovdb_vec3_t verts[8u] = {
    {-1.f, -1.f, 0.f},
    {1.f, -1.f, 0.f},
    {-1.f, 1.f, 0.f},
    {1.f, 1.f, 0.f},
    {-1.f, 0.f, 0.f},
    {0.f, -1.f, 0.f},
    {1.f, 0.f, 0.f},
    {0.f, 1.f, 0.f}
};
#endif

void ray_hdda_nanovdb(uint thread_idx,
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

    int target_grid_idx = shader_params.grid_idx;
    if (target_grid_idx < 0)
    {
        float max_dot = -1.f;
        target_grid_idx = 0;
        for (uint vert_idx = 0u; vert_idx < vert_count; vert_idx++)
        {
            float cur_dot = dot(worldRayDirCenter, normalize(verts[vert_idx]));
            if (cur_dot >= max_dot)
            {
                max_dot = cur_dot;
                target_grid_idx = int(vert_idx);
            }
        }
    }
    uint grid_count = pnanovdb_grid_get_grid_count(buf, grid);
    if (target_grid_idx >= grid_count)
    {
        target_grid_idx = grid_count == 0u ? 0 : int(grid_count) - 1;
    }
    for (int grid_idx = 0; grid_idx < target_grid_idx && grid_idx < vert_count; grid_idx++)
    {
        grid.address.byte_offset += pnanovdb_grid_get_grid_size(buf, grid);
    }

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

    hdda_t hdda = {};
    hdda_init(hdda, rayOrigin, rayDir, rayDirInv, boxMinT);

    bool should_run = isHit;
    while(should_run)
    {
        uint level = 0u;
        pnanovdb_address_t addr = pnanovdb_readaccessor_get_value_address_and_level(grid_type, buf, acc, hdda.ijk, level);

        uint hdda_dim = 1u;
        if (level == 1u) {hdda_dim = 8u;}
        if (level == 2u) {hdda_dim = 128u;}
        if (level == 3u) {hdda_dim = 4096u;}
        hdda_update(hdda, rayOrigin, rayDir, rayDirInv, hdda_dim);

        pnanovdb_uint32_t value_raw = pnanovdb_read_uint32(buf, addr);
        float4 color = float4(
            float((value_raw >> 0) & 255) * (1.f / 255.f), float((value_raw >> 8) & 255) * (1.f / 255.f),
            float((value_raw >> 16) & 255) * (1.f / 255.f), float((value_raw >> 24) & 255) * (1.f / 255.f));

        // path length correction
        if (color.a < 1.f)
        {
            color.a = 1.f - pow(1.f - color.a, (hdda.tmax - hdda.tmin) / float(hdda_dim));
        }

        color = max(float4(0.f, 0.f, 0.f, 0.f), color);
        color.a = min(1.f, color.a);

        nominalT = sum.a * (color.a * hdda.tmin) + nominalT;
        sum.rgb = sum.a * (color.a * color.rgb) + sum.rgb;
        sum.a = (1.f - color.a) * sum.a;
        if (sum.a <= 0.0001f)
        {
            should_run = false;
        }

        hdda_step(hdda, rayOrigin, rayDir, rayDirInv, hdda_dim);
        if (hdda.tmin > boxMaxT)
        {
            should_run = false;
        }
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

[shader("compute")][numthreads(16, 8, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID, uint3 thread_idx : SV_GroupThreadID)
{
    int2 tidx = int2(dispatchThreadID.xy);
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

    float3 rayDirCenter;
    get_center_ray_dir(rayDirCenter);

    float4 sum = float4(0.f, 0.f, 0.f, 1.f);
    float nominalT = 0.f;
    ray_hdda_nanovdb(thread_idx1d, buf, rayOrigin, 0.f, rayDir, 1e9f, rayDirInv, rayDirCenter, sum, nominalT);

    texture_out[tidx] = sum;
}
