// wireframe.slang
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
    uint highlight_bbox;
    uint auto_center;
};

StructuredBuffer<uint2> buf;
RWStructuredBuffer<uint> image_out;
RWTexture2D<float4> texture_out;
RWTexture2D<float> depth_out;
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

int3 ray_march_compute_final_location(float3 rayDir, int3 location, int3 locationMin, int3 locationMax)
{
    return int3(rayDir.x > 0.f ? max(location.x, locationMax.x) : min(location.x, locationMin.x - 1),
                rayDir.y > 0.f ? max(location.y, locationMax.y) : min(location.y, locationMin.y - 1),
                rayDir.z > 0.f ? max(location.z, locationMax.z) : min(location.z, locationMin.z - 1));
}

void ray_march_advance_ray(
    float3 blockSizeWorld, float3 rayDir, float3 rayDirInv, float3 rayOrigin, inout int3 location, inout float hitT)
{
    float hitTx = (float(location.x + (rayDir.x > 0.f ? +1 : 0)) * blockSizeWorld.x - rayOrigin.x) * rayDirInv.x;
    float hitTy = (float(location.y + (rayDir.y > 0.f ? +1 : 0)) * blockSizeWorld.y - rayOrigin.y) * rayDirInv.y;
    float hitTz = (float(location.z + (rayDir.z > 0.f ? +1 : 0)) * blockSizeWorld.z - rayOrigin.z) * rayDirInv.z;

    if (rayDir.x != 0.f && (hitTx <= hitTy || rayDir.y == 0.f) && (hitTx <= hitTz || rayDir.z == 0.f))
    {
        hitT = hitTx;
        location.x += rayDir.x > 0.f ? +1 : -1;
    }
    else if (rayDir.y != 0.f && (hitTy <= hitTx || rayDir.x == 0.f) && (hitTy <= hitTz || rayDir.z == 0.f))
    {
        hitT = hitTy;
        location.y += rayDir.y > 0.f ? +1 : -1;
    }
    else
    {
        hitT = hitTz;
        location.z += rayDir.z > 0.f ? +1 : -1;
    }
}

void ray_march_bbox(float3 rayOrigin,
                    float rayMinT,
                    float3 rayDir,
                    float rayMaxT,
                    float3 rayDirInv,
                    float dx_origin,
                    float dx_per_T,
                    int3 bbox_min,
                    int3 bbox_max,
                    inout float4 sum,
                    inout float nominalT)
{
    // intersect local ray with local bbox
    float3 bbox_minf = float3(bbox_min);
    float3 bbox_maxf = float3(bbox_max + int3(1, 1, 1));

    float boxMinT;
    float boxMaxT;
    bool isHit = intersect_box(rayDir, rayDirInv, bbox_minf - rayOrigin, bbox_maxf - rayOrigin, boxMinT, boxMaxT);
    if (isHit)
    {
        float3 nearPos = rayOrigin + boxMinT * rayDir;
        float3 farPos = rayOrigin + boxMaxT * rayDir;

        float3 nearDiff = nearPos - 0.5f * (bbox_maxf + bbox_minf);
        float3 farDiff = farPos - 0.5f * (bbox_maxf + bbox_minf);

        int faceId = -1;
        if (abs(nearDiff.x) > abs(nearDiff.y) && abs(nearDiff.x) > abs(nearDiff.z))
        {
            faceId = nearDiff.x >= 0.f ? 0 : 3;
        }
        else if (abs(nearDiff.y) > abs(nearDiff.x) && abs(nearDiff.y) > abs(nearDiff.z))
        {
            faceId = nearDiff.y >= 0.f ? 1 : 4;
        }
        else // if (abs(nearDiff.z) >= abs(nearDiff.x) && abs(nearDiff.z) >= abs(nearDiff.y))
        {
            faceId = nearDiff.z >= 0.f ? 2 : 5;
        }

        float3 nearMin = min(nearPos - bbox_minf, bbox_maxf - nearPos);
        float3 farMin = min(farPos - bbox_minf, bbox_maxf - farPos);

        float thresholdMin = 4.f * (dx_origin + boxMinT * dx_per_T);
        float thresholdMax = 4.f * (dx_origin + boxMaxT * dx_per_T);

        int nearCount = 0;
        if (nearMin.x < thresholdMin)
        {
            nearCount++;
        }
        if (nearMin.y < thresholdMin)
        {
            nearCount++;
        }
        if (nearMin.z < thresholdMin)
        {
            nearCount++;
        }

        int farCount = 0;
        if (farMin.x < thresholdMax)
        {
            farCount++;
        }
        if (farMin.y < thresholdMax)
        {
            farCount++;
        }
        if (farMin.z < thresholdMax)
        {
            farCount++;
        }

        float3 baseColor = float3(1.f, 1.f, 1.f);
        if (faceId == 0)
        {
            baseColor = float3(1.f, 0.f, 0.f);
        }
        else if (faceId == 1)
        {
            baseColor = float3(0.f, 1.f, 0.f);
        }
        else if (faceId == 2)
        {
            baseColor = float3(0.f, 0.f, 1.f);
        }
        else if (faceId == 3)
        {
            baseColor = float3(1.f, 0.f, 0.f);
        }
        else if (faceId == 4)
        {
            baseColor = float3(0.f, 1.f, 0.f);
        }
        else if (faceId == 5)
        {
            baseColor = float3(0.f, 0.f, 1.f);
        }

        float4 color = float4(0.f, 0.f, 0.f, 0.f);
        float finalT = 0.f;
        if (nearCount >= 2u)
        {
            color.rgb = 0.75f * baseColor;
            color.a = 1.f;
            finalT = boxMinT;
        }
        else if (farCount >= 2u)
        {
            color.rgb = 0.25f * baseColor;
            color.a = 1.f;
            finalT = boxMaxT;
        }

        nominalT = sum.a * (color.a * finalT) + nominalT;
        sum.rgb = sum.a * (color.a * color.rgb) + sum.rgb;
        sum.a = (1.f - color.a) * sum.a;
    }
}

