// voxelbvh_nanovdb_integral.slang

#define PNANOVDB_HLSL
#define PNANOVDB_ADDRESS_64
#define PNANOVDB_BUF_HLSL_64
#define PNANOVDB_BUF_HLSL_RW
#include "PNanoVDB.h"

#define WORKGROUP_VOXELBVH_THREADS 128

struct constants_t
{
    float4x4 view;
    float4x4 view_inv;
    uint resolution;
    uint dst_grid_type;
    uint pass_id;
};

#include "workgroup_hdda.slang"
#include "workgroup_voxelbvh.slang"
#include "voxelbvh_common.h"
#include "workgroup_radix_sort.slang"

ConstantBuffer<constants_t> constants;

RWStructuredBuffer<uint2> src_buf;
RWStructuredBuffer<uint2> buf;

#define SPHERICAL_HARMONIC_BUF src_buf

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

void ortho_proj(
    float3 mean3d,
    float3x3 cov3d,
    float fx, float fy,
    float cx, float cy,
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
    float4x4 view,
    uint resolution,
    inout float3 conic)
{
    float4 mean_c = mul(float4(meanf, 1.f), view);

    float3x3 covar = quat_and_scale_to_mat(quatf, scalef);

    float3x3 view_rot = float3x3(
        view[0][0], view[1][0], view[2][0],
        view[0][1], view[1][1], view[2][1],
        view[0][2], view[1][2], view[2][2]);
    float3x3 covar_c = covar_world_to_cam(view_rot, covar);

    float fx = float(resolution) * 0.5f;
    float fy = float(resolution) * 0.5f;
    // TODO: should extract from proj matrix to cover non-centered cases
    float cx = float(resolution) * 0.5f;
    float cy = float(resolution) * 0.5f;

    float2x2 covar2d;
    float2 mean2d;
    ortho_proj(
        mean_c.xyz, covar_c,
        fx, fy,
        cx, cy,
        covar2d,
        mean2d
    );

    const float eps2d = 0.3f;
    float compensation;
    float det = add_blur(eps2d, covar2d, compensation);

    float2x2 covar2d_inv;
    inverse(covar2d, covar2d_inv);

    conic = float3(covar2d_inv[0][0], covar2d_inv[0][1], covar2d_inv[1][1]);

    // move conic to camera space
    float conic_cam_scale = 0.5f * float(resolution);
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
    float4x4 view,
    uint resolution,
    inout float4 sum,
    inout float nominalT)
{
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

groupshared uint64_t s_voxel_val[128u];

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

float2 compute_culling_range(pnanovdb_buf_t cull_buf,
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
        float3 pos = pnanovdb_grid_index_to_worldf(cull_buf, grid, float3(cmp_ijk));
        float t = dot(pos, worldRayDirCenter);
        range.x = min(range.x, t);
        range.y = max(range.y, t);
    }
    return range;
}


