// voxelbvh_triangles.slang
#define PNANOVDB_HLSL
#define PNANOVDB_ADDRESS_64
#define PNANOVDB_BUF_HLSL_64
#include "PNanoVDB.h"

#include "workgroup_hdda.slang"
#include "workgroup_voxelbvh.slang"

#include "editor_params.slang"

struct shader_params_t
{
    uint sh_degree;
    uint max_level;
    float max_alpha;
    float bbox_edge_scale;
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

void compute_aabb(
    float3 meansf,
    float4 quatf,
    float3 scalef,
    float opacity,
    inout float3 aabb_min,
    inout float3 aabb_max)
{
    float local_scale = sqrt(2.f * log(max(0.01f, opacity) / 0.01f));

    aabb_min = float3(1.f / 0.f, 1.f / 0.f, 1.f / 0.f);
    aabb_max = float3(-1.f / 0.f, -1.f / 0.f, -1.f / 0.f);
    for (uint vert_idx = 0u; vert_idx < 8u; vert_idx++)
    {
        float3 cube_pos = float3(
            (vert_idx & 1u) == 0 ? -1.f : 1.f,
            (vert_idx & 2u) == 0 ? -1.f : 1.f,
            (vert_idx & 4u) == 0 ? -1.f : 1.f
        );
        cube_pos *= scalef * local_scale;

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

        float3 cube_pos_world;
        cube_pos_world.x = cube_pos.x * rot0.x + cube_pos.y * rot0.y + cube_pos.z * rot0.z + meansf.x;
        cube_pos_world.y = cube_pos.x * rot1.x + cube_pos.y * rot1.y + cube_pos.z * rot1.z + meansf.y;
        cube_pos_world.z = cube_pos.x * rot2.x + cube_pos.y * rot2.y + cube_pos.z * rot2.z + meansf.z;

        aabb_min = min(aabb_min, cube_pos_world);
        aabb_max = max(aabb_max, cube_pos_world);
    }
}

void intersect_bbox(
    uint prim_id,
    float3 meanf,
    float4 quatf,
    float3 scalef,
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
    float3 aabb_min = {};
    float3 aabb_max = {};
    compute_aabb(
        meanf,
        quatf,
        scalef,
        opacity,
        aabb_min,
        aabb_max);

    for (uint line_idx = 0u; line_idx < 12u; line_idx++)
    {
        uint axis = line_idx < 8u ? line_idx < 4u ? 0 : 1 : 2;
        uint quad = line_idx & 3;
        float w = shader_params.bbox_edge_scale;
        float winv = 1.f - w;
        float3 line_min = w * aabb_min + winv * aabb_max;
        float3 line_max = winv * aabb_min + w * aabb_max;
        if (axis == 0) {
            line_min.x = aabb_min.x;
            line_min.y = (quad & 1) != 0u ? aabb_min.y : line_min.y;
            line_min.z = (quad & 2) != 0u ? aabb_min.z : line_min.z;
            line_max.x = aabb_max.x;
            line_max.y = (quad & 1) == 0u ? aabb_max.y : line_max.y;
            line_max.z = (quad & 2) == 0u ? aabb_max.z : line_max.z;
        }
        if (axis == 1) {
            line_min.y = aabb_min.y;
            line_min.x = (quad & 1) != 0u ? aabb_min.x : line_min.x;
            line_min.z = (quad & 2) != 0u ? aabb_min.z : line_min.z;
            line_max.y = aabb_max.y;
            line_max.x = (quad & 1) == 0u ? aabb_max.x : line_max.x;
            line_max.z = (quad & 2) == 0u ? aabb_max.z : line_max.z;
        }
        if (axis == 2) {
            line_min.z = aabb_min.z;
            line_min.y = (quad & 1) != 0u ? aabb_min.y : line_min.y;
            line_min.x = (quad & 2) != 0u ? aabb_min.x : line_min.x;
            line_max.z = aabb_max.z;
            line_max.y = (quad & 1) == 0u ? aabb_max.y : line_max.y;
            line_max.x = (quad & 2) == 0u ? aabb_max.x : line_max.x;
        }
        float2 box_trange = {};
        bool box_hit = intersect_box(
            rayDir,
            rayDirInv,
            line_min - rayOrigin,
            line_max - rayOrigin,
            box_trange.x,
            box_trange.y);
        if (box_hit)
        {
            float t = box_trange.x;
            float alpha = 1.f;
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
    }
}

void intersect_gaussian(
    uint id,
    float3 meanf,
    float4 quatf,
    float3 scalef,
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
#if 1
    intersect_bbox(
        id,
        meanf,
        quatf,
        scalef,
        color,
        opacity,
        rayOrigin,
        tmin,
        rayDir,
        tmax,
        rayDirInv,
        sum,
        nominalT);
#endif
#if 1
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
#if 0
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
    if (!(sigma < 0.f || alpha < 0.01f) && alpha < 0.011f)
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
#endif
}

groupshared uint64_t s_voxel_val[128u];

struct prim_t
{
    float3 mean;
    float opacity;
    float4 quat;
    float3 scale;
    uint id;
    float3 color;
    uint pad1;
    float3 conic;
    uint pad2;
};
groupshared prim_t s_prims[128u];

void workgroup_gaussian_process(uint thread_idx,
                                bool this_thread_active,
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
    uint mask = (1u << shader_params.max_level) - 1u;

    workgroup_voxelbvh_iterator_t iter = {};
    workgroup_voxelbvh_iterator_init_masked(buf, iter, thread_idx, 32u, grid, voxel_val_raw, mask);

    uint prim_id_index = ~0u;
    while (workgroup_voxelbvh_iterator_get(iter, thread_idx, prim_id_index))
    {
        if (prim_id_index != ~0u)
        {
            pnanovdb_address_t prim_id_addr = pnanovdb_grid_get_gridblindmetadata_value_address(buf, grid, 1u);
            uint prim_id = pnanovdb_read_uint32(buf, pnanovdb_address_offset_product(prim_id_addr, 4u, prim_id_index));

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
            s_prims[thread_idx].quat = quatf;

            pnanovdb_address_t scale_addr = pnanovdb_grid_get_gridblindmetadata_value_address(buf, grid, 5u);
            float3 scalef = pnanovdb_read_vec3(buf, pnanovdb_address_offset_product(scale_addr, 12u, prim_id));
            s_prims[thread_idx].scale = scalef;

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

        uint count = workgroup_voxelbvh_iterator_count_in_pass(iter);

        // only participate if this pixel actually wanted this data
        if (this_thread_active)
        {
            for (uint s_idx = 0u; s_idx < count; s_idx++)
            {
                intersect_gaussian(
                    s_prims[s_idx].id,
                    s_prims[s_idx].mean,
                    s_prims[s_idx].quat,
                    s_prims[s_idx].scale,
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
                uint skip_level = voxelbvh_get_skip_level(val_raw);

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
            workgroup_gaussian_process(
                thread_idx, this_thread_active,
                grid,
                worldRayOrigin, worldRayDir, worldRayDirInv, worldRayDirCenter,
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

    pnanovdb_grid_handle_t grid = { pnanovdb_address_null() };
    pnanovdb_uint32_t grid_type = pnanovdb_grid_get_grid_type(buf, grid);

    float4 sum = float4(0.f, 0.f, 0.f, 1.f);
    float nominalT = 0.f;
    ray_hdda_nanovdb(thread_idx1d, buf, rayOrigin, 0.f, rayDir, 1e9f, rayDirInv, rayDirCenter, sum, nominalT);

    texture_out[tidx] = sum;
}
