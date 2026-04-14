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

float3x3 quat_to_mat(float4 quatf)
{
    float3 rot0 = float3(
        1.f - 2.f * (quatf.y * quatf.y + quatf.z * quatf.z),
        2.f * (quatf.x * quatf.y - quatf.z * quatf.w),
        2.f * (quatf.x * quatf.z + quatf.y * quatf.w));
    float3 rot1 = float3(
        2.f * (quatf.x * quatf.y + quatf.z * quatf.w),
        1.f - 2.f * (quatf.x * quatf.x + quatf.z * quatf.z),
        2.f * (quatf.y * quatf.z - quatf.x * quatf.w));
    float3 rot2 = float3(
        2.f * (quatf.x * quatf.z - quatf.y * quatf.w),
        2.f * (quatf.y * quatf.z + quatf.x * quatf.w),
        1.f - 2.f * (quatf.x * quatf.x + quatf.y * quatf.y));
    return float3x3(rot0, rot1, rot2);
}

float3x3 quat_and_scale_to_mat(float4 quatf, float3 scalef)
{
    float3x3 R = quat_to_mat(quatf);
    float3x3 S = float3x3(scalef.x, 0.f, 0.f, 0.f, scalef.y, 0.f, 0.f, 0.f, scalef.z);
    float3x3 M = mul(R, S);
    return mul(M, transpose(M));
}

float3x3 covar_world_to_cam(float3x3 R, float3x3 covar)
{
    return mul(mul(R, covar), transpose(R));
}

void persp_proj(
    float3 mean3d,
    float3x3 cov3d,
    float fx, float fy,
    float cx, float cy,
    uint width, uint height,
    inout float2x2 cov2d,
    inout float2 mean2d)
{
    float x = mean3d.x;
    float y = mean3d.y;
    float z = mean3d.z;

    // for right handed projection, flip z sign
    float J_off_sign = -1.f;
    // if (constants.near_plane < 0.f)
    if (editor_params.projection_w.z < 0.f)
    {
        z = -z;
        J_off_sign = 1.f;
    }

    float tan_fovx = 0.5f * float(width) / fx;
    float tan_fovy = 0.5f * float(height) / fy;
    float lim_x_pos = (width - cx) / fx + 0.3f * tan_fovx;
    float lim_x_neg = cx / fx + 0.3f * tan_fovx;
    float lim_y_pos = (height - cy) / fy + 0.3f * tan_fovy;
    float lim_y_neg = cy / fy + 0.3f * tan_fovy;

    float rz = 1.f / z;
    float rz2 = rz * rz;
    float tx = z * min(lim_x_pos, max(-lim_x_neg, x * rz));
    float ty = z * min(lim_y_pos, max(-lim_y_neg, y * rz));

    float2x3 J = float2x3(
        fx * rz, 0.f, J_off_sign * fx * tx * rz2,
        0.f, fy * rz, J_off_sign * fy * ty * rz2
    );

    cov2d = mul(mul(J, cov3d), transpose(J));
    mean2d = float2(fx * x * rz + cx, fy * y * rz + cy);
}

void ortho_proj(
    float3 mean3d,
    float3x3 cov3d,
    float fx, float fy,
    float cx, float cy,
    uint width, uint height,
    inout float2x2 cov2d,
    inout float2 mean2d)
{
    float x = mean3d.x;
    float y = mean3d.y;

    float2x3 J = float2x3(
        fx, 0.f, 0.f,
        0.f, fy, 0.f
    );

    cov2d = mul(mul(J, cov3d), transpose(J));
    mean2d = float2(fx * x + cx, fy * y + cy);
}

float add_blur(float eps2d, inout float2x2 covar, inout float compensation)
{
    float det_orig = covar[0][0] * covar[1][1] - covar[0][1] * covar[1][0];
    covar[0][0] += eps2d;
    covar[1][1] += eps2d;
    float det_blur = covar[0][0] * covar[1][1] - covar[0][1] * covar[1][0];
    compensation = sqrt(max(0.f, det_orig / det_blur));
    return det_blur;
}

