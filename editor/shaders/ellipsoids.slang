// ellipsoids.slang
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

struct ShaderParams
{
    bool highlight_bbox;
    bool auto_center;
};

StructuredBuffer<uint2> buf;
RWStructuredBuffer<uint> image_out;
RWTexture2D<float4> texture_out;
ConstantBuffer<EditorParams> editor_params;
ConstantBuffer<ShaderParams> shader_params;

float3 cross_product(float3 a, float3 b)
{
    return float3(a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x);
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

float2 intersect_sphere(float3 rayOrigin, float3 rayDir)
{
    float b = dot(rayOrigin, rayDir);
    float c = dot(rayOrigin, rayOrigin) - 1.f;
    float h = b * b - c;
    if (h < 0.f)
    {
        return float2(-1.f, -1.f);
    }
    h = sqrt(h);
    return float2(-b - h, -b + h);
}

void intersect_gaussian(
    StructuredBuffer<uint2> buf,
    pnanovdb_grid_handle_t grid,
    pnanovdb_address_t means,
    pnanovdb_address_t quats,
    pnanovdb_address_t scales,
    pnanovdb_address_t shs,
    pnanovdb_address_t opacities,
    pnanovdb_uint32_t id,
    float3 rayOrigin,
    float tmin,
    float3 rayDir,
    float tmax,
    float3 rayDirInv,
    inout float4 sum,
    inout float nominalT)
{
    // compute distance to plane defined by means and raydir
    float3 meanf = pnanovdb_read_vec3(buf,
        pnanovdb_address_offset_product(means, id, 12u));

    float4 quatf;
    quatf.x = pnanovdb_read_float(buf,
        pnanovdb_address_offset_product(quats, 4u * id + 1u, 4u));
    quatf.y = pnanovdb_read_float(buf,
        pnanovdb_address_offset_product(quats, 4u * id + 2u, 4u));
    quatf.z = pnanovdb_read_float(buf,
        pnanovdb_address_offset_product(quats, 4u * id + 3u, 4u));
    quatf.w = pnanovdb_read_float(buf,
        pnanovdb_address_offset_product(quats, 4u * id + 0u, 4u));

    float3 scalef = pnanovdb_read_vec3(buf,
        pnanovdb_address_offset_product(scales, id, 12u));

    float opacity = pnanovdb_read_float(buf,
        pnanovdb_address_offset_product(opacities, 1u * id + 0u, 4u));

    float3 rot0_inv = float3(
        1.f - 2.f * (quatf.y * quatf.y + quatf.z * quatf.z),
        2.f * (quatf.x * quatf.y + quatf.z * quatf.w),
        2.f * (quatf.x * quatf.z - quatf.y * quatf.w));
    float3 rot1_inv = float3(
        2.f * (quatf.x * quatf.y - quatf.z * quatf.w),
        1.f - 2.f * (quatf.x * quatf.x + quatf.z * quatf.z),
        2.f * (quatf.y * quatf.z + quatf.x * quatf.w));
    float3 rot2_inv = float3(
        2.f * (quatf.x * quatf.z + quatf.y * quatf.w),
        2.f * (quatf.y * quatf.z - quatf.x * quatf.w),
        1.f - 2.f * (quatf.x * quatf.x + quatf.y * quatf.y));

    // translate/rotate ray into local space
    float3 localRayOrigin = float3(
        dot(rayOrigin - meanf, rot0_inv),
        dot(rayOrigin - meanf, rot1_inv),
        dot(rayOrigin - meanf, rot2_inv));
    float3 localRayDir = float3(
        dot(rayDir, rot0_inv),
        dot(rayDir, rot1_inv),
        dot(rayDir, rot2_inv));

    const float local_scale = sqrt(2.f * log(max(0.01f, opacity) / 0.01f));
    scalef *= local_scale;

    scalef = max(scalef, float3(0.001f));
    float3 scalef_inv = float3(1.f) / scalef;

    localRayOrigin *= scalef_inv;
    localRayDir *= scalef_inv;
    float localRayDirMagn = length(localRayDir);
    if (localRayDirMagn > 0.f)
    {
        localRayDir /= localRayDirMagn;
    }

    float2 t_range = intersect_sphere(localRayOrigin, localRayDir);
    float t = t_range.x / localRayDirMagn;

    //if (t > 0.f && t >= tmin && t < tmax)
    if (t > 0.f)
    {
        float3 sh = pnanovdb_read_vec3(buf,
            pnanovdb_address_offset_product(shs, id, 48u * 4u));

        float4 color = float4(
            float3(0.2820947917738781f * sh + 0.5f),
            opacity);

        //color.a = color.a * exp(-dot(pos, pos));
        color.a = 1.f;

        #if 1
        if (t < nominalT || nominalT == 0.f)
        {
            nominalT = t;
            sum.rgb = color.a * color.rgb;
            sum.a = 0.f;
        }
        #else
        nominalT = sum.a * (color.a * t) + nominalT;
        sum.rgb = sum.a * (color.a * color.rgb) + sum.rgb;
        sum.a = (1.f - color.a) * sum.a;
        #endif
    }
}

void ray_march_nanovdb(
    StructuredBuffer<uint2> buf,
    float3 worldRayOrigin,
    float rayMinT,
    float3 worldRayDir,
    float rayMaxT,
    float3 worldRayDirInv,
    float dx_origin,
    float dx_per_T,
    inout float4 sum,
    inout float nominalT
)
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
        dx_origin *= rayDirMagn;
    }
    float3 rayDirInv = float3(1.f, 1.f, 1.f) / rayDir;

    // intersect local ray with local bbox
    int3 bbox_min = pnanovdb_root_get_bbox_min(buf, root);
    int3 bbox_max = pnanovdb_root_get_bbox_max(buf, root);

    // auto centering
    int3 ijk_offset = int3(0, 0, 0);
    if (shader_params.auto_center)
    {
        int3 bbox_ave = ((bbox_max + bbox_min) >> 1u);
        ijk_offset = (bbox_ave & ~4095);
        bbox_min = bbox_min - ijk_offset;
        bbox_max = bbox_max - ijk_offset;
        rayOrigin = rayOrigin + float3(bbox_ave - ijk_offset);
    }

    float3 bbox_minf = float3(bbox_min);
    float3 bbox_maxf = float3(bbox_max + int3(1,1,1));

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
        pnanovdb_address_t headers = pnanovdb_grid_get_gridblindmetadata_value_address(buf, grid, 0u);
        pnanovdb_address_t ids = pnanovdb_grid_get_gridblindmetadata_value_address(buf, grid, 1u);
        pnanovdb_address_t means = pnanovdb_grid_get_gridblindmetadata_value_address(buf, grid, 2u);
        pnanovdb_address_t quats = pnanovdb_grid_get_gridblindmetadata_value_address(buf, grid, 3u);
        pnanovdb_address_t scales = pnanovdb_grid_get_gridblindmetadata_value_address(buf, grid, 4u);
        pnanovdb_address_t shs = pnanovdb_grid_get_gridblindmetadata_value_address(buf, grid, 5u);
        pnanovdb_address_t opacities = pnanovdb_grid_get_gridblindmetadata_value_address(buf, grid, 6u);

        float3 rayLocation = rayDir * boxMinT + rayOrigin;

        pnanovdb_hdda_t hdda;
        pnanovdb_hdda_init(hdda, rayOrigin, boxMinT, rayDir, boxMaxT, 1);

        pnanovdb_uint32_t current_header_idxs[16u];
        for (uint i = 0u; i < 16u; i++)
        {
            current_header_idxs[i] = 0u;
        }
        do
        {
            // can break if it's not possible for next ellipsoids to be closer
            if (nominalT > 0.f && nominalT < hdda.tmin * rayDirMagnInv)
            {
                break;
            }

            int dim = pnanovdb_readaccessor_get_dim(grid_type, buf, acc, hdda.voxel);
            if (hdda.dim != dim)
            {
                pnanovdb_hdda_update(hdda, rayOrigin, rayDir, dim);
            }
            dim = pnanovdb_readaccessor_get_dim(grid_type, buf, acc, hdda.voxel);
            if (hdda.dim != dim)
            {
                pnanovdb_hdda_update(hdda, rayOrigin, rayDir, dim);
            }
            dim = pnanovdb_readaccessor_get_dim(grid_type, buf, acc, hdda.voxel);
            if (hdda.dim != dim)
            {
                pnanovdb_hdda_update(hdda, rayOrigin, rayDir, dim);
            }

            float hdda_tmax = min(hdda.next.x, min(hdda.next.y, hdda.next.z));

            pnanovdb_address_t addr = pnanovdb_readaccessor_get_value_address(grid_type, buf, acc, hdda.voxel);
            pnanovdb_uint32_t gaussian_header_idx = pnanovdb_read_uint32(buf, addr);

            if (gaussian_header_idx != 0u)
            {
                pnanovdb_uint32_t parent_idx = ~0u;
                for (uint i = 0u; i < 16u && parent_idx != 0u; i++)
                {
                    pnanovdb_uint32_t begin_idx = pnanovdb_read_uint32(buf,
                        pnanovdb_address_offset_product(headers, 4u * gaussian_header_idx + 0u, 4u));
                    pnanovdb_uint32_t count = pnanovdb_read_uint32(buf,
                        pnanovdb_address_offset_product(headers, 4u * gaussian_header_idx + 1u, 4u));
                    parent_idx = pnanovdb_read_uint32(buf,
                        pnanovdb_address_offset_product(headers, 4u * gaussian_header_idx + 2u, 4u));
                    pnanovdb_uint32_t level = pnanovdb_read_uint32(buf,
                        pnanovdb_address_offset_product(headers, 4u * gaussian_header_idx + 3u, 4u));

                    if (current_header_idxs[level & 15] != gaussian_header_idx)
                    {
                        current_header_idxs[level & 15] = gaussian_header_idx;
                        // filter on level
                        if (level >= 4u)
                        {
                            count = 0u;
                        }
                        // process gaussian list
                        for (uint list_idx = 0u; list_idx < count; list_idx++)
                        {
                            uint ids_idx = begin_idx + list_idx;
                            pnanovdb_uint32_t id = pnanovdb_read_uint32(buf,
                                pnanovdb_address_offset_product(ids, ids_idx, 4u));

                            intersect_gaussian(
                                buf,
                                grid,
                                means,
                                quats,
                                scales,
                                shs,
                                opacities,
                                id,
                                worldRayOrigin,
                                hdda.tmin * rayDirMagnInv,
                                worldRayDir,
                                hdda_tmax * rayDirMagnInv,
                                worldRayDirInv,
                                sum,
                                nominalT
                            );
                        }
                    }
                    gaussian_header_idx = parent_idx;
                }
            }
        } while (pnanovdb_hdda_step(hdda));
    }
}