void ray_march_nanovdb(StructuredBuffer<uint2> buf,
                       float3 worldRayOrigin,
                       float rayMinT,
                       float3 worldRayDir,
                       float rayMaxT,
                       float3 worldRayDirInv,
                       float dx_origin,
                       float dx_per_T,
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
    if (rayDirMagn > 0.f)
    {
        rayDir /= rayDirMagn;
        rayMinT *= rayDirMagn;
        rayMaxT *= rayDirMagn;
        dx_origin *= rayDirMagn;
    }
    float3 rayDirInv = float3(1.f, 1.f, 1.f) / rayDir;

    // intersect local ray with local bbox
    int3 bbox_min = pnanovdb_root_get_bbox_min(buf, root);
    int3 bbox_max = pnanovdb_root_get_bbox_max(buf, root);

    // auto centering
    int3 ijk_offset = int3(0, 0, 0);
    if (shader_params.auto_center != 0u)
    {
        int3 bbox_ave = ((bbox_max + bbox_min) >> 1u);
        ijk_offset = (bbox_ave & ~4095);
        bbox_min = bbox_min - ijk_offset;
        bbox_max = bbox_max - ijk_offset;
        rayOrigin = rayOrigin + float3(bbox_ave - ijk_offset);
    }

    float3 bbox_minf = float3(bbox_min);
    float3 bbox_maxf = float3(bbox_max + int3(1, 1, 1));

    float boxMinT;
    float boxMaxT;
    bool isHit = intersect_box(rayDir, rayDirInv, bbox_minf - rayOrigin, bbox_maxf - rayOrigin, boxMinT, boxMaxT);

    boxMinT = max(rayMinT, boxMinT);
    if (boxMinT > boxMaxT)
    {
        isHit = false;
    }

    if (isHit)
    {
        float3 rayLocation = rayDir * boxMinT + rayOrigin;
        int3 location = int3(floor(rayLocation * (1.f / 8.f)));

        int3 finalLocation = ray_march_compute_final_location(
            rayDir, location, int3(bbox_min >> 3u), int3(bbox_max >> 3u) + int3(1, 1, 1));

        bool hitMax = false;
        float blockHitT = boxMinT;

        while (location.x != finalLocation.x && location.y != finalLocation.y && location.z != finalLocation.z && !hitMax)
        {
            if (shader_params.highlight_bbox != 0u)
            {
                sum.g = max(0.1f, sum.g);
            }

            int3 ijk = int3(location.xyz << 3u) + ijk_offset;
            pnanovdb_readaccessor_get_value_address(grid_type, buf, PNANOVDB_REF(acc), ijk);
            // disable check for now, until specialized tile value support is added.
            if (!pnanovdb_address_is_null(acc.leaf.address))
            {
                ray_march_bbox(rayOrigin, rayMinT, rayDir, rayMaxT, rayDirInv, dx_origin, dx_per_T,
                               int3(location.xyz << 3u), int3(location.xyz << 3u) + int3(7, 7, 7), sum, nominalT);
            }
            if (sum.a < 0.00005f)
            {
                hitMax = true;
            }

            ray_march_advance_ray(float3(8.f, 8.f, 8.f), rayDir, rayDirInv, rayOrigin, location, blockHitT);
        }
    }

    // put nominalT back to world space
    nominalT /= rayDirMagn;
}

