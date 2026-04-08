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
    float alpha_scale;
    uint narrow_band_only;
    uint highlight_bbox;
    float slice_plane_thickness;

    float4 slice_plane;

    uint auto_center;
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
    float3 meanf,
    float4 quatf,
    float3 scalef,
    float3 sh0,
    float opacity,
    float3 rayOrigin,
    float tmin,
    float3 rayDir,
    float tmax,
    float3 rayDirInv,
    inout float4 sum,
    inout float nominalT)
{
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
        float4 color = float4(
            float3(0.2820947917738781f * sh0 + 0.5f),
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

void ray_march_nanovdb(StructuredBuffer<uint2> buf,
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
        hdda_t hdda;
        hdda_init(hdda, rayOrigin, rayDir, rayDirInv, boxMinT);
        hdda_update(hdda, rayOrigin, rayDir, rayDirInv, 1);

        do
        {
            pnanovdb_address_t addr = pnanovdb_readaccessor_get_value_address(grid_type, buf, acc, hdda.ijk);
            uint64_t val_raw = pnanovdb_read_uint64(buf, addr);
            uint active_level_mask = uint(val_raw) & 0xFFFF;
            uint skip_level = (uint(val_raw) >> 16u) & 0xFFFF;
            uint list_begin_idx = uint(val_raw >> 32u);

            // align HDDA tmin/tmax to current voxel size
            int dim = 1 << skip_level;
            if (dim > 1)
            {
                dim /= 2u;
            }
            hdda_update(hdda, rayOrigin, rayDir, rayDirInv, dim);

            // can break if it's not possible for next ellipsoids to be closer
            if (nominalT > 0.f && nominalT < hdda.tmin * rayDirMagnInv)
            {
                break;
            }

            // show only smallest three voxel sizes for now
            uint filter_mask = 0x7;

            float4 color = float4(0.f, 0.f, 0.f, 0.f);
            uint active_level_count = countbits(active_level_mask & filter_mask);
            for (uint active_level_idx = 0u; active_level_idx < active_level_count; active_level_idx++)
            {
                pnanovdb_address_t range_addr = pnanovdb_grid_get_gridblindmetadata_value_address(buf, grid, 0u);
                pnanovdb_uint64_t range = pnanovdb_read_uint64(buf, pnanovdb_address_offset_product(range_addr, 8u, list_begin_idx + active_level_idx));
                uint range_begin = uint(range);
                uint range_end = uint(range >> 32u);
                uint range_count = range_end - range_begin;
                for (uint range_idx = 0u; range_idx < range_count; range_idx++)
                {
                    pnanovdb_address_t prim_id_addr = pnanovdb_grid_get_gridblindmetadata_value_address(buf, grid, 1u);
                    pnanovdb_uint32_t prim_id = pnanovdb_read_uint32(buf, pnanovdb_address_offset_product(prim_id_addr, 4u, range_begin + range_idx));
#if 1
                    pnanovdb_address_t mean_addr = pnanovdb_grid_get_gridblindmetadata_value_address(buf, grid, 2u);
                    pnanovdb_vec3_t meanf = pnanovdb_read_vec3(buf, pnanovdb_address_offset_product(mean_addr, 12u, prim_id));

                    pnanovdb_address_t quat_addr = pnanovdb_grid_get_gridblindmetadata_value_address(buf, grid, 4u);
                    float4 quatf = float4(
                        pnanovdb_read_float(buf, pnanovdb_address_offset_product(quat_addr, 4u, 4u * prim_id + 1u)),
                        pnanovdb_read_float(buf, pnanovdb_address_offset_product(quat_addr, 4u, 4u * prim_id + 2u)),
                        pnanovdb_read_float(buf, pnanovdb_address_offset_product(quat_addr, 4u, 4u * prim_id + 3u)),
                        pnanovdb_read_float(buf, pnanovdb_address_offset_product(quat_addr, 4u, 4u * prim_id + 0u))
                    );

                    pnanovdb_address_t scale_addr = pnanovdb_grid_get_gridblindmetadata_value_address(buf, grid, 5u);
                    pnanovdb_vec3_t scalef = pnanovdb_read_vec3(buf, pnanovdb_address_offset_product(scale_addr, 12u, prim_id));
#endif
                    pnanovdb_address_t sh0_addr = pnanovdb_grid_get_gridblindmetadata_value_address(buf, grid, 6u);
                    pnanovdb_vec3_t sh0 = pnanovdb_read_vec3(buf, pnanovdb_address_offset_product(sh0_addr, 12u, prim_id));

                    pnanovdb_address_t opacity_addr = pnanovdb_grid_get_gridblindmetadata_value_address(buf, grid, 3u);
                    float opacity = pnanovdb_read_float(buf, pnanovdb_address_offset_product(opacity_addr, 4u, prim_id));

#if 0
                    color.rgb = 0.2820947917738781f * sh0 + 0.5f;
                    color.a = opacity;

                    float currentT = hdda.tmin;

                    color = max(float4(0.f, 0.f, 0.f, 0.f), color);
                    color.a = min(1.f, color.a);
                    color.a *= shader_params.alpha_scale;

                    nominalT = sum.a * (color.a * currentT) + nominalT;
                    sum.rgb = sum.a * (color.a * color.rgb) + sum.rgb;
                    sum.a = (1.f - color.a) * sum.a;
#else
                    intersect_gaussian(
                        meanf,
                        quatf,
                        scalef,
                        sh0,
                        opacity,
                        worldRayOrigin,
                        hdda.tmin * rayDirMagnInv,
                        worldRayDir,
                        hdda.tmax * rayDirMagnInv,
                        worldRayDirInv,
                        sum,
                        nominalT
                    );
#endif
                }
            }

            hdda_step(hdda, rayOrigin, rayDir, rayDirInv, dim);

        } while (hdda.tmin < boxMaxT);// && sum.a > 0.0001f);
    }
}

float4 mat_mul(float4 v, float4 x, float4 y, float4 z, float4 w)
{
    return float4(dot(v, x), dot(v, y), dot(v, z), dot(v, w));
}

[shader("compute")][numthreads(32, 4, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    int2 tidx = int2(dispatchThreadID.xy);

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

    pnanovdb_grid_handle_t grid = { pnanovdb_address_null() };
    pnanovdb_uint32_t grid_type = pnanovdb_grid_get_grid_type(buf, grid);

    float4 sum = float4(0.f, 0.f, 0.f, 1.f);
    float nominalT = 0.f;
    ray_march_nanovdb(buf, rayOrigin, 0.f, rayDir, 1e9f, rayDirInv, sum, nominalT);

    float4 value = sum;
    // Note: BGRA target
    uint rawValue = uint(255.f * max(0.f, min(1.f, value.z))) | (uint(255.f * max(0.f, min(1.f, value.y))) << 8u) |
                    (uint(255.f * max(0.f, min(1.f, value.x))) << 16u) |
                    (uint(255.f * max(0.f, min(1.f, value.w))) << 24u);
    image_out[(editor_params.height - 1 - tidx.y) * editor_params.width + tidx.x] = rawValue;

    texture_out[tidx] = value;
}
