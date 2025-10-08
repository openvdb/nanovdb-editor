
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <vector>
#include <map>
#include <unordered_map>
#include <algorithm>
#include <math.h>

#include "nanovdb_editor/putil/ThreadPool.hpp"

#include "Raster.h"

#include <nanovdb/NanoVDB.h>
#include <nanovdb/tools/GridBuilder.h>
#include <nanovdb/tools/CreateNanoGrid.h>
#include <nanovdb/io/IO.h>

#define PNANOVDB_C
#define PNANOVDB_CMATH
#include <nanovdb/PNanoVDB.h>

namespace pnanovdb_raster
{

static uint32_t float_to_sort_key(float v)
{
    uint32_t raw = pnanovdb_float_as_uint32(v);
    uint32_t mask = -int(raw >> 31) | 0x80000000;
    return raw ^ mask;
}

struct vec4
{
    float x, y, z, w;
};
struct mat4x4
{
    vec4 x, y, z, w;
};
static vec4 mul(vec4 v, mat4x4 m)
{
    vec4 ret = {};
    ret.x = v.x * m.x.x + v.y * m.y.x + v.z * m.z.x + v.w * m.w.x;
    ret.y = v.x * m.x.y + v.y * m.y.y + v.z * m.z.y + v.w * m.w.y;
    ret.z = v.x * m.x.z + v.y * m.y.z + v.z * m.z.z + v.w * m.w.z;
    ret.w = v.x * m.x.w + v.y * m.y.w + v.z * m.z.w + v.w * m.w.w;
    return ret;
}
static mat4x4 mul(mat4x4 a, mat4x4 b)
{
    mat4x4 ret = {
        mul(a.x, b),
        mul(a.y, b),
        mul(a.z, b),
        mul(a.w, b)
    };
    return ret;
}
static mat4x4 transpose(mat4x4 m)
{
    mat4x4 ret = {
        m.x.x, m.y.x, m.z.x, m.w.x,
        m.x.y, m.y.y, m.z.y, m.w.y,
        m.x.z, m.y.z, m.z.z, m.w.z,
        m.x.w, m.y.w, m.z.w, m.w.w
    };
    return ret;
}

mat4x4 quat_to_mat(vec4 quatf)
{
    vec4 rot0 = vec4{
        1.f - 2.f * (quatf.y * quatf.y + quatf.z * quatf.z),
        2.f * (quatf.x * quatf.y - quatf.z * quatf.w),
        2.f * (quatf.x * quatf.z + quatf.y * quatf.w),
        0.f };
    vec4 rot1 = vec4{
        2.f * (quatf.x * quatf.y + quatf.z * quatf.w),
        1.f - 2.f * (quatf.x * quatf.x + quatf.z * quatf.z),
        2.f * (quatf.y * quatf.z - quatf.x * quatf.w),
        0.f };
    vec4 rot2 = vec4{
        2.f * (quatf.x * quatf.z - quatf.y * quatf.w),
        2.f * (quatf.y * quatf.z + quatf.x * quatf.w),
        1.f - 2.f * (quatf.x * quatf.x + quatf.y * quatf.y),
        0.f };
    mat4x4 ret = {
        rot0,
        rot1,
        rot2,
        {0.f, 0.f, 0.f, 1.f}
    };
    return ret;
}

mat4x4 quat_and_scale_to_mat(vec4 quatf, vec4 scalef)
{
    mat4x4 R = quat_to_mat(quatf);
    mat4x4 S = {
        {scalef.x, 0.f, 0.f, 0.f},
        {0.f, scalef.y, 0.f, 0.f},
        {0.f, 0.f, scalef.z, 0.f},
        {0.f, 0.f, 0.f, 1.f}
    };
    mat4x4 M = mul(R, S);
    return mul(M, transpose(M));
}

mat4x4 covar_world_to_cam(mat4x4 R, mat4x4 covar)
{
    return mul(mul(R, covar), transpose(R));
}

vec4 compute_conic(float posf[3], float quatf[4], float scalef[3], float opacity, int axis)
{
    mat4x4 view_x = {
        {0.f, 0.f, 1.f, 0.f},
        {0.f, 1.f, 0.f, 0.f},
        {1.f, 0.f, 0.f, 0.f},
        {0.f, 0.f, 0.f, 1.f}
    };
    mat4x4 view_y = {
        {1.f, 0.f, 0.f, 0.f},
        {0.f, 0.f, 1.f, 0.f},
        {0.f, 1.f, 0.f, 0.f},
        {0.f, 0.f, 0.f, 1.f}
    };
    mat4x4 view_z = {
        {1.f, 0.f, 0.f, 0.f},
        {0.f, 1.f, 0.f, 0.f},
        {0.f, 0.f, 1.f, 0.f},
        {0.f, 0.f, 0.f, 1.f}
    };

    mat4x4 view = view_z;
    if (axis == 0)
    {
        view = view_x;
    }
    else if (axis == 1)
    {
        view = view_y;
    }

    vec4 mean = {posf[0], posf[1], posf[2], 1.f};
    vec4 quat = {quatf[0], quatf[1], quatf[2], quatf[3]};
    vec4 scale = {scalef[0], scalef[1], scalef[2], 0.f};

    vec4 mean_c = mul(mean, view);

    mat4x4 covar = quat_and_scale_to_mat(quat, scale);
    mat4x4 covar_c = covar_world_to_cam(view, covar);

    float w = 2048.f;
    float fx = w * 0.5f * 1.f;
    float fy = w * 0.5f * 1.f;
    mat4x4 J = {
        {fx, 0.f, 0.f, 0.f},
        {0.f, fy, 0.f, 0.f},
        {0.f, 0.f, 0.f, 0.f},
        {0.f, 0.f, 0.f, 0.f}
    };

    mat4x4 cov2d = mul(mul(J, covar_c), transpose(J));

    const float eps2d = 0.3f;
    cov2d.x.x += eps2d;
    cov2d.y.y += eps2d;

    vec4 cov2d_inv;
    float det = cov2d.x.x * cov2d.y.y - cov2d.x.y * cov2d.y.x;
    float det_inv = 1.f / det;
    cov2d_inv.x = cov2d.y.y * det_inv;
    cov2d_inv.y = -cov2d.x.y * det_inv;
    cov2d_inv.z = -cov2d.y.x * det_inv;
    cov2d_inv.w = cov2d.x.x * det_inv;

    vec4 conic = {
        cov2d_inv.x,
        cov2d_inv.y,
        cov2d_inv.w,
        0.f
    };

    // move conic to camera space
    float conic_scale = w * 0.5f * 1.f;
    conic.x *= conic_scale * conic_scale;
    conic.y *= conic_scale * conic_scale;
    conic.z *= conic_scale * conic_scale;

    return conic;
}

static bool voxel_ellipsoid_overlap(
    gaussian_data_t* data, size_t idx,
    float scale, float offset[3],
    int32_t ijk[3], uint32_t level)
{
    float posf[3] = {
        ((const float*)data->means_cpu_array->data)[3u * idx + 0u],
        ((const float*)data->means_cpu_array->data)[3u * idx + 1u],
        ((const float*)data->means_cpu_array->data)[3u * idx + 2u]
    };
    float quatf[4] = {
        ((const float*)data->quaternions_cpu_array->data)[4u * idx + 1u],
        ((const float*)data->quaternions_cpu_array->data)[4u * idx + 2u],
        ((const float*)data->quaternions_cpu_array->data)[4u * idx + 3u],
        ((const float*)data->quaternions_cpu_array->data)[4u * idx + 0u]
    };
    float scalef[3] = {
        ((const float*)data->scales_cpu_array->data)[3u * idx + 0u],
        ((const float*)data->scales_cpu_array->data)[3u * idx + 1u],
        ((const float*)data->scales_cpu_array->data)[3u * idx + 2u],
    };
    float opacity = ((const float*)data->opacities_cpu_array->data)[idx];

    float local_scale = sqrtf(2.f * logf(fmaxf(0.01f, opacity) / 0.01f));

    float voxel_min[3];
    float voxel_max[3];
    for (uint vert_idx = 0u; vert_idx < 8u; vert_idx++)
    {
        float voxel_pos[3] = {float(ijk[0]), float(ijk[1]), float(ijk[2])};
        if ((vert_idx & 1u) != 0u) { voxel_pos[0] += float(1u << level);}
        if ((vert_idx & 2u) != 0u) { voxel_pos[1] += float(1u << level);}
        if ((vert_idx & 4u) != 0u) { voxel_pos[2] += float(1u << level);}

        float local_pos[3] = {
            (voxel_pos[0] - offset[0]) / scale,
            (voxel_pos[1] - offset[1]) / scale,
            (voxel_pos[2] - offset[2]) / scale
        };

        local_pos[0] -= posf[0];
        local_pos[1] -= posf[1];
        local_pos[2] -= posf[2];

        float rot0_inv[3] = {
            1.f - 2.f * (quatf[1] * quatf[1] + quatf[2] * quatf[2]),
            2.f * (quatf[0] * quatf[1] + quatf[2] * quatf[3]),
            2.f * (quatf[0] * quatf[2] - quatf[1] * quatf[3])};
        float rot1_inv[3] = {
            2.f * (quatf[0] * quatf[1] - quatf[2] * quatf[3]),
            1.f - 2.f * (quatf[0] * quatf[0] + quatf[2] * quatf[2]),
            2.f * (quatf[1] * quatf[2] + quatf[0] * quatf[3])};
        float rot2_inv[3] = {
            2.f * (quatf[0] * quatf[2] + quatf[1] * quatf[3]),
            2.f * (quatf[1] * quatf[2] - quatf[0] * quatf[3]),
            1.f - 2.f * (quatf[0] * quatf[0] + quatf[1] * quatf[1])};

        float local_pos_rot[3];
        local_pos_rot[0] = local_pos[0] * rot0_inv[0] + local_pos[1] * rot0_inv[1] + local_pos[2] * rot0_inv[2];
        local_pos_rot[1] = local_pos[0] * rot1_inv[0] + local_pos[1] * rot1_inv[1] + local_pos[2] * rot1_inv[2];
        local_pos_rot[2] = local_pos[0] * rot2_inv[0] + local_pos[1] * rot2_inv[1] + local_pos[2] * rot2_inv[2];

        local_pos_rot[0] *= (1.f / (local_scale * scalef[0]));
        local_pos_rot[1] *= (1.f / (local_scale * scalef[1]));
        local_pos_rot[2] *= (1.f / (local_scale * scalef[2]));

        if (vert_idx == 0u)
        {
            voxel_min[0] = local_pos_rot[0];
            voxel_min[1] = local_pos_rot[1];
            voxel_min[2] = local_pos_rot[2];
            voxel_max[0] = local_pos_rot[0];
            voxel_max[1] = local_pos_rot[1];
            voxel_max[2] = local_pos_rot[2];
        }
        else
        {
            voxel_min[0] = fminf(voxel_min[0], local_pos_rot[0]);
            voxel_min[1] = fminf(voxel_min[1], local_pos_rot[1]);
            voxel_min[2] = fminf(voxel_min[2], local_pos_rot[2]);
            voxel_max[0] = fmaxf(voxel_max[0], local_pos_rot[0]);
            voxel_max[1] = fmaxf(voxel_max[1], local_pos_rot[1]);
            voxel_max[2] = fmaxf(voxel_max[2], local_pos_rot[2]);
        }
    }
    float dist2 = 1.f;
    if (0.f < voxel_min[0]) { dist2 -= voxel_min[0] * voxel_min[0];}
    else if (0.f > voxel_max[0]) { dist2 -= voxel_max[0] * voxel_max[0];}
    if (0.f < voxel_min[1]) { dist2 -= voxel_min[1] * voxel_min[1];}
    else if (0.f > voxel_max[1]) { dist2 -= voxel_max[1] * voxel_max[1];}
    if (0.f < voxel_min[2]) { dist2 -= voxel_min[2] * voxel_min[2];}
    else if (0.f > voxel_max[2]) { dist2 -= voxel_max[2] * voxel_max[2];}
    return dist2 > 0.f;
}

void raster_octree_build(pnanovdb_raster_gaussian_data_t* data_in)
{
    gaussian_data_t* data = cast(data_in);

    size_t input_count = data->point_count;

    std::vector<float> world_bboxes(6u * input_count);
    for (size_t idx = 0u; idx < input_count; idx++)
    {
        float posf[3] = {
            ((const float*)data->means_cpu_array->data)[3u * idx + 0u],
            ((const float*)data->means_cpu_array->data)[3u * idx + 1u],
            ((const float*)data->means_cpu_array->data)[3u * idx + 2u]
        };
        float quatf[4] = {
            ((const float*)data->quaternions_cpu_array->data)[4u * idx + 1u],
            ((const float*)data->quaternions_cpu_array->data)[4u * idx + 2u],
            ((const float*)data->quaternions_cpu_array->data)[4u * idx + 3u],
            ((const float*)data->quaternions_cpu_array->data)[4u * idx + 0u]
        };
        float scalef[3] = {
            ((const float*)data->scales_cpu_array->data)[3u * idx + 0u],
            ((const float*)data->scales_cpu_array->data)[3u * idx + 1u],
            ((const float*)data->scales_cpu_array->data)[3u * idx + 2u],
        };
        float opacity = ((const float*)data->opacities_cpu_array->data)[idx];

        float local_scale = sqrtf(2.f * logf(fmaxf(0.01f, opacity) / 0.01f));

        float aabb_min[3] = {};
        float aabb_max[3] = {};
        for (uint32_t vert_idx = 0u; vert_idx < 8u; vert_idx++)
        {
            float cube_pos[3] = {
                (vert_idx & 1u) == 0 ? -1.f : 1.f,
                (vert_idx & 2u) == 0 ? -1.f : 1.f,
                (vert_idx & 4u) == 0 ? -1.f : 1.f
            };
            cube_pos[0] *= scalef[0] * local_scale;
            cube_pos[1] *= scalef[1] * local_scale;
            cube_pos[2] *= scalef[2] * local_scale;

            float rot0[3] = {
                1.f - 2.f * (quatf[1] * quatf[1] + quatf[2] * quatf[2]),
                2.f * (quatf[0] * quatf[1] - quatf[2] * quatf[3]),
                2.f * (quatf[0] * quatf[2] + quatf[1] * quatf[3])};
            float rot1[3] = {
                2.f * (quatf[0] * quatf[1] + quatf[2] * quatf[3]),
                1.f - 2.f * (quatf[0] * quatf[0] + quatf[2] * quatf[2]),
                2.f * (quatf[1] * quatf[2] - quatf[0] * quatf[3])};
            float rot2[3] = {
                2.f * (quatf[0] * quatf[2] - quatf[1] * quatf[3]),
                2.f * (quatf[1] * quatf[2] + quatf[0] * quatf[3]),
                1.f - 2.f * (quatf[0] * quatf[0] + quatf[1] * quatf[1])};

            float cube_pos_world[3];
            cube_pos_world[0] = cube_pos[0] * rot0[0] + cube_pos[1] * rot0[1] + cube_pos[2] * rot0[2] + posf[0];
            cube_pos_world[1] = cube_pos[0] * rot1[0] + cube_pos[1] * rot1[1] + cube_pos[2] * rot1[2] + posf[1];
            cube_pos_world[2] = cube_pos[0] * rot2[0] + cube_pos[1] * rot2[1] + cube_pos[2] * rot2[2] + posf[2];

            if (vert_idx == 0u)
            {
                aabb_min[0] = cube_pos_world[0];
                aabb_min[1] = cube_pos_world[1];
                aabb_min[2] = cube_pos_world[2];
                aabb_max[0] = cube_pos_world[0];
                aabb_max[1] = cube_pos_world[1];
                aabb_max[2] = cube_pos_world[2];
            }
            else
            {
                aabb_min[0] = fminf(aabb_min[0], cube_pos_world[0]);
                aabb_min[1] = fminf(aabb_min[1], cube_pos_world[1]);
                aabb_min[2] = fminf(aabb_min[2], cube_pos_world[2]);
                aabb_max[0] = fmaxf(aabb_max[0], cube_pos_world[0]);
                aabb_max[1] = fmaxf(aabb_max[1], cube_pos_world[1]);
                aabb_max[2] = fmaxf(aabb_max[2], cube_pos_world[2]);
            }
        }
        world_bboxes[6u * idx + 0u] = aabb_min[0];
        world_bboxes[6u * idx + 1u] = aabb_min[1];
        world_bboxes[6u * idx + 2u] = aabb_min[2];
        world_bboxes[6u * idx + 3u] = aabb_max[0];
        world_bboxes[6u * idx + 4u] = aabb_max[1];
        world_bboxes[6u * idx + 5u] = aabb_max[2];
    }

    float world_bbox_min[3] = {INFINITY, INFINITY, INFINITY};
    float world_bbox_max[3] = {-INFINITY, -INFINITY, -INFINITY};

    for (size_t idx = 0u; idx < data->point_count; idx++)
    {
        world_bbox_min[0] = fminf(world_bbox_min[0], world_bboxes[6u * idx + 0u]);
        world_bbox_min[1] = fminf(world_bbox_min[1], world_bboxes[6u * idx + 1u]);
        world_bbox_min[2] = fminf(world_bbox_min[2], world_bboxes[6u * idx + 2u]);
        world_bbox_max[0] = fmaxf(world_bbox_max[0], world_bboxes[6u * idx + 3u]);
        world_bbox_max[1] = fmaxf(world_bbox_max[1], world_bboxes[6u * idx + 4u]);
        world_bbox_max[2] = fmaxf(world_bbox_max[2], world_bboxes[6u * idx + 5u]);
    }

    printf("World bbox((%f,%f,%f):(%f,%f,%f))\n",
        world_bbox_min[0], world_bbox_min[1], world_bbox_min[2],
        world_bbox_max[0], world_bbox_max[1], world_bbox_max[2]);

    static const uint32_t integer_space_max = 4095;
    // map bbox into 0,integer_space_max range
    float world_delta[3] = {
        world_bbox_max[0] - world_bbox_min[0],
        world_bbox_max[1] - world_bbox_min[1],
        world_bbox_max[2] - world_bbox_min[2]
    };
    float world_max_delta = fmaxf(world_delta[0], fmaxf(world_delta[1], world_delta[2]));
    float scale = float(integer_space_max) / (world_max_delta);
    float offset[3] = {
        -world_bbox_min[0] * scale,
        -world_bbox_min[1] * scale,
        -world_bbox_min[2] * scale
    };

    std::vector<int32_t> bboxes(6u * input_count);
    for (size_t idx = 0u; idx < input_count; idx++)
    {
        bboxes[6u * idx + 0] = floorf(scale * world_bboxes[6u * idx + 0u] + offset[0]);
        bboxes[6u * idx + 1] = floorf(scale * world_bboxes[6u * idx + 1u] + offset[1]);
        bboxes[6u * idx + 2] = floorf(scale * world_bboxes[6u * idx + 2u] + offset[2]);
        bboxes[6u * idx + 3] = floorf(scale * world_bboxes[6u * idx + 3u] + offset[0]);
        bboxes[6u * idx + 4] = floorf(scale * world_bboxes[6u * idx + 4u] + offset[1]);
        bboxes[6u * idx + 5] = floorf(scale * world_bboxes[6u * idx + 5u] + offset[2]);
    }

    int32_t ijk_bbox[6] = {
        bboxes[0], bboxes[1], bboxes[2],
        bboxes[3], bboxes[4], bboxes[5]
    };
    for (size_t idx = 0u; idx < input_count; idx++)
    {
        if (bboxes[6u * idx + 0u] < ijk_bbox[0]) {ijk_bbox[0] = bboxes[6u * idx + 0u];}
        if (bboxes[6u * idx + 1u] < ijk_bbox[1]) {ijk_bbox[1] = bboxes[6u * idx + 1u];}
        if (bboxes[6u * idx + 2u] < ijk_bbox[2]) {ijk_bbox[2] = bboxes[6u * idx + 2u];}
        if (bboxes[6u * idx + 4u] > ijk_bbox[4]) {ijk_bbox[4] = bboxes[6u * idx + 4u];}
        if (bboxes[6u * idx + 5u] > ijk_bbox[5]) {ijk_bbox[5] = bboxes[6u * idx + 5u];}
        if (bboxes[6u * idx + 6u] > ijk_bbox[6]) {ijk_bbox[6] = bboxes[6u * idx + 6u];}
    }
    printf("ijk_bbox((%d,%d,%d):(%d,%d,%d))\n",
        ijk_bbox[0], ijk_bbox[1], ijk_bbox[2],
        ijk_bbox[3], ijk_bbox[4], ijk_bbox[5]);

    struct result_t
    {
        std::vector<uint64_t> tmp_keys;
        std::vector<uint64_t> tmp_bbox_ids;
    };
    pnanovdb_util::ThreadPool pool;
    std::vector<std::future<result_t>> futures;
    for (size_t idx = 0u; idx < input_count; idx++)
    {
        auto future = pool.enqueue([&bboxes, idx, data, scale, &offset]() -> result_t
            {
                result_t result = {};
                bool hit_max = false;
                for (uint32_t level = 15u; level < 16u; level--)
                {
                    int32_t bbox_level[6] = {
                        bboxes[6u * idx + 0u] & ~((1 << level) - 1),
                        bboxes[6u * idx + 1u] & ~((1 << level) - 1),
                        bboxes[6u * idx + 2u] & ~((1 << level) - 1),
                        bboxes[6u * idx + 3u] & ~((1 << level) - 1),
                        bboxes[6u * idx + 4u] & ~((1 << level) - 1),
                        bboxes[6u * idx + 5u] & ~((1 << level) - 1)
                    };
                    int32_t bbox_size[3] = {
                        (bbox_level[3] >> level) - (bbox_level[0] >> level) + 1,
                        (bbox_level[4] >> level) - (bbox_level[1] >> level) + 1,
                        (bbox_level[5] >> level) - (bbox_level[2] >> level) + 1
                    };
                    static const size_t max_aniso = 8u;
                    result.tmp_keys.clear();
                    result.tmp_bbox_ids.clear();
                    for (int32_t i = bbox_level[0]; i <= bbox_level[3]; i += (1u << level))
                    {
                        for (int32_t j = bbox_level[1]; j <= bbox_level[4]; j += (1u << level))
                        {
                            for (int32_t k = bbox_level[2]; k <= bbox_level[5]; k += (1u << level))
                            {
                                int32_t ijk[3] = {i,j,k};
                                bool is_overlap = voxel_ellipsoid_overlap(data, idx, scale, offset, ijk, level);
                                if (is_overlap)
                                {
                                    uint64_t key = (uint64_t(level) << 48u) | (uint64_t(i) << 32u) |
                                        (uint64_t(j) << 16u) | uint64_t(k);
                                    result.tmp_keys.push_back(key);
                                    result.tmp_bbox_ids.push_back(idx);
                                }
                            }
                        }
                    }
                    size_t key_count = result.tmp_keys.size();
                    if (!hit_max)
                    {
                        if (key_count > max_aniso)
                        {
                            level += 2u;
                            hit_max = true;
                        }
                        continue;
                    }
                    break;
                }
                return result;
            });
        futures.push_back(std::move(future));
    }
    printf("waiting on futures\n");
    for (auto& future : futures)
    {
        future.wait();
    }
    printf("processing futures\n");
    std::map<uint64_t, uint64_t> footprint_counts;
    size_t max_prim_key_count = 0u;
    std::vector<std::pair<uint64_t, uint64_t>> keys;
    for (auto& future : futures)
    {
        result_t result = future.get();
        size_t key_begin = keys.size();
        size_t key_end = keys.size() + result.tmp_keys.size();
        for (size_t tmp_idx = 0u; tmp_idx < result.tmp_keys.size(); tmp_idx++)
        {
            keys.push_back(std::pair<uint64_t, uint64_t>(result.tmp_keys[tmp_idx], result.tmp_bbox_ids[tmp_idx]));
        }
        if (result.tmp_keys.size() > max_prim_key_count)
        {
            max_prim_key_count = result.tmp_keys.size();
        }
        footprint_counts[result.tmp_keys.size()]++;
    }
    printf("total_keys(%zu) max_prim_key_count(%zu)\n", keys.size(), max_prim_key_count);
    for (auto& footprint : footprint_counts)
    {
        printf("footprint voxels(%llu) instances(%llu):\n",
            (unsigned long long int)footprint.first, (unsigned long long int)footprint.second);
    }

    std::sort(keys.begin(), keys.end(), [](const std::pair<uint64_t,uint64_t>& a, const std::pair<uint64_t,uint64_t>& b) {
        return a.first < b.first;
    });

    struct key_header_t
    {
        uint64_t begin_idx;
        uint64_t count;
        uint64_t parent_idx;
        uint64_t key;
    };
    std::vector<key_header_t> key_headers;

    key_header_t key_header = {};
    key_header.key = ~0llu;
    for (size_t idx = 0u; idx < keys.size(); idx++)
    {
        if (key_header.key != keys[idx].first)
        {
            key_headers.push_back(key_header);

            key_header.begin_idx = idx;
            key_header.count = 0u;
            key_header.parent_idx = 0u;
            key_header.key = keys[idx].first;
        }
        key_header.count++;
    }
    key_headers.push_back(key_header);
    printf("key_headers(%zu)\n", key_headers.size());

    std::unordered_map<uint64_t, uint64_t> key_header_map;
    for (size_t idx = 0u; idx < key_headers.size(); idx++)
    {
        key_header_map[key_headers[idx].key] = idx;
    }

    // link to parents
    size_t parent_link_counter = 0u;
    for (size_t idx = 0u; idx < key_headers.size(); idx++)
    {
        uint64_t key = key_headers[idx].key;
        for (uint32_t attempt = 0u; attempt < 16u; attempt++)
        {
            uint32_t level = uint32_t(key >> 48u) & 0xFFFF;
            uint32_t i = uint32_t(key >> 32u) & 0xFFFF;
            uint32_t j = uint32_t(key >> 16u) & 0xFFFF;
            uint32_t k = uint32_t(key) & 0xFFFF;
            if (level >= 15)
            {
                break;
            }
            level += 1u;
            i = i & ~((1 << level) - 1);
            j = j & ~((1 << level) - 1);
            k = k & ~((1 << level) - 1);
            uint64_t parent_key = (uint64_t(level) << 48u) |
                (uint64_t(i) << 32u) | (uint64_t(j) << 16u) | uint64_t(k);

            auto it = key_header_map.find(parent_key);
            if (it != key_header_map.end())
            {
                key_headers[idx].parent_idx = it->second;
                parent_link_counter++;
                break;
            }
        }
    }
    printf("parent_link_counter(%zu)\n", parent_link_counter);

    struct header_flat_t
    {
        uint32_t begin_idxs[12u];
        uint16_t counts[12u];
    };
    std::vector<header_flat_t> flat_headers(key_headers.size());

    std::map<uint64_t, uint64_t> parent_counts;
    for (size_t idx = 0u; idx < key_headers.size(); idx++)
    {
        header_flat_t flat = {};

        size_t parent_count = 0u;
        key_header_t current_header = key_headers[idx];
        for (uint32_t attempt = 0u; attempt < 16u; attempt++)
        {
            uint64_t level = current_header.key >> 48u;
            if (level < 12u)
            {
                flat.begin_idxs[level] = current_header.begin_idx;
                flat.counts[level] = (uint16_t)current_header.count;
            }
            if (current_header.parent_idx != 0llu)
            {
                parent_count++;
                current_header = key_headers[current_header.parent_idx];
            }
        }
        parent_counts[parent_count]++;

        flat_headers[idx] = flat;
    }
    for (auto& parent_count : parent_counts)
    {
        printf("key_header parent_count(%llu) instances(%llu):\n",
            (unsigned long long int)parent_count.first, (unsigned long long int)parent_count.second);
    }

    std::vector<std::pair<uint64_t, uint64_t>> tmp_keys;
    std::vector<uint32_t> gaussian_ids_axis[3];
    std::vector<uint32_t> sort_keys_axis[3];
    for (int axis = 0; axis < 3; axis++)
    {
        gaussian_ids_axis[axis].resize(keys.size());
        sort_keys_axis[axis].resize(keys.size());

        for (size_t idx = 0u; idx < key_headers.size(); idx++)
        {
            uint64_t begin_idx = key_headers[idx].begin_idx;
            uint64_t count = key_headers[idx].count;

            tmp_keys.clear();
            tmp_keys.resize(count);
            for (uint64_t idx = 0u; idx < count; idx++)
            {
                auto key = keys[begin_idx + idx];

                uint64_t gid = key.second;
                float posf[3] = {
                    ((const float*)data->means_cpu_array->data)[3u * gid + 0u],
                    ((const float*)data->means_cpu_array->data)[3u * gid + 1u],
                    ((const float*)data->means_cpu_array->data)[3u * gid + 2u]
                };

                key.first = (uint64_t)float_to_sort_key(posf[axis]);

                tmp_keys[idx] = key;
            }

            std::sort(tmp_keys.begin(), tmp_keys.end(), [](const std::pair<uint64_t,uint64_t>& a, const std::pair<uint64_t,uint64_t>& b) {
                return a.first < b.first;
            });

            for (uint64_t idx = 0u; idx < count; idx++)
            {
                sort_keys_axis[axis][begin_idx + idx] = (uint32_t)tmp_keys[idx].first;
                gaussian_ids_axis[axis][begin_idx + idx] = (uint32_t)tmp_keys[idx].second;
            }
        }
    }

    std::vector<float> conics_axis[3];
    for (int axis = 0; axis < 3; axis++)
    {
        conics_axis[axis].resize(3u * data->point_count);

        for (size_t idx = 0u; idx < data->point_count; idx++)
        {
            float posf[3] = {
                ((const float*)data->means_cpu_array->data)[3u * idx + 0u],
                ((const float*)data->means_cpu_array->data)[3u * idx + 1u],
                ((const float*)data->means_cpu_array->data)[3u * idx + 2u]
            };
            float quatf[4] = {
                ((const float*)data->quaternions_cpu_array->data)[4u * idx + 1u],
                ((const float*)data->quaternions_cpu_array->data)[4u * idx + 2u],
                ((const float*)data->quaternions_cpu_array->data)[4u * idx + 3u],
                ((const float*)data->quaternions_cpu_array->data)[4u * idx + 0u]
            };
            float scalef[3] = {
                ((const float*)data->scales_cpu_array->data)[3u * idx + 0u],
                ((const float*)data->scales_cpu_array->data)[3u * idx + 1u],
                ((const float*)data->scales_cpu_array->data)[3u * idx + 2u],
            };
            float opacity = ((const float*)data->opacities_cpu_array->data)[idx];

            vec4 conic = compute_conic(posf, quatf, scalef, opacity, axis);

            conics_axis[axis][3u * idx + 0u] = conic.x;
            conics_axis[axis][3u * idx + 1u] = conic.y;
            conics_axis[axis][3u * idx + 2u] = conic.z;
        }
    }

#if 1
    nanovdb::tools::build::UInt32Grid grid(0u, "gaussians");

    auto setValue = [&](nanovdb::Coord ijk, uint32_t level, uint64_t value)
    {
        if (level <= 3u)
        {
            for (int32_t i = 0u; i < (1u << level); i++)
            {
                for (int32_t j = 0u; j < (1u << level); j++)
                {
                    for (int32_t k = 0u; k < (1u << level); k++)
                    {
                        nanovdb::Coord ijk_tmp = ijk + nanovdb::Coord(i, j, k);
                        grid.tree().root().setValue(ijk_tmp, value);
                    }
                }
            }
        }
        else if (level <= 7u)
        {
            for (int32_t i = 0u; i < (1u << level); i+=8)
            {
                for (int32_t j = 0u; j < (1u << level); j+=8)
                {
                    for (int32_t k = 0u; k < (1u << level); k+=8)
                    {
                        nanovdb::Coord ijk_tmp = ijk + nanovdb::Coord(i, j, k);
                        grid.tree().root().addTile<1>(ijk_tmp, value, true);
                    }
                }
            }
        }
        else if (level <= 12u)
        {
            for (int32_t i = 0u; i < (1u << level); i+=128)
            {
                for (int32_t j = 0u; j < (1u << level); j+=128)
                {
                    for (int32_t k = 0u; k < (1u << level); k+=128)
                    {
                        nanovdb::Coord ijk_tmp = ijk + nanovdb::Coord(i, j, k);
                        grid.tree().root().addTile<2>(ijk_tmp, value, true);
                    }
                }
            }
        }
        else
        {
            for (int32_t i = 0u; i < (1u << level); i+=4096)
            {
                for (int32_t j = 0u; j < (1u << level); j+=4096)
                {
                    for (int32_t k = 0u; k < (1u << level); k+=4096)
                    {
                        nanovdb::Coord ijk_tmp = ijk + nanovdb::Coord(i, j, k);
                        grid.tree().root().addTile<3>(ijk_tmp, value, true);
                    }
                }
            }
        }
    };

    for (uint32_t level = 15u; level < 16u; level--)
    {
        for (size_t idx = 0u; idx < key_headers.size(); idx++)
        {
            uint64_t key = key_headers[idx].key;
            uint32_t key_level = uint32_t(key >> 48u) & 0xFFFF;
            uint32_t i = uint32_t(key >> 32u) & 0xFFFF;
            uint32_t j = uint32_t(key >> 16u) & 0xFFFF;
            uint32_t k = uint32_t(key) & 0xFFFF;

            if (level == key_level)
            {
                nanovdb::Coord ijk(i, j, k);
                setValue(ijk, level, idx);
            }
        }
    }

    grid.setTransform(1.f / scale,
        nanovdb::math::Vec3d(-offset[0] / scale, -offset[1] / scale, -offset[2] / scale));

    nanovdb::tools::CreateNanoGrid<nanovdb::tools::build::UInt32Grid> grid_built(grid);

    grid_built.addBlindData("gaussian_flat_headers",
        nanovdb::GridBlindDataSemantic::Unknown,
        nanovdb::GridBlindDataClass::Unknown,
        nanovdb::GridType::UInt32, (sizeof(header_flat_t) / 4u) * flat_headers.size(), 4u);
    grid_built.addBlindData("gaussian_ids",
        nanovdb::GridBlindDataSemantic::Unknown,
        nanovdb::GridBlindDataClass::Unknown,
        nanovdb::GridType::UInt32, keys.size(), 4u);
    grid_built.addBlindData("means",
        nanovdb::GridBlindDataSemantic::Unknown,
        nanovdb::GridBlindDataClass::Unknown,
        nanovdb::GridType::Float, 3u * data->point_count, 4u);
    grid_built.addBlindData("quaternions",
        nanovdb::GridBlindDataSemantic::Unknown,
        nanovdb::GridBlindDataClass::Unknown,
        nanovdb::GridType::Float, 4u * data->point_count, 4u);
    grid_built.addBlindData("scales",
        nanovdb::GridBlindDataSemantic::Unknown,
        nanovdb::GridBlindDataClass::Unknown,
        nanovdb::GridType::Float, 3u * data->point_count, 4u);
    grid_built.addBlindData("spherical_harmonics",
        nanovdb::GridBlindDataSemantic::Unknown,
        nanovdb::GridBlindDataClass::Unknown,
        nanovdb::GridType::Float, 48u * data->point_count, 4u);
    grid_built.addBlindData("opacities",
        nanovdb::GridBlindDataSemantic::Unknown,
        nanovdb::GridBlindDataClass::Unknown,
        nanovdb::GridType::Float, 1u * data->point_count, 4u);
    grid_built.addBlindData("gaussian_header_ids",
        nanovdb::GridBlindDataSemantic::Unknown,
        nanovdb::GridBlindDataClass::Unknown,
        nanovdb::GridType::UInt32, keys.size(), 4u);
    grid_built.addBlindData("sort_keys",
        nanovdb::GridBlindDataSemantic::Unknown,
        nanovdb::GridBlindDataClass::Unknown,
        nanovdb::GridType::UInt32, keys.size(), 4u);
    grid_built.addBlindData("conics",
        nanovdb::GridBlindDataSemantic::Unknown,
        nanovdb::GridBlindDataClass::Unknown,
        nanovdb::GridType::Float, 3u * data->point_count, 4u);
    grid_built.addBlindData("colors",
        nanovdb::GridBlindDataSemantic::Unknown,
        nanovdb::GridBlindDataClass::Unknown,
        nanovdb::GridType::Float, 3u * data->point_count, 4u);
    grid_built.addBlindData("gaussian_ids_x",
        nanovdb::GridBlindDataSemantic::Unknown,
        nanovdb::GridBlindDataClass::Unknown,
        nanovdb::GridType::UInt32, keys.size(), 4u);
    grid_built.addBlindData("gaussian_ids_y",
        nanovdb::GridBlindDataSemantic::Unknown,
        nanovdb::GridBlindDataClass::Unknown,
        nanovdb::GridType::UInt32, keys.size(), 4u);
    grid_built.addBlindData("gaussian_ids_z",
        nanovdb::GridBlindDataSemantic::Unknown,
        nanovdb::GridBlindDataClass::Unknown,
        nanovdb::GridType::UInt32, keys.size(), 4u);
    grid_built.addBlindData("sort_keys_x",
        nanovdb::GridBlindDataSemantic::Unknown,
        nanovdb::GridBlindDataClass::Unknown,
        nanovdb::GridType::UInt32, keys.size(), 4u);
    grid_built.addBlindData("sort_keys_y",
        nanovdb::GridBlindDataSemantic::Unknown,
        nanovdb::GridBlindDataClass::Unknown,
        nanovdb::GridType::UInt32, keys.size(), 4u);
    grid_built.addBlindData("sort_keys_z",
        nanovdb::GridBlindDataSemantic::Unknown,
        nanovdb::GridBlindDataClass::Unknown,
        nanovdb::GridType::UInt32, keys.size(), 4u);
    grid_built.addBlindData("conics_x",
        nanovdb::GridBlindDataSemantic::Unknown,
        nanovdb::GridBlindDataClass::Unknown,
        nanovdb::GridType::Float, 3u * data->point_count, 4u);
    grid_built.addBlindData("conics_y",
        nanovdb::GridBlindDataSemantic::Unknown,
        nanovdb::GridBlindDataClass::Unknown,
        nanovdb::GridType::Float, 3u * data->point_count, 4u);
    grid_built.addBlindData("conics_z",
        nanovdb::GridBlindDataSemantic::Unknown,
        nanovdb::GridBlindDataClass::Unknown,
        nanovdb::GridType::Float, 3u * data->point_count, 4u);
    grid_built.addBlindData("gaussians_headers",
        nanovdb::GridBlindDataSemantic::Unknown,
        nanovdb::GridBlindDataClass::Unknown,
        nanovdb::GridType::UInt32, 5u * key_headers.size(), 4u);

    nanovdb::GridHandle<nanovdb::HostBuffer> grid_handle = grid_built.getHandle<uint32_t>();

    auto grid_ptr = grid_handle.grid<uint32_t>();

    uint32_t* blind_flat_headers = grid_ptr->getBlindData<uint32_t>(0u);
    for (size_t idx = 0u; idx < key_headers.size(); idx++)
    {
        memcpy(blind_flat_headers + (sizeof(header_flat_t) / 4u) * idx, &flat_headers[idx], sizeof(header_flat_t));
    }
    uint32_t* blind_ids = grid_ptr->getBlindData<uint32_t>(1u);
    for (size_t idx = 0u; idx < keys.size(); idx++)
    {
        blind_ids[idx] = keys[idx].second;
    }
    float* blind_means = grid_ptr->getBlindData<float>(2u);
    for (size_t idx = 0u; idx < 3u * data->point_count; idx++)
    {
        blind_means[idx] = ((const float*)(data->means_cpu_array->data))[idx];
    }
    float* blind_quats = grid_ptr->getBlindData<float>(3u);
    for (size_t idx = 0u; idx < 4u * data->point_count; idx++)
    {
        blind_quats[idx] = ((const float*)(data->quaternions_cpu_array->data))[idx];
    }
    float* blind_scales = grid_ptr->getBlindData<float>(4u);
    for (size_t idx = 0u; idx < 3u * data->point_count; idx++)
    {
        blind_scales[idx] = ((const float*)(data->scales_cpu_array->data))[idx];
    }
    float* blind_shs = grid_ptr->getBlindData<float>(5u);
    for (size_t idx = 0u; idx < 48u * data->point_count; idx++)
    {
        blind_shs[idx] = ((const float*)(data->spherical_harmonics_cpu_array->data))[idx];
    }
    float* blind_opacities = grid_ptr->getBlindData<float>(6u);
    for (size_t idx = 0u; idx < 1u * data->point_count; idx++)
    {
        blind_opacities[idx] = ((const float*)(data->opacities_cpu_array->data))[idx];
    }
    uint32_t* blind_header_ids = grid_ptr->getBlindData<uint32_t>(7u);
    for (size_t idx = 0u; idx < keys.size(); idx++)
    {
        blind_header_ids[idx] = (uint32_t)(key_header_map[keys[idx].first]);
    }
    uint32_t* blind_sort_keys = grid_ptr->getBlindData<uint32_t>(8u);
    for (size_t idx = 0u; idx < keys.size(); idx++)
    {
        blind_sort_keys[idx] = 0u;
    }
    float* blind_conics = grid_ptr->getBlindData<float>(9u);
    for (size_t idx = 0u; idx < 3u * data->point_count; idx++)
    {
        blind_conics[idx] = 0.f;
    }
    float* blind_colors = grid_ptr->getBlindData<float>(10u);
    for (size_t idx = 0u; idx < 3u * data->point_count; idx++)
    {
        blind_colors[idx] = 0.f;
    }
    uint32_t* blind_ids_x = grid_ptr->getBlindData<uint32_t>(11u);
    for (size_t idx = 0u; idx < keys.size(); idx++)
    {
        blind_ids_x[idx] = gaussian_ids_axis[0][idx];
    }
    uint32_t* blind_ids_y = grid_ptr->getBlindData<uint32_t>(12u);
    for (size_t idx = 0u; idx < keys.size(); idx++)
    {
        blind_ids_y[idx] = gaussian_ids_axis[1][idx];
    }
    uint32_t* blind_ids_z = grid_ptr->getBlindData<uint32_t>(13u);
    for (size_t idx = 0u; idx < keys.size(); idx++)
    {
        blind_ids_z[idx] = gaussian_ids_axis[2][idx];
    }
    uint32_t* blind_sort_keys_x = grid_ptr->getBlindData<uint32_t>(14u);
    for (size_t idx = 0u; idx < keys.size(); idx++)
    {
        blind_sort_keys_x[idx] = sort_keys_axis[0][idx];
    }
    uint32_t* blind_sort_keys_y = grid_ptr->getBlindData<uint32_t>(15u);
    for (size_t idx = 0u; idx < keys.size(); idx++)
    {
        blind_sort_keys_y[idx] = sort_keys_axis[1][idx];
    }
    uint32_t* blind_sort_keys_z = grid_ptr->getBlindData<uint32_t>(16u);
    for (size_t idx = 0u; idx < keys.size(); idx++)
    {
        blind_sort_keys_z[idx] = sort_keys_axis[2][idx];
    }
    float* blind_conics_x = grid_ptr->getBlindData<float>(17u);
    for (size_t idx = 0u; idx < 3u * data->point_count; idx++)
    {
        blind_conics_x[idx] = conics_axis[0][idx];
    }
    float* blind_conics_y = grid_ptr->getBlindData<float>(18u);
    for (size_t idx = 0u; idx < 3u * data->point_count; idx++)
    {
        blind_conics_y[idx] = conics_axis[1][idx];
    }
    float* blind_conics_z = grid_ptr->getBlindData<float>(19u);
    for (size_t idx = 0u; idx < 3u * data->point_count; idx++)
    {
        blind_conics_z[idx] = conics_axis[2][idx];
    }
    uint32_t* blind_headers = grid_ptr->getBlindData<uint32_t>(20u);
    for (size_t idx = 0u; idx < key_headers.size(); idx++)
    {
        blind_headers[5u * idx + 0u] = (uint32_t)key_headers[idx].begin_idx;
        blind_headers[5u * idx + 1u] = (uint32_t)key_headers[idx].count;
        blind_headers[5u * idx + 2u] = (uint32_t)(key_headers[idx].key);
        blind_headers[5u * idx + 3u] = (uint32_t)(key_headers[idx].key >> 32u);
        blind_headers[5u * idx + 4u] = (uint32_t)key_headers[idx].parent_idx;
    }

    static const char* vdb_path = "./data/octree.nvdb";
    try
    {
        nanovdb::io::writeGrid(vdb_path, grid_handle);
    }
    catch (const std::ios_base::failure& e)
    {
        printf("Error: Could not save nanovdb '%s' (%s)\n", vdb_path, e.what());
    }
#endif

    std::map<uint64_t, uint64_t> bucket_counts;
    for (size_t idx = 0u; idx < keys.size(); idx++)
    {
        uint64_t key = keys[idx].first;
        bucket_counts[key]++;
    }

    std::map<uint64_t, std::map<uint64_t, uint64_t>> histogram;
    for (auto& entry : bucket_counts)
    {
        uint64_t level = (entry.first >> 48u);
        histogram[level][entry.second]++;
    }

    size_t total_bucket_count = 0u;
    for (auto& histogram_level : histogram)
    {
        printf("histogram_level(%llu):\n", (unsigned long long int)histogram_level.first);
        int line_counter = 0;
        for (auto& entry : histogram_level.second)
        {
            total_bucket_count += entry.second;
            printf("[%llu](%llu),",
                (unsigned long long int)entry.first,
                (unsigned long long int)entry.second);
            line_counter++;
            if ((line_counter & 15) == 0)
            {
                printf("\n");
            }
        }
        printf("\n");
    }
    printf("total_bucket_count(%zu)\n", total_bucket_count);
}

}