void workgroup_gaussian_process(uint thread_idx,
                                bool this_thread_active,
                                pnanovdb_grid_handle_t grid,
                                float3 worldRayOrigin,
                                float3 worldRayDir,
                                float3 worldRayDirInv,
                                float4x4 view,
                                uint resolution,
                                float3 worldRayDirCenter,
                                float worldTmin,
                                float worldTmax,
                                uint64_t voxel_val_raw,
                                int3 src_ijk,
                                inout float4 sum,
                                inout float nominalT)
{
    workgroup_voxelbvh_iterator_t iter = {};
    workgroup_voxelbvh_iterator_init(src_buf, iter, thread_idx, 32u, grid, voxel_val_raw);

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
                pnanovdb_address_t prim_id_addr = pnanovdb_grid_get_gridblindmetadata_value_address(src_buf, grid, 1u);
                uint prim_id = pnanovdb_read_uint32(src_buf, pnanovdb_address_offset_product(prim_id_addr, 4u, prim_id_index));

                pnanovdb_address_t mean_addr = pnanovdb_grid_get_gridblindmetadata_value_address(src_buf, grid, 2u);
                float3 meanf = pnanovdb_read_vec3(src_buf, pnanovdb_address_offset_product(mean_addr, 12u, prim_id));

                // see if mean falls within cull range
                float cull_t = dot(meanf, worldRayDirCenter);

                uint skip_level = voxelbvh_get_skip_level(voxel_val_raw);
                int dim = 1 << skip_level;

                float2 cull_range = compute_culling_range(src_buf,
                             grid,
                             worldRayDirCenter,
                             src_ijk,
                             dim);

                // generate 4 bit sort key
                float key_norm = (cull_t - cull_range.x) /
                    (cull_range.y - cull_range.x);
                key_norm = key_norm < 0.f ? 0.f : key_norm;
                key_norm = key_norm > 1.f ? 1.f : key_norm;
                prim_key = (prim_id << 4u) | uint(15.f * key_norm);

                bool should_cull_t = !(cull_t >= cull_range.x && cull_t <= cull_range.y);
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

                pnanovdb_address_t mean_addr = pnanovdb_grid_get_gridblindmetadata_value_address(src_buf, grid, 2u);
                float3 meanf = pnanovdb_read_vec3(src_buf, pnanovdb_address_offset_product(mean_addr, 12u, prim_id));
                s_prims[thread_idx].mean = meanf;

                pnanovdb_address_t quat_addr = pnanovdb_grid_get_gridblindmetadata_value_address(src_buf, grid, 4u);
                float4 quatf = float4(
                    pnanovdb_read_float(src_buf, pnanovdb_address_offset_product(quat_addr, 4u, 4u * prim_id + 1u)),
                    pnanovdb_read_float(src_buf, pnanovdb_address_offset_product(quat_addr, 4u, 4u * prim_id + 2u)),
                    pnanovdb_read_float(src_buf, pnanovdb_address_offset_product(quat_addr, 4u, 4u * prim_id + 3u)),
                    pnanovdb_read_float(src_buf, pnanovdb_address_offset_product(quat_addr, 4u, 4u * prim_id + 0u))
                );
                pnanovdb_address_t scale_addr = pnanovdb_grid_get_gridblindmetadata_value_address(src_buf, grid, 5u);
                float3 scalef = pnanovdb_read_vec3(src_buf, pnanovdb_address_offset_product(scale_addr, 12u, prim_id));

                float3 conic = {};
                project_conic(s_prims[thread_idx].mean, quatf, scalef, view, resolution, conic);
                s_prims[thread_idx].conic = conic;

                pnanovdb_address_t sh0_addr = pnanovdb_grid_get_gridblindmetadata_value_address(src_buf, grid, 6u);
                pnanovdb_address_t shn_addr = pnanovdb_grid_get_gridblindmetadata_value_address(src_buf, grid, 7u);

                // Note: this is assuming the typical PLY case for now
                const uint sh_degree = 0u;
                float3 color = eval_sh_function(sh_degree, sh0_addr, 3u * prim_id, shn_addr, 3u * 15u * prim_id, worldRayDirCenter);
                s_prims[thread_idx].color = color;

                pnanovdb_address_t opacity_addr = pnanovdb_grid_get_gridblindmetadata_value_address(src_buf, grid, 3u);
                s_prims[thread_idx].opacity = pnanovdb_read_float(src_buf, pnanovdb_address_offset_product(opacity_addr, 4u, prim_id));
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
                            view,
                            resolution,
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

groupshared int3 s_src_ijk[WORKGROUP_VOXELBVH_THREADS];

void ray_hdda_nanovdb(uint thread_idx,
                      float3 worldRayOrigin,
                      float rayMinT,
                      float3 worldRayDir,
                      float rayMaxT,
                      float3 worldRayDirInv,
                      float4x4 view,
                      uint resolution,
                      float3 worldRayDirCenter,
                      inout float4 sum,
                      inout float nominalT)
{
    pnanovdb_grid_handle_t grid = { pnanovdb_address_null() };
    pnanovdb_tree_handle_t tree = pnanovdb_grid_get_tree(src_buf, grid);
    pnanovdb_root_handle_t root = pnanovdb_tree_get_root(src_buf, tree);
    pnanovdb_grid_type_t grid_type = pnanovdb_grid_get_grid_type(src_buf, grid);

    pnanovdb_readaccessor_t acc;
    pnanovdb_readaccessor_init(PNANOVDB_REF(acc), root);

    // transform ray from world to index space
    float3 rayOrigin = pnanovdb_grid_world_to_indexf(src_buf, grid, worldRayOrigin);
    float3 rayDir = pnanovdb_grid_world_to_index_dirf(src_buf, grid, worldRayDir);
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
    int3 bbox_min = pnanovdb_root_get_bbox_min(src_buf, root);
    int3 bbox_max = pnanovdb_root_get_bbox_max(src_buf, root);

    float3 bbox_minf = float3(bbox_min - int3(1,1,1));
    float3 bbox_maxf = float3(bbox_max + int3(2,2,2));

    float boxMinT;
    float boxMaxT;
    bool isHit = intersect_box(rayDir, rayDirInv, bbox_minf - rayOrigin, bbox_maxf - rayOrigin, boxMinT, boxMaxT);

    boxMinT = max(rayMinT, boxMinT);
    boxMaxT = min(rayMaxT, boxMaxT);
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
                pnanovdb_address_t addr = pnanovdb_readaccessor_get_value_address(grid_type, src_buf, acc, hdda.hdda.ijk);
                uint64_t val_raw = pnanovdb_read_uint64(src_buf, addr);
                uint skip_level = voxelbvh_get_skip_level(val_raw);

                // align HDDA tmin/tmax to current voxel size
                dim = 1 << skip_level;

                s_voxel_val[thread_idx] = val_raw;
                s_src_ijk[thread_idx] = hdda.hdda.ijk;
            }
            else
            {
                s_voxel_val[thread_idx] = ~uint64_t(0u);
                s_src_ijk[thread_idx] = int3(0x40000000, 0x40000000, 0x40000000);
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
                worldRayOrigin, worldRayDir, worldRayDirInv,
                view, resolution, worldRayDirCenter,
                max(boxMinT, hdda.hdda.tmin) * rayDirMagnInv,
                min(boxMaxT, hdda.hdda.tmax) * rayDirMagnInv,
                voxel_val_raw,
                s_src_ijk[current_hdda_idx],
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

void nanovdb_set_bit(pnanovdb_buf_t write_buf, pnanovdb_address_t addr, uint bit_idx)
{
    uint64_t word_idx = (addr.byte_offset >> 3u) + (bit_idx >> 6u);
    uint64_t val = uint64_t(1u) << (bit_idx & 63u);
    uint2 val2 = uint2(uint(val), uint(val >> 32u));
    if (val2.x != 0u) { InterlockedOr(write_buf[word_idx].x, val2.x); };
    if (val2.y != 0u) { InterlockedOr(write_buf[word_idx].y, val2.y); };
}

bool find_child_bit(
    pnanovdb_buf_t lookup_buf,
    pnanovdb_grid_handle_t grid,
    uint pass_id,
    pnanovdb_coord_t ijk,
    inout pnanovdb_address_t child_mask_addr,
    inout uint child_mask_n)
{
    pnanovdb_uint32_t grid_type = pnanovdb_grid_get_grid_type(lookup_buf, grid);
    pnanovdb_tree_handle_t tree = pnanovdb_grid_get_tree(lookup_buf, grid);
    pnanovdb_root_handle_t root = pnanovdb_tree_get_root(lookup_buf, tree);

    child_mask_addr = pnanovdb_address_null();
    child_mask_n = 0u;
    pnanovdb_root_tile_handle_t tile = pnanovdb_root_find_tile(grid_type, lookup_buf, root, ijk);
    if (!pnanovdb_address_is_null(tile.address))
    {
        int64_t child_off = pnanovdb_root_tile_get_child(lookup_buf, tile);
        if (pass_id == 0u)
        {
            child_mask_addr = pnanovdb_address_offset(tile.address, PNANOVDB_ROOT_TILE_OFF_CHILD);
            child_mask_n = 0u;
        }
        else
        {
            if (child_off != 0)
            {
                pnanovdb_upper_handle_t upper = pnanovdb_root_get_child(grid_type, lookup_buf, root, tile);
                pnanovdb_uint32_t upper_n = pnanovdb_upper_coord_to_offset(ijk);
                if (pass_id == 1u)
                {
                    child_mask_addr = pnanovdb_address_offset(upper.address, PNANOVDB_UPPER_OFF_CHILD_MASK);
                    child_mask_n = upper_n;
                }
                else
                {
                    if (pnanovdb_upper_get_child_mask(lookup_buf, upper, upper_n))
                    {
                        pnanovdb_lower_handle_t lower = pnanovdb_upper_get_child(grid_type, lookup_buf, upper, upper_n);
                        pnanovdb_uint32_t lower_n = pnanovdb_lower_coord_to_offset(ijk);
                        if (pass_id == 2u)
                        {
                            child_mask_addr = pnanovdb_address_offset(lower.address, PNANOVDB_LOWER_OFF_CHILD_MASK);
                            child_mask_n = lower_n;
                        }
                    }
                }
            }
        }
    }
    return !pnanovdb_address_is_null(child_mask_addr);
}

[shader("compute")][numthreads(16, 8, 1)]
void main(uint3 group_idx : SV_GroupID, uint3 thread_idx : SV_GroupThreadID)
{
    uint thread_idx_1d = (thread_idx.y << 4) | thread_idx.x;

    int3 tidx = int3(
        (int(group_idx.x) << 4) + thread_idx.x,
        (int(group_idx.y) << 3) + thread_idx.y,
        0
    );

    float3 ray_origin = float3(tidx) + 0.5f;
    float3 ray_dir = float3(0.f, 0.f, 1.f);
    float4x4 view = float4x4(
        1.f, 0.f, 0.f, 0.f,
        0.f, 1.f, 0.f, 0.f,
        0.f, 0.f, 1.f, 0.f,
        0.f, 0.f, 0.f, 1.f
    );
    float tmin = 0.f;
    float tmax = float(constants.resolution);

    pnanovdb_grid_handle_t grid = {};

    // need to transform ray and view to world space
    float3 worldRayOrigin = pnanovdb_grid_index_to_worldf(buf, grid, ray_origin);
    float3 worldRayDir = pnanovdb_grid_index_to_world_dirf(buf, grid, ray_dir);
    float worldTmin = tmin;
    float worldTmax = tmax;
    float rayDirMagn = length(worldRayDir);
    if (rayDirMagn > 0.f)
    {
        worldRayDir /= rayDirMagn;
        worldTmin *= rayDirMagn;
        worldTmax *= rayDirMagn;
    }
    float3 worldRayDirInv = float3(1.f, 1.f, 1.f) / worldRayDir;

#if 0
    float3 view_x = pnanovdb_grid_index_to_world_dirf(buf, grid, view[0].xyz);
    float3 view_y = pnanovdb_grid_index_to_world_dirf(buf, grid, view[1].xyz);
    float3 view_z = pnanovdb_grid_index_to_world_dirf(buf, grid, view[2].xyz);
    float3 view_w = pnanovdb_grid_index_to_worldf(buf, grid, view[3].xyz);
    float4x4 worldView = float4x4(
        view_x.x, view_x.y, view_x.z, 0.f,
        view_y.x, view_y.y, view_y.z, 0.f,
        view_z.x, view_z.y, view_z.z, 0.f,
        view_w.x, view_w.y, view_w.z, 1.f);
#else
    float3 up_vec = float3(0.f, 0.f, 1.f);
    if (abs(worldRayDir.x) <= abs(worldRayDir.y) && abs(worldRayDir.x) <= abs(worldRayDir.z))
    {
        up_vec = float3(1.f, 0.f, 0.f);
    }
    else if (abs(worldRayDir.y) <= abs(worldRayDir.x) && abs(worldRayDir.y) <= abs(worldRayDir.z))
    {
        up_vec = float3(0.f, 1.f, 0.f);
    }
    float3 x_vec = normalize(cross_product(up_vec, worldRayDir));
    float3 y_vec = cross_product(worldRayDir, x_vec);

    float4x4 worldView = float4x4(
        x_vec.x, y_vec.x, worldRayDir.x, 0.f,
        x_vec.y, y_vec.y, worldRayDir.y, 0.f,
        x_vec.z, y_vec.z, worldRayDir.z, 0.f,
        0.f, 0.f, 0.f, 1.f
    );
#endif

    float4 sum = float4(0.f, 0.f, 0.f, 1.f);
    float nominalT = 0.f;
    ray_hdda_nanovdb(thread_idx_1d,
                     worldRayOrigin,
                     worldTmin,
                     worldRayDir,
                     worldTmax,
                     worldRayDirInv,
                     worldView,
                     constants.resolution,
                     worldRayDir,
                     sum,
                     nominalT);

    if (sum.a < 1.f)
    {
        float world_hit_t = nominalT / (1.f - sum.a);
        // move t back to local space
        float hit_t = world_hit_t / rayDirMagn;

        float3 hitidxf = ray_dir * hit_t + ray_origin;
        int3 ijk = int3(floor(hitidxf));

        // topology passes
        if (constants.pass_id < 3u)
        {
            pnanovdb_address_t child_mask_addr = {};
            uint child_mask_n = 0u;
            if (find_child_bit(
                buf,
                grid,
                constants.pass_id,
                ijk,
                child_mask_addr,
                child_mask_n))
            {
                nanovdb_set_bit(buf, child_mask_addr, child_mask_n);
            }
        }

        // value pass
        if (constants.pass_id == 3u)
        {
            pnanovdb_uint32_t grid_type = pnanovdb_grid_get_grid_type(buf, grid);
            pnanovdb_tree_handle_t tree = pnanovdb_grid_get_tree(buf, grid);
            pnanovdb_root_handle_t root = pnanovdb_tree_get_root(buf, tree);

            uint level = 0u;
            pnanovdb_address_t val_addr = pnanovdb_root_get_value_address_and_level(
                grid_type, buf, root, ijk, level);
            if (level < 4)
            {
                // normalize
                sum.rgb *= (1.f / (1.f - sum.a));

                float4 color = float4(sum.rgb, 1.f - sum.a);
                uint rgba_raw = uint(255.f * max(0.f, min(1.f, color.x))) | (uint(255.f * max(0.f, min(1.f, color.y))) << 8u) |
                                (uint(255.f * max(0.f, min(1.f, color.z))) << 16u) |
                                (uint(255.f * max(0.f, min(1.f, color.w))) << 24u);
                pnanovdb_write_uint32(buf, val_addr, rgba_raw);
            }
        }
    }
}
