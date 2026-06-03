// editor_surface.slang
//
// SDF / level-set isosurface renderer.
//
// Finds the first zero-crossing of the signed scalar field along each camera
// ray using the PNanoVDB hierarchical DDA helpers (PNANOVDB_HDDA, enabled by
// default for HLSL in PNanoVDB.h), then shades the hit as an opaque surface.
// Supports FLOAT and ONINDEX (float blind-data) grids. Output uses the same
// premultiplied alpha-as-transmittance convention as editor.slang: alpha 0 ==
// opaque, alpha 1 == fully transparent (background shows through).
#define PNANOVDB_HLSL
#define PNANOVDB_ADDRESS_64
#define PNANOVDB_BUF_HLSL_64
#include "PNanoVDB.h"

#include "editor_params.slang"

struct shader_params_t
{
    float isovalue;     // iso level to extract (0 for level sets)
    float surface_eps;  // index-space bisection tolerance (<= 0 disables refinement)
    uint auto_center;    // recenter the traversal near the origin in index space (float precision for off-origin grids)
    float ambient;      // ambient lighting term

    float4 surface_color; // base albedo (rgb)
    float3 light_dir;     // world-space light direction; (0,0,0) -> headlight
    float _pad1;
};

StructuredBuffer<uint2> buf;
RWStructuredBuffer<uint> image_out_deprecated;
RWTexture2D<float4> texture_out;
ConstantBuffer<EditorParams> editor_params;
ConstantBuffer<shader_params_t> shader_params;

// Read the raw signed scalar value at an integer coordinate, generalized across
// grid types. This is the surface-finding analogue of editor.slang's
// ray_march_nanovdb_leaf_fetch_float, but returns the value instead of a color
// (and reinterprets ONINDEX blind float data with asfloat rather than asuint).
float read_distance(pnanovdb_grid_type_t grid_type, StructuredBuffer<uint2> buf, inout pnanovdb_readaccessor_t acc, int3 ijk)
{
    pnanovdb_grid_handle_t grid = { pnanovdb_address_null() };
    pnanovdb_uint32_t level = 0u;
    pnanovdb_address_t address =
        pnanovdb_readaccessor_get_value_address_and_level(grid_type, buf, PNANOVDB_REF(acc), ijk, level);

    float value = 0.f;
    if (grid_type == PNANOVDB_GRID_TYPE_ONINDEX)
    {
        pnanovdb_gridblindmetadata_handle_t metadata = pnanovdb_grid_get_gridblindmetadata(buf, grid, 0u);
        pnanovdb_uint64_t val_index64;
        if (level == 0)
        {
            val_index64 = pnanovdb_leaf_onindex_get_value_index(buf, address, ijk);
        }
        else
        {
            val_index64 = pnanovdb_read_uint64(buf, address);
        }
        pnanovdb_uint32_t val_index = pnanovdb_uint64_low(val_index64);
        pnanovdb_uint64_t value_count = pnanovdb_gridblindmetadata_get_value_count(buf, metadata);
        pnanovdb_uint32_t value_raw = 0u;
        if (val_index < pnanovdb_uint64_low(value_count))
        {
            pnanovdb_int64_t byte_offset = pnanovdb_gridblindmetadata_get_data_offset(buf, metadata);
            pnanovdb_address_t val_addr =
                pnanovdb_address_offset64(metadata.address, pnanovdb_int64_as_uint64(byte_offset));
            val_addr = pnanovdb_address_offset_product(val_addr, val_index, 4u);
            value_raw = pnanovdb_read_uint32(buf, val_addr);
        }
        value = asfloat(value_raw);
    }
    else if (grid_type == PNANOVDB_GRID_TYPE_FLOAT)
    {
        value = pnanovdb_read_float(buf, address);
    }
    else
    {
        // Mask / binary fallback: active voxel -> inside (-1), otherwise outside (+1).
        if (!pnanovdb_address_is_null(PNANOVDB_DEREF(acc).leaf.address))
        {
            pnanovdb_uint32_t leaf_n = pnanovdb_leaf_coord_to_offset(PNANOVDB_REF(ijk));
            value = pnanovdb_leaf_get_value_mask(buf, PNANOVDB_DEREF(acc).leaf, leaf_n) ? -1.f : 1.f;
        }
        else
        {
            value = 1.f;
        }
    }
    return value;
}