void ray_march_nanovdb_node2_intern(StructuredBuffer<uint2> buf,
                                    float3 worldRayOrigin,
                                    float rayMinT,
                                    float3 worldRayDir,
                                    float rayMaxT,
                                    float3 worldRayDirInv,
                                    float dx_origin,
                                    float dx_per_T,
                                    pnanovdb_grid_handle_t grid,
                                    pnanovdb_node2_handle_t root,
                                    inout float4 sum,
                                    inout float nominalT)
{
    pnanovdb_node2_accessor_t acc;
    pnanovdb_node2_accessor_init(PNANOVDB_REF(acc), root);

    // transform ray from world to index space
    float3 rayOrigin = pnanovdb_grid_world_to_indexf(buf, grid, worldRayOrigin);
    float3 rayDir = pnanovdb_grid_world_to_index_dirf(buf, grid, worldRayDir);
    float rayDirMagn = length(rayDir);
    if (rayDirMagn > 0.f)
    {
        rayDir /= rayDirMagn;
        rayMinT *= rayDirMagn;
        rayMaxT *= rayDirMagn;
        dx_origin *= rayDirMagn;
    }
    float3 rayDirInv = float3(1.f, 1.f, 1.f) / rayDir;

    // intersect local ray with local bbox
    pnanovdb_address_t bboxes = pnanovdb_grid_get_gridblindmetadata_value_address(buf, grid, 0u);
    pnanovdb_coord_t bbox_min = pnanovdb_read_coord(buf, pnanovdb_address_offset(bboxes, 0u));
    pnanovdb_coord_t bbox_max = pnanovdb_read_coord(buf, pnanovdb_address_offset(bboxes, 12u));

    // auto centering
    int3 ijk_offset = int3(0, 0, 0);
    if (shader_params.auto_center != 0u)
    {
        int3 bbox_ave = ((bbox_max + bbox_min) >> 1u);
        ijk_offset = (bbox_ave & ~4095);
        bbox_min = bbox_min - ijk_offset;
        bbox_max = bbox_max - ijk_offset;
        rayOrigin = rayOrigin + float3(bbox_ave - ijk_offset);
    }

    float3 bbox_minf = float3(bbox_min);
    float3 bbox_maxf = float3(bbox_max + int3(1, 1, 1));

    float boxMinT;
    float boxMaxT;
    bool isHit = intersect_box(rayDir, rayDirInv, bbox_minf - rayOrigin, bbox_maxf - rayOrigin, boxMinT, boxMaxT);

    boxMinT = max(rayMinT, boxMinT);
    if (boxMinT > boxMaxT)
    {
        isHit = false;
    }

    if (isHit)
    {
        float3 rayLocation = rayDir * boxMinT + rayOrigin;
        int3 location = int3(floor(rayLocation * (1.f / 8.f)));

        int3 finalLocation = ray_march_compute_final_location(
            rayDir, location, int3(bbox_min >> 3u), int3(bbox_max >> 3u) + int3(1, 1, 1));

        bool hitMax = false;
        float blockHitT = boxMinT;

        while (location.x != finalLocation.x && location.y != finalLocation.y && location.z != finalLocation.z && !hitMax)
        {
            if (shader_params.highlight_bbox != 0u)
            {
                sum.g = max(0.1f, sum.g);
            }

            pnanovdb_coord_t leaf_ijk = int3(location.xyz << 3u) + ijk_offset;
            pnanovdb_node2_handle_t node = {};
            pnanovdb_uint32_t node_type = 0u;
            pnanovdb_uint32_t node_n = 0u;
            pnanovdb_uint32_t level = 0u;
            pnanovdb_node2_accessor_find_node(buf, acc, node, node_type, node_n, level, leaf_ijk);
            if (node_type == PNANOVDB_NODE2_TYPE_LEAF)
            {
                ray_march_bbox(rayOrigin, rayMinT, rayDir, rayMaxT, rayDirInv, dx_origin, dx_per_T,
                               int3(location.xyz << 3u), int3(location.xyz << 3u) + int3(7, 7, 7), sum, nominalT);
            }
            if (sum.a < 0.00005f)
            {
                hitMax = true;
            }

            ray_march_advance_ray(float3(8.f, 8.f, 8.f), rayDir, rayDirInv, rayOrigin, location, blockHitT);
        }
    }

    // put nominalT back to world space
    nominalT /= rayDirMagn;
}