float inverse(float2x2 M, inout float2x2 Minv)
{
    float det = M[0][0] * M[1][1] - M[0][1] * M[1][0];
    if (det <= 0.f) {
        return det;
    }
    float invDet = 1.f / det;
    Minv = float2x2(
        M[1][1] * invDet, -M[0][1] * invDet,
        -M[1][0] * invDet, M[0][0] * invDet);
    return det;
}

void project_conic(
    float3 meanf,
    float4 quatf,
    float3 scalef,
    inout float3 conic)
{
    float4 mean_c = mat_mul(float4(meanf, 1.f),
                            editor_params.view_x, editor_params.view_y,
                            editor_params.view_z, editor_params.view_w);

    float3x3 covar = quat_and_scale_to_mat(quatf, scalef);

    float3x3 view_rot = float3x3(
        editor_params.view_x.x, editor_params.view_x.y, editor_params.view_x.z,
        editor_params.view_y.x, editor_params.view_y.y, editor_params.view_y.z,
        editor_params.view_z.x, editor_params.view_z.y, editor_params.view_z.z);
    float3x3 covar_c = covar_world_to_cam(view_rot, covar);

    float fx = float(editor_params.width) * 0.5f * editor_params.projection_x.x;
    float fy = float(editor_params.height) * 0.5f * editor_params.projection_y.y;
    // TODO: should extract from proj matrix to cover non-centered cases
    float cx = float(editor_params.width) * 0.5f;
    float cy = float(editor_params.height) * 0.5f;

    float2x2 covar2d;
    float2 mean2d;
    if (editor_params.projection_w.z == 0.f)
    {
        ortho_proj(
            mean_c.xyz, covar_c,
            fx, fy,
            cx, cy,
            editor_params.width, editor_params.height,
            covar2d,
            mean2d
        );
    }
    else
    {
        persp_proj(
            mean_c.xyz, covar_c,
            fx, fy,
            cx, cy,
            editor_params.width, editor_params.height,
            covar2d,
            mean2d
        );
    }

    const float eps2d = 0.3f;
    float compensation;
    float det = add_blur(eps2d, covar2d, compensation);

    float2x2 covar2d_inv;
    inverse(covar2d, covar2d_inv);

    conic = float3(covar2d_inv[0][0], covar2d_inv[0][1], covar2d_inv[1][1]);

    // move conic to camera space
    float conic_cam_scale = 1.f;
    if (editor_params.projection_w.z == 0.f)
    {
        conic_cam_scale = 0.5f * float(editor_params.width) * editor_params.projection_x.x;
    }
    else
    {
        conic_cam_scale = 0.5f * float(editor_params.width) * editor_params.projection_x.x / abs(mean_c.z);
    }
    conic *= (conic_cam_scale * conic_cam_scale);
}

