// gaussian.slang
#include "editor_params.slang"

struct shader_params_t
{
    float3 mean;
    float opacity;
    float4 quat;
    float3 scale;
    float pad0;
    float3 sh0;
    float pad1;
};

StructuredBuffer<uint2> buf;
RWStructuredBuffer<uint> image_out_deprecated;
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

void ray_intersect(float3 worldRayOrigin,
                   float rayMinT,
                   float3 worldRayDir,
                   float rayMaxT,
                   float3 worldRayDirInv,
                   inout float4 sum,
                   inout float nominalT)
{
    float3 aabb_min = {};
    float3 aabb_max = {};
    compute_aabb(
        shader_params.mean,
        normalize(shader_params.quat),
        shader_params.scale,
        shader_params.opacity,
        aabb_min,
        aabb_max);

    float2 box_trange = {};
    bool box_hit = intersect_box(
        worldRayDir,
        worldRayDirInv,
        aabb_min - worldRayOrigin,
        aabb_max - worldRayOrigin,
        box_trange.x,
        box_trange.y);
    if (box_hit)
    {
        sum.g += 0.3f;
    }

    float3 conic = {};
    project_conic(
        shader_params.mean,
        normalize(shader_params.quat),
        shader_params.scale,
        conic
    );

    float3 color = 0.2820947917738781f * shader_params.sh0 + 0.5f;

    // clip by box
    float gaussian_min_t = box_trange.x;
    float gaussian_max_t = box_trange.y;

    intersect_gaussian(
        shader_params.mean,
        conic, color,
        shader_params.opacity,
        worldRayOrigin, gaussian_min_t,
        worldRayDir, gaussian_max_t,
        worldRayDirInv,
        sum, nominalT
    );
}

[shader("compute")][numthreads(16, 8, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID, uint3 thread_idx : SV_GroupThreadID)
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
    float3 rayDirInv = float3(1.f, 1.f, 1.f) / rayDir;

    float4 rayOrigin4 = is_reverse_z ? pos_d1 : pos_d0;
    rayOrigin4 = mul(rayOrigin4, editor_params.view_inv);
    float3 rayOrigin = rayOrigin4.xyz / rayOrigin4.w;

    float4 sum = float4(0.f, 0.f, 0.f, 1.f);
    float nominalT = 0.f;
    ray_intersect(rayOrigin, 0.f, rayDir, 1e9f, rayDirInv, sum, nominalT);

    texture_out[tidx] = sum;
}
