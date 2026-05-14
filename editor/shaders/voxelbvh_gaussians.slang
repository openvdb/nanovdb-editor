// voxelbvh_gaussians.slang
#define PNANOVDB_HLSL
#define PNANOVDB_ADDRESS_64
#define PNANOVDB_BUF_HLSL_64
#include "PNanoVDB.h"
#include "PNanoVDBExt.h"

#include "workgroup_hdda.slang"
#include "workgroup_voxelbvh.slang"
#include "workgroup_radix_sort.slang"

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
    uint sh_degree;
};

StructuredBuffer<uint2> buf;
RWStructuredBuffer<uint> image_out_deprecated;
RWTexture2D<float4> texture_out;
ConstantBuffer<EditorParams> editor_params;
ConstantBuffer<shader_params_t> shader_params;

#include "spherical_harmonics.slang"

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
    if (editor_params.projection[2][3] < 0.f)
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
    float4 mean_c = mul(float4(meanf, 1.f), editor_params.view);

    float3x3 covar = quat_and_scale_to_mat(quatf, scalef);

    float3x3 view_rot = float3x3(
        editor_params.view[0][0], editor_params.view[1][0], editor_params.view[2][0],
        editor_params.view[0][1], editor_params.view[1][1], editor_params.view[2][1],
        editor_params.view[0][2], editor_params.view[1][2], editor_params.view[2][2]);
    float3x3 covar_c = covar_world_to_cam(view_rot, covar);

    float fx = float(editor_params.width) * 0.5f * editor_params.projection[0][0];
    float fy = float(editor_params.height) * 0.5f * editor_params.projection[1][1];
    // TODO: should extract from proj matrix to cover non-centered cases
    float cx = float(editor_params.width) * 0.5f;
    float cy = float(editor_params.height) * 0.5f;

    float2x2 covar2d;
    float2 mean2d;
    if (editor_params.projection[2][3] == 0.f)
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
    if (editor_params.projection[2][3] == 0.f)
    {
        conic_cam_scale = 0.5f * float(editor_params.width) * editor_params.projection[0][0];
    }
    else
    {
        conic_cam_scale = 0.5f * float(editor_params.width) * editor_params.projection[0][0] / abs(mean_c.z);
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
    float4x4 view = editor_params.view;

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

float2 compute_hdda_culling_range(StructuredBuffer<uint2> buf,
                                  pnanovdb_grid_handle_t grid,
                                  float3 worldRayDirCenter,
                                  int3 ijk,
                                  int dim)
{
    int dim_mask = dim - 1;
    int3 ijk_min = ijk & ~dim_mask;
    int3 ijk_max = ijk_min + int3(dim);
    float2 range = float2(1.f / 0.f, -1.f / 0.f);
    for (int i = 0u; i < 8; i++)
    {
        int3 cmp_ijk = int3(
            (i & 1) == 0 ? ijk_min.x : ijk_max.x,
            (i & 2) == 0 ? ijk_min.y : ijk_max.y,
            (i & 4) == 0 ? ijk_min.z : ijk_max.z
        );
        float3 pos = pnanovdb_grid_index_to_worldf(buf, grid, float3(cmp_ijk));
        float t = dot(pos, worldRayDirCenter);
        range.x = min(range.x, t);
        range.y = max(range.y, t);
    }
    return range;
}

groupshared uint64_t s_voxel_val[128u];

groupshared float2 s_cull_range[128u];
groupshared uint s_prim_keys[512u];

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

void workgroup_gaussian_process(uint thread_idx,
                                bool this_thread_active,
                                uint current_hdda_idx,
                                pnanovdb_grid_handle_t grid,
                                float3 worldRayOrigin,
                                float3 worldRayDir,
                                float3 worldRayDirInv,
                                float3 worldRayDirCenter,
                                float worldTmin,
                                float worldTmax,
                                uint64_t voxel_val_raw,
                                inout float4 sum,
                                inout float nominalT)
{
    workgroup_voxelbvh_iterator_t iter = {};
    workgroup_voxelbvh_iterator_init(iter, thread_idx, 32u, grid, voxel_val_raw);

    for (uint layer = 0u; layer <= 16u; layer++)
    {
        workgroup_voxelbvh_iterator_reset(iter);

        uint workgroup_scan_accum = 0u;
        uint prim_id_index = ~0u;
        while (workgroup_voxelbvh_iterator_get(iter, thread_idx, prim_id_index))
        {
            uint prim_key = ~0u;
            if (prim_id_index != ~0u)
            {
                // culling stage
                pnanovdb_address_t prim_id_addr = pnanovdb_grid_get_gridblindmetadata_value_address(buf, grid, 1u);
                uint prim_id = pnanovdb_read_uint32(buf, pnanovdb_address_offset_product(prim_id_addr, 4u, prim_id_index));

                pnanovdb_address_t mean_addr = pnanovdb_grid_get_gridblindmetadata_value_address(buf, grid, 2u);
                float3 meanf = pnanovdb_read_vec3(buf, pnanovdb_address_offset_product(mean_addr, 12u, prim_id));

                // see if mean falls within cull range
                float cull_t = dot(meanf, worldRayDirCenter);

                // generate 4 bit sort key
                float key_norm = (cull_t - s_cull_range[current_hdda_idx].x) /
                    (s_cull_range[current_hdda_idx].y - s_cull_range[current_hdda_idx].x);
                key_norm = key_norm < 0.f ? 0.f : key_norm;
                key_norm = key_norm > 1.f ? 1.f : key_norm;
                prim_key = (prim_id << 4u) | uint(15.f * key_norm);

                bool should_cull_t = !(cull_t >= s_cull_range[current_hdda_idx].x && cull_t <= s_cull_range[current_hdda_idx].y);
                bool should_cull_layer = layer > 0u && layer - 1u != (prim_key & 15);
                if (should_cull_t || should_cull_layer)
                {
                    prim_key = ~0u;
                }
            }

            uint scan_val = 0u;
            uint scan_total = 0u;
            workgroup_scan(thread_idx, prim_key != ~0u ? 1u : 0u, scan_val, scan_total);
            if (prim_key != ~0u)
            {
                uint write_idx = scan_val - 1u + workgroup_scan_accum;
                if (write_idx < 512u)
                {
                    s_prim_keys[write_idx] = prim_key;
                }
            }
            workgroup_scan_accum += scan_total;
            if (workgroup_scan_accum > 512u)
            {
                break;
            }

            workgroup_voxelbvh_iterator_step(iter);
        }

        if (workgroup_scan_accum >= 512u && layer == 0u)
        {
            // single pass didn't work, need to go slice by slice
            continue;
        }
        if (workgroup_scan_accum > 512u)
        {
            workgroup_scan_accum = 512u;
        }

        for (uint write_idx = thread_idx; write_idx < 512u; write_idx += 128u)
        {
            if (write_idx >= workgroup_scan_accum)
            {
                s_prim_keys[write_idx] = ~0u;
            }
        }

        GroupMemoryBarrierWithGroupSync();

        if (layer == 0u)
        {
            radix_sort(thread_idx);
        }

        uint pass_count = (workgroup_scan_accum + 127u) / 128u;
        for (uint pass_idx = 0u; pass_idx < pass_count; pass_idx++)
        {
            uint list_idx = 128u * pass_idx + thread_idx;

            if (list_idx < workgroup_scan_accum)
            {
                // load gaussians into shared memory and performance projection
                uint prim_id = (s_prim_keys[list_idx] >> 4u);

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
                pnanovdb_address_t shn_addr = pnanovdb_grid_get_gridblindmetadata_value_address(buf, grid, 7u);

                // Note: this is assuming the typical PLY case for now
                float3 color = eval_sh_function(shader_params.sh_degree, sh0_addr, 3u * prim_id, shn_addr, 3u * 15u * prim_id, worldRayDirCenter);
                s_prims[thread_idx].color = color;

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
            if (this_thread_active)
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
                            worldTmin,
                            worldRayDir,
                            worldTmax,
                            worldRayDirInv,
                            sum,
                            nominalT
                        );
                    }
                }
            }

            GroupMemoryBarrierWithGroupSync();
        }

        if (workgroup_scan_accum < 512u && layer == 0u)
        {
            break;
        }
    }
}

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
                s_cull_range[thread_idx] = compute_hdda_culling_range(buf, grid, worldRayDirCenter, hdda.hdda.ijk, dim);
            }
            else
            {
                s_voxel_val[thread_idx] = ~uint64_t(0u);
                s_cull_range[thread_idx] = float2(1.f / 0.f, 1.f / 0.f);
            }
            workgroup_hdda_update(hdda, thread_idx, rayOrigin, rayDir, rayDirInv, dim);
        }

        uint current_hdda_idx = workgroup_hdda_vote(hdda, thread_idx);
        // cooperatively process min_voxel_val
        if (current_hdda_idx != ~0u)
        {
            uint64_t voxel_val_raw = s_voxel_val[current_hdda_idx];
            bool this_thread_active = workgroup_this_thread_active(hdda, thread_idx);
            workgroup_gaussian_process(
                thread_idx, this_thread_active, current_hdda_idx,
                grid,
                worldRayOrigin, worldRayDir, worldRayDirInv, worldRayDirCenter,
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

    float3 rayDirCenter;
    get_center_ray_dir(rayDirCenter);

    pnanovdb_grid_handle_t grid = { pnanovdb_address_null() };
    pnanovdb_uint32_t grid_type = pnanovdb_grid_get_grid_type(buf, grid);

    float4 sum = float4(0.f, 0.f, 0.f, 1.f);
    float nominalT = 0.f;
    ray_hdda_nanovdb(thread_idx1d, buf, rayOrigin, 0.f, rayDir, 1e9f, rayDirInv, rayDirCenter, sum, nominalT);

    texture_out[tidx] = sum;
}