void intersect_gaussian(
    float3 meanf,
    float3 conic,
    float3 color,
    float opacity,
    float3 rayOrigin,
    float tmin,
    float3 rayDir,
    float tmax,
    float3 rayDirInv,
    inout float4 sum,
    inout float nominalT)
{
    float4x4 view = float4x4(
        editor_params.view_x.x, editor_params.view_y.x, editor_params.view_z.x, editor_params.view_w.x,
        editor_params.view_x.y, editor_params.view_y.y, editor_params.view_z.y, editor_params.view_w.y,
        editor_params.view_x.z, editor_params.view_y.z, editor_params.view_z.z, editor_params.view_w.z,
        editor_params.view_x.w, editor_params.view_y.w, editor_params.view_z.w, editor_params.view_w.w
    );

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
#if 1
    if (t > 0.f && t >= tmin && t < tmax)
    {
        if (!(sigma < 0.f || alpha < 0.01f))
        {
            nominalT = sum.a * (alpha * t) + nominalT;
            sum.rgb = sum.a * (alpha * color) + sum.rgb;
            sum.a = (1.f - alpha) * sum.a;
        }
    }
#else
    if (!(sigma < 0.f || alpha < 0.01f))
    {
        alpha = 1.f;
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
#endif
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
groupshared uint s_min_tmin;
groupshared uint s_min_hdda_idx;

groupshared uint64_t s_range[12u];

groupshared float2 s_cull_range[128u];

groupshared uint s_prim_ids[1024u];
groupshared uint s_prim_keys[1024u];

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
    float3 mean;
    uint id;
    float3 conic;
    float pad;
    float3 color;
    float opacity;
};
groupshared prim_t s_prims[128u];

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
    float3 world_min = pnanovdb_grid_index_to_worldf(buf, grid, float3(ijk_min));
    float3 world_max = pnanovdb_grid_index_to_worldf(buf, grid, float3(ijk_max));
    float2 range = float2(1.f / 0.f, -1.f / 0.f);
    for (int i = 0u; i < 8; i++)
    {
        float3 pos = float3(
            (i & 1) == 0 ? world_min.x : world_max.x,
            (i & 2) == 0 ? world_min.y : world_max.y,
            (i & 4) == 0 ? world_min.z : world_max.z
        );
        float t = dot(pos, worldRayDirCenter);
        range.x = min(range.x, t);
        range.y = max(range.y, t);
    }
    return range;
}

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
                if (dim > 1)
                {
                    dim /= 2u;
                }
                hdda_update(hdda, rayOrigin, rayDir, rayDirInv, dim);

                s_voxel_val[thread_idx] = val_raw;
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
                uint lprim_idx = 128u * pass_idx + thread_idx;

                uint active_level_idx = 0u;
                uint range_accum = 0u;
                uint range_begin = 0u;
                uint range_end = 0u;
                uint range_idx = 0u;
                while (active_level_idx < active_level_count)
                {
                    range_begin = uint(s_range[active_level_idx]);
                    range_end = uint(s_range[active_level_idx] >> 32u);
                    range_idx = lprim_idx - range_accum;
                    if (range_begin + range_idx < range_end)
                    {
                        break;
                    }
                    range_accum += (range_end - range_begin);
                    active_level_idx++;
                }

                uint prim_id = ~0u;
                float cull_t = 1.f / 0.f;
                if (range_begin + range_idx < range_end)
                {
                    // culling stage
                    pnanovdb_address_t prim_id_addr = pnanovdb_grid_get_gridblindmetadata_value_address(buf, grid, 1u);
                    prim_id = pnanovdb_read_uint32(buf, pnanovdb_address_offset_product(prim_id_addr, 4u, range_begin + range_idx));

                    pnanovdb_address_t mean_addr = pnanovdb_grid_get_gridblindmetadata_value_address(buf, grid, 2u);
                    float3 meanf = pnanovdb_read_vec3(buf, pnanovdb_address_offset_product(mean_addr, 12u, prim_id));

                    // see if mean falls within cull range
                    cull_t = dot(meanf, worldRayDirCenter);
                    bool should_cull = !(cull_t >= s_cull_range[s_min_hdda_idx].x && cull_t <= s_cull_range[s_min_hdda_idx].y);
                    if (should_cull)
                    {
                        prim_id = ~0u;
                    }
                }

                uint scan_val = 0u;
                uint scan_total = 0u;
                workgroup_scan(thread_idx, prim_id != ~0u ? 1u : 0u, scan_val, scan_total);
                if (prim_id != ~0u)
                {
                    uint write_idx = scan_val - 1u + workgroup_scan_accum;
                    if (write_idx < 1024u)
                    {
                        s_prim_ids[write_idx] = prim_id;
                        s_prim_keys[write_idx] = sort_asuint(cull_t);
                    }
                }
                workgroup_scan_accum += scan_total;
            }

            if (workgroup_scan_accum > 1024u)
            {
                workgroup_scan_accum = 1024u;
            }

            pass_count = (workgroup_scan_accum + 127u) / 128u;
            for (uint pass_idx = 0u; pass_idx < pass_count; pass_idx++)
            {
                uint list_idx = 128u * pass_idx + thread_idx;

                if (list_idx < workgroup_scan_accum)
                {
                    // load gaussians into shared memory and performance projection
                    uint prim_id = s_prim_ids[list_idx];

                    s_prims[thread_idx].id = prim_id;

                    pnanovdb_address_t mean_addr = pnanovdb_grid_get_gridblindmetadata_value_address(buf, grid, 2u);
                    float3 meanf = pnanovdb_read_vec3(buf, pnanovdb_address_offset_product(mean_addr, 12u, prim_id));
                    s_prims[thread_idx].mean = meanf;

                    pnanovdb_address_t quat_addr = pnanovdb_grid_get_gridblindmetadata_value_address(buf, grid, 4u);
                    float4 quatf = float4(
                        pnanovdb_read_float(buf, pnanovdb_address_offset_product(quat_addr, 4u, 4u * prim_id + 1u)),
                        pnanovdb_read_float(buf, pnanovdb_address_offset_product(quat_addr, 4u, 4u * prim_id + 2u)),
                        pnanovdb_read_float(buf, pnanovdb_address_offset_product(quat_addr, 4u, 4u * prim_id + 3u)),
                        pnanovdb_read_float(buf, pnanovdb_address_offset_product(quat_addr, 4u, 4u * prim_id + 0u))
                    );
                    pnanovdb_address_t scale_addr = pnanovdb_grid_get_gridblindmetadata_value_address(buf, grid, 5u);
                    float3 scalef = pnanovdb_read_vec3(buf, pnanovdb_address_offset_product(scale_addr, 12u, prim_id));

                    float3 conic = {};
                    project_conic(s_prims[thread_idx].mean, quatf, scalef, conic);
                    s_prims[thread_idx].conic = conic;

                    pnanovdb_address_t sh0_addr = pnanovdb_grid_get_gridblindmetadata_value_address(buf, grid, 6u);
                    float3 sh0 = pnanovdb_read_vec3(buf, pnanovdb_address_offset_product(sh0_addr, 12u, prim_id));
                    s_prims[thread_idx].color = float3(0.2820947917738781f * sh0 + 0.5f);

                    pnanovdb_address_t opacity_addr = pnanovdb_grid_get_gridblindmetadata_value_address(buf, grid, 3u);
                    s_prims[thread_idx].opacity = pnanovdb_read_float(buf, pnanovdb_address_offset_product(opacity_addr, 4u, prim_id));
                }

                GroupMemoryBarrierWithGroupSync();

                uint prim_count = workgroup_scan_accum - 128u * pass_idx;
                if (prim_count > 128u)
                {
                    prim_count = 128u;
                }

                // only participate if this pixel actually wanted this data
                if (s_voxel_val[s_min_hdda_idx] == s_voxel_val[thread_idx])
                {
                    for (uint s_idx = 0u; s_idx < prim_count; s_idx++)
                    {
                        if (s_prims[s_idx].id != ~0u)
                        {
                            intersect_gaussian(
                                s_prims[s_idx].mean,
                                s_prims[s_idx].conic,
                                s_prims[s_idx].color,
                                s_prims[s_idx].opacity,
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
            if (s_voxel_val[s_min_hdda_idx] == s_voxel_val[thread_idx])
            {
                hdda_needs_update = true;

                uint skip_level = (uint(s_voxel_val[s_min_hdda_idx]) >> 16u) & 0xFFFF;
                // align HDDA tmin/tmax to current voxel size
                int dim = 1 << skip_level;
                if (dim > 1)
                {
                    dim /= 2u;
                }
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

[shader("compute")][numthreads(32, 4, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID, uint3 thread_idx : SV_GroupThreadID)
{
    int2 tidx = int2(dispatchThreadID.xy);
    uint thread_idx1d = thread_idx.x | (thread_idx.y << 5);

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