// Trilinearly interpolated distance at an arbitrary position expressed in
// recentered ("local") index space. Voxel values are treated as samples at
// integer (cell-center) coordinates, matching editor.slang's
// compute_trilinear_weights convention. ijk_offset (a multiple of 4096, see
// auto_center) is added back to recover the true coordinate for each lookup.
float read_distance_trilinear(pnanovdb_grid_type_t grid_type, StructuredBuffer<uint2> buf, inout pnanovdb_readaccessor_t acc, float3 pos, int3 ijk_offset)
{
    float3 p = pos - 0.5f;
    int3 base = int3(floor(p));
    float3 w = p - float3(base);
    int3 ijk000 = base + ijk_offset;

    float result = 0.f;
    [unroll]
    for (int i = 0; i < 8; i++)
    {
        int3 o = int3(i & 1, (i >> 1) & 1, (i >> 2) & 1);
        float wx = (o.x == 0) ? (1.f - w.x) : w.x;
        float wy = (o.y == 0) ? (1.f - w.y) : w.y;
        float wz = (o.z == 0) ? (1.f - w.z) : w.z;
        result += wx * wy * wz * read_distance(grid_type, buf, acc, ijk000 + o);
    }
    return result;
}

// Index-space surface normal = normalized gradient of the field via central
// differences of the trilinearly sampled distance. pos is in recentered index
// space; the offset is direction-invariant so the normal is unaffected by it.
float3 surface_normal(pnanovdb_grid_type_t grid_type, StructuredBuffer<uint2> buf, inout pnanovdb_readaccessor_t acc, float3 pos, int3 ijk_offset)
{
    const float h = 1.f;
    float dx = read_distance_trilinear(grid_type, buf, acc, pos + float3(h, 0.f, 0.f), ijk_offset) -
               read_distance_trilinear(grid_type, buf, acc, pos - float3(h, 0.f, 0.f), ijk_offset);
    float dy = read_distance_trilinear(grid_type, buf, acc, pos + float3(0.f, h, 0.f), ijk_offset) -
               read_distance_trilinear(grid_type, buf, acc, pos - float3(0.f, h, 0.f), ijk_offset);
    float dz = read_distance_trilinear(grid_type, buf, acc, pos + float3(0.f, 0.f, h), ijk_offset) -
               read_distance_trilinear(grid_type, buf, acc, pos - float3(0.f, 0.f, h), ijk_offset);
    float3 n = float3(dx, dy, dz);
    float len = length(n);
    return (len > 1e-8f) ? (n / len) : float3(0.f, 0.f, 1.f);
}

// Generalized HDDA zero-crossing, adapted from pnanovdb_hdda_zero_crossing in
// PNanoVDB.h. Differences: the scalar value read goes through read_distance (so
// FLOAT and ONINDEX both work), the test is against an arbitrary isovalue, and
// the hit t is refined between bracketing samples (linear, then optional
// bisection on the trilinear field).
bool surface_zero_crossing(pnanovdb_grid_type_t grid_type,
                           StructuredBuffer<uint2> buf,
                           inout pnanovdb_readaccessor_t acc,
                           float3 origin,
                           float tmin,
                           float3 direction,
                           float tmax,
                           float isovalue,
                           float eps,
                           int3 ijk_offset,
                           out float thit)
{
    thit = tmin;

    // origin/direction are in recentered ("local") index space; shift the true
    // root bbox by the same offset so the HDDA float math stays near the origin.
    pnanovdb_coord_t bbox_min = pnanovdb_root_get_bbox_min(buf, acc.root);
    pnanovdb_coord_t bbox_max = pnanovdb_root_get_bbox_max(buf, acc.root);
    float3 bbox_minf = float3(bbox_min - ijk_offset);
    float3 bbox_maxf = float3(bbox_max - ijk_offset + int3(1, 1, 1));

    bool hit = pnanovdb_hdda_ray_clip(bbox_minf, bbox_maxf, origin, tmin, direction, tmax);
    if (!hit || tmax > 1.0e20f)
    {
        return false;
    }

    float3 pos = pnanovdb_hdda_ray_start(origin, tmin, direction);
    pnanovdb_coord_t ijk = pnanovdb_hdda_pos_to_ijk(pos);

    // ijk is in local space; add ijk_offset back to recover the true coordinate
    // for every accessor / value lookup.
    float v0 = read_distance(grid_type, buf, acc, ijk + ijk_offset);
    float d_prev = v0 - isovalue;
    float t_prev = tmin;

    int dim = pnanovdb_uint32_as_int32(pnanovdb_readaccessor_get_dim(grid_type, buf, acc, ijk + ijk_offset));
    pnanovdb_hdda_t hdda;
    pnanovdb_hdda_init(hdda, origin, tmin, direction, tmax, dim);
    while (pnanovdb_hdda_step(hdda))
    {
        float3 pos_start = pnanovdb_hdda_ray_start(origin, hdda.tmin + 1.0001f, direction);
        ijk = pnanovdb_hdda_pos_to_ijk(pos_start);
        dim = pnanovdb_uint32_as_int32(pnanovdb_readaccessor_get_dim(grid_type, buf, acc, ijk + ijk_offset));
        pnanovdb_hdda_update(hdda, origin, direction, dim);
        if (hdda.dim > 1 || !pnanovdb_readaccessor_is_active(grid_type, buf, acc, ijk + ijk_offset))
        {
            continue;
        }
        while (pnanovdb_hdda_step(hdda) && pnanovdb_readaccessor_is_active(grid_type, buf, acc, hdda.voxel + ijk_offset))
        {
            float v = read_distance(grid_type, buf, acc, hdda.voxel + ijk_offset);
            float d = v - isovalue;
            if (d * d_prev < 0.f)
            {
                float t_curr = hdda.tmin;
                float denom = d_prev - d;
                float t_lin = (abs(denom) > 1e-12f) ? (t_prev + (t_curr - t_prev) * (d_prev / denom)) : t_curr;

                if (eps > 0.f)
                {
                    // Bisection on the continuous trilinear field for a sub-voxel hit.
                    float ta = t_prev;
                    float tb = t_curr;
                    float da = d_prev;
                    [loop]
                    for (int it = 0; it < 12; it++)
                    {
                        if ((tb - ta) <= eps)
                        {
                            break;
                        }
                        float tm = 0.5f * (ta + tb);
                        float dm = read_distance_trilinear(grid_type, buf, acc, origin + tm * direction, ijk_offset) - isovalue;
                        if (da * dm <= 0.f)
                        {
                            tb = tm;
                        }
                        else
                        {
                            ta = tm;
                            da = dm;
                        }
                    }
                    thit = 0.5f * (ta + tb);
                }
                else
                {
                    thit = t_lin;
                }
                return true;
            }
            d_prev = d;
            t_prev = hdda.tmin;
        }
    }
    return false;
}