float4 mat_mul(float4 v, float4 x, float4 y, float4 z, float4 w)
{
    return float4(dot(v, x), dot(v, y), dot(v, z), dot(v, w));
}

void tidx_to_ray(int2 tidx, out float3 rayOrigin, out float3 rayDir, out float3 rayDirInv)
{
    float2 ndc = float2(
        2.f * ((float(tidx.x) + 0.5f) / float(editor_params.width)) - 1.f,
        -2.f * ((float(tidx.y) + 0.5f) / float(editor_params.height)) + 1.f
    );

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

    rayDirInv = float3(1.f, 1.f, 1.f) / rayDir;

    float4 rayOrigin4 = is_reverse_z ? pos_d1 : pos_d0;
    rayOrigin4 = mat_mul(rayOrigin4,
                         editor_params.view_inv_x, editor_params.view_inv_y,
                         editor_params.view_inv_z, editor_params.view_inv_w);
    rayOrigin = rayOrigin4.xyz / rayOrigin4.w;
}

[shader("compute")]
[numthreads(32, 4, 1)]
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
        // NOP
    }
    else
    {
        ray_march_nanovdb(
            buf,
            rayOrigin,
            0.f,
            rayDir,
            1e9f,
            rayDirInv,
            dx_origin,
            dx_per_T,
            sum,
            nominalT
        );
    }

    float4 value = sum;
    // Note: BGRA target
    uint rawValue = uint(255.f * max(0.f, min(1.f, value.z))) |
        (uint(255.f * max(0.f, min(1.f, value.y))) << 8u) |
        (uint(255.f * max(0.f, min(1.f, value.x))) << 16u) |
        (uint(255.f * max(0.f, min(1.f, value.w))) << 24u);
    image_out[(editor_params.height - 1 - tidx.y) * editor_params.width + tidx.x] = rawValue;

    texture_out[tidx] = value;
}
