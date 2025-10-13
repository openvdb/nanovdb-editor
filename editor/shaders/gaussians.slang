// gaussians.slang
#define PNANOVDB_HLSL
#define PNANOVDB_ADDRESS_64
#define PNANOVDB_BUF_HLSL_64
#include "PNanoVDB.h"
#include "PNanoVDBExt.h"

#define RAY_TRACE_TEST 0

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

// source: https://www.reedbeta.com/blog/hash-functions-for-gpu-rendering/
uint ray_march_hash(uint inputValue)
{
    uint state = inputValue * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float ray_march_rand_norm(uint inputValue)
{
    return float(ray_march_hash(inputValue) & 0xFFFF) * float(1.f / 65535.f);
}

float ray_march_noise_from_dir(float3 rayDir)
{
    float2 uv;
    if (abs(rayDir.x) > abs(rayDir.y) && abs(rayDir.x) > abs(rayDir.z))
    {
        uv = rayDir.yz;
    }
    else if (abs(rayDir.y) > abs(rayDir.x) && abs(rayDir.y) > abs(rayDir.z))
    {
        uv = rayDir.xz;
    }
    else //if (abs(rayDir.z) > abs(rayDir.x) && abs(rayDir.z) > abs(rayDir.y))
    {
        uv = rayDir.xy;
    }
    float maxAxis = max(abs(rayDir.x), max(abs(rayDir.y), abs(rayDir.z)));
    if (maxAxis > 0.f)
    {
        uv *= (1.f / maxAxis);
    }
    uv = 0.5f * uv + 0.5f;
    uint hashInput = uint(65535.f * uv.x) ^ (uint(65535.f * uv.y) << 16u);
    return ray_march_rand_norm(hashInput);
}

float4 mat_mul(float4 v, float4 x, float4 y, float4 z, float4 w)
{
    return float4(dot(v, x), dot(v, y), dot(v, z), dot(v, w));
}

float3 cross_product(float3 a, float3 b)
{
    return float3(a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x);
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

void intersect_gaussian_2d(
    StructuredBuffer<uint2> buf,
    pnanovdb_grid_handle_t grid,
    pnanovdb_address_t means,
    pnanovdb_address_t opacities,
    pnanovdb_address_t conics,
    pnanovdb_address_t colors,
    pnanovdb_uint32_t id,
    float4x4 view,
    float3 rayOrigin,
    float tmin,
    float3 rayDir,
    float tmax,
    float3 rayDirInv,
    inout float4 sum,
    inout float nominalT)
{
#if 1
    float3 conic;
    float3 color;
    conic.x = pnanovdb_read_float(buf, pnanovdb_address_offset_product(conics, 3u * id + 0u, 4u));
    conic.y = pnanovdb_read_float(buf, pnanovdb_address_offset_product(conics, 3u * id + 1u, 4u));
    conic.z = pnanovdb_read_float(buf, pnanovdb_address_offset_product(conics, 3u * id + 2u, 4u));
    color.x = pnanovdb_read_float(buf, pnanovdb_address_offset_product(colors, 3u * id + 0u, 4u));
    color.y = pnanovdb_read_float(buf, pnanovdb_address_offset_product(colors, 3u * id + 1u, 4u));
    color.z = pnanovdb_read_float(buf, pnanovdb_address_offset_product(colors, 3u * id + 2u, 4u));

    float opacity = pnanovdb_read_float(buf, pnanovdb_address_offset_product(opacities, 1u * id + 0u, 4u));

    float3 meanf;
    meanf.x = pnanovdb_read_float(buf, pnanovdb_address_offset_product(means, 3u * id + 0u, 4u));
    meanf.y = pnanovdb_read_float(buf, pnanovdb_address_offset_product(means, 3u * id + 1u, 4u));
    meanf.z = pnanovdb_read_float(buf, pnanovdb_address_offset_product(means, 3u * id + 2u, 4u));

    // put mean and ray into view space
    float4 mean_c = mul(float4(meanf, 1.f), view);
    float4 rayOrigin_c = mul(float4(rayOrigin, 1.f), view);
    float4 rayDir_c = mul(float4(rayDir, 0.f), view);

    rayDir_c.xyz = normalize(rayDir_c.xyz);

    // find t to intersect on z plane
    float t = abs(rayDir_c.z != 0.f ? (mean_c.z - rayOrigin_c.z) / rayDir_c.z : 0.f);

    // apply t to move xy
    float2 hit_2d = rayDir_c.xy * t + rayOrigin_c.xy;

    float2 delta = (hit_2d.xy - mean_c.xy);

    float sigma = 0.5f * (conic.x * delta.x * delta.x + conic.z * delta.y * delta.y) +
                    conic.y * delta.x * delta.y;
    float alpha = min(0.999f, opacity * exp(-sigma));
    if (!(sigma < 0.f || alpha < 1.f / 255.f))
    {
        nominalT = sum.a * (alpha * t) + nominalT;
        sum.rgb = sum.a * (alpha * color) + sum.rgb;
        sum.a = (1.f - alpha) * sum.a;
    }
#else
    sum.g += 0.0005f;
#endif
}

uint float_to_sort_key(float v)
{
    uint raw = pnanovdb_float_as_uint32(v);
    uint mask = -int(raw >> 31) | 0x80000000;
    return raw ^ mask;
}

void ray_march_nanovdb(
    StructuredBuffer<uint2> buf,
    bool is_primary,
    float3 worldRayOrigin,
    float rayMinT,
    float3 worldRayDir,
    float rayMaxT,
    float3 worldRayDirInv,
    float dx_origin,
    float3 rayDir_center,
    float dx_per_T,
    int2 tidx,
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
        pnanovdb_address_t means = pnanovdb_grid_get_gridblindmetadata_value_address(buf, grid, 2u);
        pnanovdb_address_t opacities = pnanovdb_grid_get_gridblindmetadata_value_address(buf, grid, 6u);
        pnanovdb_address_t colors = pnanovdb_grid_get_gridblindmetadata_value_address(buf, grid, 10u);

        pnanovdb_address_t ids = {};
        pnanovdb_address_t addr_sort_keys = {};
        pnanovdb_address_t conics = {};

        float4x4 view;
        if (is_primary)
        {
            view = float4x4(
                editor_params.view_x.x, editor_params.view_y.x, editor_params.view_z.x, editor_params.view_w.x,
                editor_params.view_x.y, editor_params.view_y.y, editor_params.view_z.y, editor_params.view_w.y,
                editor_params.view_x.z, editor_params.view_y.z, editor_params.view_z.z, editor_params.view_w.z,
                editor_params.view_x.w, editor_params.view_y.w, editor_params.view_z.w, editor_params.view_w.w
            );
            ids = pnanovdb_grid_get_gridblindmetadata_value_address(buf, grid, 1u);
            addr_sort_keys = pnanovdb_grid_get_gridblindmetadata_value_address(buf, grid, 8u);
            conics = pnanovdb_grid_get_gridblindmetadata_value_address(buf, grid, 9u);
        }
        else
        {
            bool do_x = abs(rayDir.x) > abs(rayDir.y) && abs(rayDir.x) > abs(rayDir.z);
            bool do_y = abs(rayDir.y) > abs(rayDir.x) && abs(rayDir.y) > abs(rayDir.z);
            if (do_x)
            {
                static const float4x4 view_x = float4x4(
                    0.f, 0.f, 1.f, 0.f,
                    0.f, 1.f, 0.f, 0.f,
                    1.f, 0.f, 0.f, 0.f,
                    0.f, 0.f, 0.f, 1.f
                );
                view = view_x;
                rayDir_center = float3(1.f, 0.f, 0.f);
                ids = pnanovdb_grid_get_gridblindmetadata_value_address(buf, grid, 11u);
                addr_sort_keys = pnanovdb_grid_get_gridblindmetadata_value_address(buf, grid, 14u);
                conics = pnanovdb_grid_get_gridblindmetadata_value_address(buf, grid, 17u);
            }
            else if (do_y)
            {
                static const float4x4 view_y = float4x4(
                    1.f, 0.f, 0.f, 0.f,
                    0.f, 0.f, 1.f, 0.f,
                    0.f, 1.f, 0.f, 0.f,
                    0.f, 0.f, 0.f, 1.f
                );
                view = view_y;
                rayDir_center = float3(0.f, 1.f, 0.f);
                ids = pnanovdb_grid_get_gridblindmetadata_value_address(buf, grid, 12u);
                addr_sort_keys = pnanovdb_grid_get_gridblindmetadata_value_address(buf, grid, 15u);
                conics = pnanovdb_grid_get_gridblindmetadata_value_address(buf, grid, 18u);
            }
            else // (abs(rayDir.z) > abs(rayDir.x) && abs(rayDir.z) > abs(rayDir.y))
            {
                static const float4x4 view_z = float4x4(
                    1.f, 0.f, 0.f, 0.f,
                    0.f, 1.f, 0.f, 0.f,
                    0.f, 0.f, 1.f, 0.f,
                    0.f, 0.f, 0.f, 1.f
                );
                view = view_z;
                rayDir_center = float3(0.f, 0.f, 1.f);
                ids = pnanovdb_grid_get_gridblindmetadata_value_address(buf, grid, 13u);
                addr_sort_keys = pnanovdb_grid_get_gridblindmetadata_value_address(buf, grid, 16u);
                conics = pnanovdb_grid_get_gridblindmetadata_value_address(buf, grid, 19u);
            }
        }

        bool is_reverse = dot(rayDir, rayDir_center) < 0.f;

        hdda_t hdda;
        hdda_init(hdda, rayOrigin, rayDir, rayDirInv, boxMinT);
        hdda_update(hdda, rayOrigin, rayDir, rayDirInv, 1);

        static const uint max_levels = 12u;

        pnanovdb_uint32_t current_list_current_idxs[max_levels];
        pnanovdb_uint32_t current_list_end_idxs[max_levels];
        pnanovdb_uint32_t current_sort_key[max_levels];
        for (uint i = 0u; i < max_levels; i++)
        {
            current_list_current_idxs[i] = 0u;
            current_list_end_idxs[i] = 0u;
            current_sort_key[i] = is_reverse ? 0u : ~0u;
        }
        do
        {
            int dim = pnanovdb_readaccessor_get_dim(grid_type, buf, acc, hdda.ijk);

            hdda_update(hdda, rayOrigin, rayDir, rayDirInv, dim);

            // compute allowed sort keys for this array interval
            float3 world_max = worldRayDir * hdda.tmax * rayDirMagnInv + worldRayOrigin;
            float3 world_min = worldRayDir * hdda.tmin * rayDirMagnInv + worldRayOrigin;
            uint sort_key_max = float_to_sort_key(dot(world_max, rayDir_center));
            uint sort_key_min = float_to_sort_key(dot(world_min, rayDir_center));
            if (is_reverse)
            {
                uint tmp = sort_key_max;
                sort_key_max = sort_key_min;
                sort_key_min = tmp;
            }

            pnanovdb_address_t addr = pnanovdb_readaccessor_get_value_address(grid_type, buf, acc, hdda.ijk);
            pnanovdb_uint32_t gaussian_header_idx = pnanovdb_read_uint32(buf, addr);

            if (gaussian_header_idx != 0u)
            {
                for (uint level = 0u; level < max_levels; level += 2u)
                {
                    uint current_idx_a = pnanovdb_read_uint32(buf, pnanovdb_address_offset_product(headers, 18u * gaussian_header_idx + level + 0u, 4u));
                    uint current_idx_b = pnanovdb_read_uint32(buf, pnanovdb_address_offset_product(headers, 18u * gaussian_header_idx + level + 1u, 4u));
                    uint count_raw = pnanovdb_read_uint32(buf, pnanovdb_address_offset_product(headers, 18u * gaussian_header_idx + (level >> 1u) + 12u, 4u));
                    uint current_end_a = current_idx_a + (count_raw & 0xFFFF);
                    uint current_end_b = current_idx_b + (count_raw >> 16u);
                    if (!is_reverse)
                    {
                        if (current_list_end_idxs[level + 0u] != current_end_a)
                        {
                            current_list_current_idxs[level + 0u] = current_idx_a;
                            current_list_end_idxs[level + 0u] = current_end_a;
                        }
                        if (current_list_end_idxs[level + 1u] != current_end_b)
                        {
                            current_list_current_idxs[level + 1u] = current_idx_b;
                            current_list_end_idxs[level + 1u] = current_end_b;
                        }
                    }
                    else
                    {
                        if (current_list_current_idxs[level + 0u] != current_idx_a)
                        {
                            current_list_current_idxs[level + 0u] = current_idx_a;
                            current_list_end_idxs[level + 0u] = current_end_a - 1u;
                        }
                        if (current_list_current_idxs[level + 1u] != current_idx_b)
                        {
                            current_list_current_idxs[level + 1u] = current_idx_b;
                            current_list_end_idxs[level + 1u] = current_end_b - 1u;
                        }
                    }
                }
                for (uint level = 0u; level < max_levels; level++)
                {
                    uint sort_key = is_reverse ? 0u : ~0u;
                    if (!is_reverse)
                    {
                        if (current_list_current_idxs[level] < current_list_end_idxs[level])
                        {
                            uint current_idx = current_list_current_idxs[level];
                            sort_key = pnanovdb_read_uint32(buf, pnanovdb_address_offset_product(addr_sort_keys, current_idx, 4u));
                        }
                    }
                    else
                    {
                        if (current_list_end_idxs[level] >= current_list_current_idxs[level] && current_list_end_idxs[level] != ~0u)
                        {
                            uint current_idx = current_list_end_idxs[level];
                            sort_key = pnanovdb_read_uint32(buf, pnanovdb_address_offset_product(addr_sort_keys, current_idx, 4u));
                        }
                    }
                    current_sort_key[level] = sort_key;
                }

                // pick closest sort key
                uint closest_sort_key;
                uint closest_idx;
                uint closest_level;
                do
                {
                    closest_sort_key = is_reverse ? 0u : ~0u;
                    closest_idx = ~0u;
                    closest_level = ~0u;
                    for (uint level = 0u; level < max_levels; level++)
                    {
                        pnanovdb_uint32_t sort_key = current_sort_key[level];
                        if (!is_reverse)
                        {
                            if (sort_key < sort_key_max && sort_key < closest_sort_key)
                            {
                                closest_sort_key = sort_key;
                                closest_idx = current_list_current_idxs[level];
                                closest_level = level;
                            }
                        }
                        else
                        {
                            if (sort_key > sort_key_min && sort_key > closest_sort_key)
                            {
                                closest_sort_key = sort_key;
                                closest_idx = current_list_end_idxs[level];
                                closest_level = level;
                            }
                        }
                    }
                    if (closest_idx != ~0u)
                    {
                        uint sort_key = is_reverse ? 0u : ~0u;
                        if (!is_reverse)
                        {
                            // increment past processed Gaussian
                            current_list_current_idxs[closest_level]++;
                            uint current_idx = current_list_current_idxs[closest_level];
                            if (current_idx < current_list_end_idxs[closest_level])
                            {
                                sort_key = pnanovdb_read_uint32(buf, pnanovdb_address_offset_product(addr_sort_keys, current_idx, 4u));
                            }
                        }
                        else
                        {
                            // decrement past processed Gaussian
                            current_list_end_idxs[closest_level]--;
                            uint current_idx = current_list_end_idxs[closest_level];
                            if (current_idx >= current_list_current_idxs[closest_level] && current_idx != ~0u)
                            {
                                sort_key = pnanovdb_read_uint32(buf, pnanovdb_address_offset_product(addr_sort_keys, current_idx, 4u));
                            }
                        }
                        current_sort_key[closest_level] = sort_key;
                    }
                    if (closest_idx != ~0u && ((!is_reverse && closest_sort_key >= sort_key_min) || (is_reverse && closest_sort_key <= sort_key_max)))
                    {
                        pnanovdb_uint32_t id = pnanovdb_read_uint32(buf,
                            pnanovdb_address_offset_product(ids, closest_idx, 4u));

                        intersect_gaussian_2d(
                            buf,
                            grid,
                            means,
                            opacities,
                            conics,
                            colors,
                            id,
                            view,
                            worldRayOrigin,
                            hdda.tmin * rayDirMagnInv,
                            worldRayDir,
                            hdda.tmax * rayDirMagnInv,
                            worldRayDirInv,
                            sum,
                            nominalT
                        );
                    }
                } while(closest_idx != ~0u && sum.a > 0.0001f);
            }

            hdda_step(hdda, rayOrigin, rayDir, rayDirInv, dim);

        } while (hdda.tmin < boxMaxT && sum.a > 0.0001f);
    }
}

void tidx_to_ray(int2 tidx, out float3 rayOrigin, out float3 rayDir, out float3 rayDirInv)
{
    float2 ndc = float2(
        2.f * ((float(tidx.x) + 0.5f) / float(editor_params.width)) - 1.f,
        -2.f * ((float(tidx.y) + 0.5f) / float(editor_params.height)) + 1.f
    );

#if RAY_TRACE_TEST
    bool is_reverse_z = true;
    float4 pos_d0 = float4(0.f, 0.f, 0.f, 1.f);
    float4 pos_d1 = float4(0.f, 0.f, 0.f, 1.f);

    float2 off = ndc * float2(float(editor_params.width) / float(editor_params.height), 1.f);
    float dist2 = 1.f - off.x * off.x - off.y * off.y;
    if (dist2 > 0.f)
    {
        rayDir.z = -sqrt(dist2);
        rayDir.x = off.x;
        rayDir.y = off.y;
    }
    else
    {
        rayDir = float3(0.f, 0.f, -1.f);
        pos_d1 = float4(0.f, 0.f, -1000000.f, 1.f);
    }
    rayDir = normalize(rayDir);
    //rayDir.z = -1.f;
    //rayDir.x = sin(0.9f * 3.14f / 2.f * ndc.x);
    //rayDir.y = sin(0.9f * 3.14f / 2.f * ndc.y * float(editor_params.height) / float(editor_params.width));
#else
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
#endif

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

float dist_intersect(float3 rayOrigin, float3 rayDir, float3 center, float3 normal, float radius)
{
    float3 delta = rayOrigin - center;
    float t = -dot(normal, delta) / dot(rayDir, normal);
    float3 q = delta + rayDir * t;
    return (dot(q, q) < radius * radius) ? t : -1.f;
}

bool mirror_test(inout float3 rayOrigin, inout float3 rayDir, inout float3 rayDirInv, float test_t)
{
#if 0
    float3 disk_center = float3(0.5f, 0.5f, 0.5f);
    float3 disk_normal = normalize(float3(1.f, 1.f, 0.f));
    float disk_radius = 0.5f;

    float hit_t = dist_intersect(rayOrigin, rayDir, disk_center, disk_normal, disk_radius);
    float3 hit_normal = disk_normal;
#else
    float3 sphere_center = float3(0.5f, 0.f, 0.5f);
    float sphere_radius = 0.5f;

    float3 scale = float3(0.025f, 0.25f, 0.25f);
    float3 scale_inv = float3(1.f, 1.f, 1.f) / scale;

    // transform to local space
    float3 localRayOrigin = scale_inv * (rayOrigin - sphere_center);
    float3 localRayDir = scale_inv * rayDir;
    float localMagn = length(localRayDir);
    if (localMagn > 0.f)
    {
        localRayDir /= localMagn;
    }

    float2 hits = intersect_sphere(localRayOrigin, localRayDir);
    float hit_t = -1.f;
    float3 hit_normal = float3(0.f, 0.f, 1.f);
    if (hits.x > 0.f)
    {
        hit_t = hits.x / localMagn;
        hit_normal = -normalize(localRayDir * hits.x + localRayOrigin);
    }
#endif
    if (hit_t > 0.f && hit_t < test_t)
    {
        float3 reflect = rayDir - 2.f * dot(rayDir, hit_normal) * hit_normal;

        rayOrigin = rayDir * hit_t + rayOrigin;
        rayDir = normalize(reflect);
        rayDirInv = 1.f / rayDir;
    }
    return hit_t > 0.f && hit_t < test_t;
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

    float3 rayDir_center;
    get_center_ray_dir(rayDir_center);

    float dx_origin = (1.f / 128.f) * length(rayOrigin - rayOrigin_dx);
    float dx_per_T = (1.f / 128.f) * length((rayOrigin_dx + rayDir_dx) - (rayOrigin + rayDir)) - dx_origin;

    pnanovdb_grid_handle_t grid = { pnanovdb_address_null() };
    pnanovdb_uint32_t grid_type = pnanovdb_grid_get_grid_type(buf, grid);

    float noise_val = ray_march_noise_from_dir(rayDir);

    bool is_primary = true;
#if RAY_TRACE_TEST
    if (dot(rayDir, rayDir_center) < 0.75f + 0.1f * (2.f * noise_val - 1.f))
    {
        is_primary = false;
    }
#endif

    float4 sum = float4(0.f, 0.f, 0.f, 1.f);
    float nominalT = 0.f;
    ray_march_nanovdb(
        buf,
        is_primary,
        rayOrigin,
        0.f,
        rayDir,
        1e9f,
        rayDirInv,
        dx_origin,
        rayDir_center,
        dx_per_T,
        tidx,
        sum,
        nominalT
    );

#if RAY_TRACE_TEST
    float test_t = 1e9f;
    if (sum.a < 1.f)
    {
        test_t = nominalT / (1.f - sum.a);
    }

    if (mirror_test(rayOrigin, rayDir, rayDirInv, test_t))
    {
        sum = float4(0.f, 0.f, 0.f, 1.f);
        nominalT = 0.f;
        ray_march_nanovdb(
            buf,
            false,
            rayOrigin,
            0.f,
            rayDir,
            1e9f,
            rayDirInv,
            dx_origin,
            rayDir_center,
            dx_per_T,
            tidx,
            sum,
            nominalT
        );
    }
#endif

    float4 value = sum;
    // Note: BGRA target
    uint rawValue = uint(255.f * max(0.f, min(1.f, value.z))) |
        (uint(255.f * max(0.f, min(1.f, value.y))) << 8u) |
        (uint(255.f * max(0.f, min(1.f, value.x))) << 16u) |
        (uint(255.f * max(0.f, min(1.f, value.w))) << 24u);
    image_out[(editor_params.height - 1 - tidx.y) * editor_params.width + tidx.x] = rawValue;

    texture_out[tidx] = value;
}