bool render_surface(StructuredBuffer<uint2> buf, float3 worldRayOrigin, float3 worldRayDir, float isovalue, out float3 hitColor)
{
    hitColor = float3(0.f, 0.f, 0.f);

    pnanovdb_grid_handle_t grid = { pnanovdb_address_null() };
    pnanovdb_tree_handle_t tree = pnanovdb_grid_get_tree(buf, grid);
    pnanovdb_root_handle_t root = pnanovdb_tree_get_root(buf, tree);
    pnanovdb_grid_type_t grid_type = pnanovdb_grid_get_grid_type(buf, grid);

    pnanovdb_readaccessor_t acc;
    pnanovdb_readaccessor_init(PNANOVDB_REF(acc), root);

    // Transform ray into index space.
    float3 origin = pnanovdb_grid_world_to_indexf(buf, grid, worldRayOrigin);
    float3 dir = pnanovdb_grid_world_to_index_dirf(buf, grid, worldRayDir);
    float dirMag = length(dir);
    if (dirMag <= 0.f)
    {
        return false;
    }
    dir /= dirMag;

    // Auto-centering: shift the float ray math by an integer (multiple-of-4096)
    // offset so coordinates stay near the origin for off-origin grids. The
    // offset is added back at every accessor/value lookup, so results are
    // identical to true-index-space traversal, just with better float precision.
    int3 ijk_offset = int3(0, 0, 0);
    if (shader_params.auto_center != 0u)
    {
        int3 bbox_min = pnanovdb_root_get_bbox_min(buf, root);
        int3 bbox_max = pnanovdb_root_get_bbox_max(buf, root);
        int3 bbox_ave = ((bbox_max + bbox_min) >> 1u);
        ijk_offset = (bbox_ave & ~4095);
    }
    float3 origin_local = origin - float3(ijk_offset);

    float thit;
    if (!surface_zero_crossing(
            grid_type, buf, acc, origin_local, 0.f, dir, 1e9f, isovalue, shader_params.surface_eps, ijk_offset, thit))
    {
        return false;
    }

    float3 hitPos = origin_local + thit * dir;
    float3 n = surface_normal(grid_type, buf, acc, hitPos, ijk_offset);

    // Orient the normal toward the viewer.
    if (dot(n, -dir) < 0.f)
    {
        n = -n;
    }

    float3 L;
    if (length(shader_params.light_dir) > 1e-6f)
    {
        L = pnanovdb_grid_world_to_index_dirf(buf, grid, shader_params.light_dir);
        float Llen = length(L);
        L = (Llen > 1e-8f) ? (L / Llen) : -dir;
    }
    else
    {
        L = -dir; // headlight
    }

    float diffuse = max(0.f, dot(n, L));
    float ambient = shader_params.ambient;
    hitColor = shader_params.surface_color.rgb * (ambient + (1.f - ambient) * diffuse);
    return true;
}

[shader("compute")][numthreads(16, 8, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    int2 tidx = int2(dispatchThreadID.xy);

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

    float4 rayOrigin4 = is_reverse_z ? pos_d1 : pos_d0;
    rayOrigin4 = mul(rayOrigin4, editor_params.view_inv);
    float3 rayOrigin = rayOrigin4.xyz / rayOrigin4.w;

    float3 hitColor;
    bool hit = render_surface(buf, rayOrigin, rayDir, shader_params.isovalue, hitColor);
    if (hit)
    {
        texture_out[tidx] = float4(hitColor, 0.f);
    }
    else
    {
        texture_out[tidx] = float4(0.f, 0.f, 0.f, 1.f);
    }
}