void ray_march_nanovdb_node2(StructuredBuffer<uint2> buf,
                             float3 worldRayOrigin,
                             float rayMinT,
                             float3 worldRayDir,
                             float rayMaxT,
                             float3 worldRayDirInv,
                             float dx_origin,
                             float dx_per_T,
                             inout float4 sum,
                             inout float nominalT)
{
    pnanovdb_grid_handle_t grid = { pnanovdb_address_null() };
    pnanovdb_tree_handle_t tree = pnanovdb_grid_get_tree(buf, grid);
    pnanovdb_node2_handle_t root = { pnanovdb_tree_get_root(buf, tree).address.byte_offset >> 3u };

    ray_march_nanovdb_node2_intern(buf, worldRayOrigin, rayMinT, worldRayDir, rayMaxT, worldRayDirInv, dx_origin,
                                   dx_per_T, grid, root, sum, nominalT);
}

float4 mat_mul(float4 v, float4 x, float4 y, float4 z, float4 w)
{
    return float4(dot(v, x), dot(v, y), dot(v, z), dot(v, w));
}

void tidx_to_ray(int2 tidx, out float3 rayOrigin, out float3 rayDir, out float3 rayDirInv)
{
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
    rayDir = normalize((ray_dir_far.xyz / ray_dir_far.w) - (ray_dir_near.xyz / ray_dir_near.w));
    if (is_reverse_z)
    {
        rayDir = -rayDir;
    }

    rayDir = mat_mul(float4(rayDir, 0.f), editor_params.view_inv_x, editor_params.view_inv_y, editor_params.view_inv_z,
                     editor_params.view_inv_w)
                 .xyz;

    rayDir = normalize(rayDir);

    rayDirInv = float3(1.f, 1.f, 1.f) / rayDir;

    float4 rayOrigin4 = is_reverse_z ? pos_d1 : pos_d0;
    rayOrigin4 = mat_mul(rayOrigin4, editor_params.view_inv_x, editor_params.view_inv_y, editor_params.view_inv_z,
                         editor_params.view_inv_w);
    rayOrigin = rayOrigin4.xyz / rayOrigin4.w;
}

[shader("compute")][numthreads(32, 4, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    int2 tidx = int2(dispatchThreadID.xy);

    float3 rayOrigin;
    float3 rayDir;
    float3 rayDirInv;
    tidx_to_ray(tidx, rayOrigin, rayDir, rayDirInv);

    float3 rayOrigin_dx;
    float3 rayDir_dx;
    float3 rayDirInv_dx;
    tidx_to_ray(tidx ^ int2(128, 0), rayOrigin_dx, rayDir_dx, rayDirInv_dx);

    float dx_origin = (1.f / 128.f) * length(rayOrigin - rayOrigin_dx);
    float dx_per_T = (1.f / 128.f) * length((rayOrigin_dx + rayDir_dx) - (rayOrigin + rayDir)) - dx_origin;

    pnanovdb_grid_handle_t grid = { pnanovdb_address_null() };
    pnanovdb_uint32_t grid_type = pnanovdb_grid_get_grid_type(buf, grid);

    float4 sum = float4(0.f, 0.f, 0.f, 1.f);
    float nominalT = 0.f;
    if (grid_type == PNANOVDB_GRID_TYPE_NODE2)
    {
        ray_march_nanovdb_node2(buf, rayOrigin, 0.f, rayDir, 1e9f, rayDirInv, dx_origin, dx_per_T, sum, nominalT);
    }
    else
    {
        ray_march_nanovdb(buf, rayOrigin, 0.f, rayDir, 1e9f, rayDirInv, dx_origin, dx_per_T, sum, nominalT);
    }

    if (sum.a < 1.f)
    {
        nominalT *= (1.f / (1.f - sum.a));

        float3 world_pos = rayDir * nominalT + rayOrigin;
        float4 ndc = mat_mul(float4(world_pos, 1.f), editor_params.view_x, editor_params.view_y,
            editor_params.view_z, editor_params.view_w);
        ndc = mat_mul(ndc, editor_params.projection_x, editor_params.projection_y,
            editor_params.projection_z, editor_params.projection_w);
        if (ndc.w != 0.f)
        {
            ndc.z *= (1.f / ndc.w);
        }

        float cmp_depth = depth_out[tidx];
        if (cmp_depth < ndc.z)
        {
            depth_out[tidx] = ndc.z;
            texture_out[tidx] = sum;
        }
    }

    float4 value = sum;
    // Note: BGRA target
    uint rawValue = uint(255.f * max(0.f, min(1.f, value.z))) | (uint(255.f * max(0.f, min(1.f, value.y))) << 8u) |
                    (uint(255.f * max(0.f, min(1.f, value.x))) << 16u) |
                    (uint(255.f * max(0.f, min(1.f, value.w))) << 24u);
    image_out[(editor_params.height - 1 - tidx.y) * editor_params.width + tidx.x] = rawValue;
}
