// nanovdb_prepass.slang
#define PNANOVDB_HLSL
#define PNANOVDB_ADDRESS_64
#define PNANOVDB_BUF_HLSL_64
#define PNANOVDB_BUF_HLSL_RW
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

RWStructuredBuffer<uint2> buf;
RWStructuredBuffer<uint> image_out;
RWTexture2D<float4> texture_out;
ConstantBuffer<EditorParams> editor_params;
ConstantBuffer<ShaderParams> shader_params;

#define BLOCK_DIM 256

groupshared uint skey[4 * BLOCK_DIM];
groupshared uint sval1[4 * BLOCK_DIM];
groupshared uint sval2[4 * BLOCK_DIM];
groupshared uint sdata0[BLOCK_DIM];
groupshared uint sdata1[BLOCK_DIM];

uint4 blockScan(uint threadIdx, uint4 val, out uint totalCount)
{
    uint localVal = val.x + val.y + val.z + val.w;
    sdata0[threadIdx] = localVal;

    GroupMemoryBarrierWithGroupSync();

    if (threadIdx >= 1) localVal += sdata0[threadIdx - 1];
    if (threadIdx >= 2) localVal += sdata0[threadIdx - 2];
    if (threadIdx >= 3) localVal += sdata0[threadIdx - 3];
    sdata1[threadIdx] = localVal;

    GroupMemoryBarrierWithGroupSync();

    if (threadIdx >= 4) localVal += sdata1[threadIdx - 4];
    if (threadIdx >= 8) localVal += sdata1[threadIdx - 8];
    if (threadIdx >= 12) localVal += sdata1[threadIdx - 12];
    sdata0[threadIdx] = localVal;

    GroupMemoryBarrierWithGroupSync();

    if (threadIdx >= 16) localVal += sdata0[threadIdx - 16];
    if (threadIdx >= 32) localVal += sdata0[threadIdx - 32];
    if (threadIdx >= 48) localVal += sdata0[threadIdx - 48];
    sdata1[threadIdx] = localVal;

    GroupMemoryBarrierWithGroupSync();

    if (threadIdx >= 64) localVal += sdata1[threadIdx - 64];
    if (threadIdx >= 128) localVal += sdata1[threadIdx - 128];
    if (threadIdx >= 192) localVal += sdata1[threadIdx - 192];

    uint4 retVal;
    retVal.w = localVal;
    retVal.z = retVal.w - val.w;
    retVal.y = retVal.z - val.z;
    retVal.x = retVal.y - val.y;

    // compute totalCount
    totalCount = sdata1[63] + sdata1[127] + sdata1[191] + sdata1[255];

    return retVal;
}

// where pred==1 indicates a zero allocation, pred==0 indicates a one allocation
uint4 split4(uint threadIdx, uint4 pred)
{
    uint totalCount;
    uint4 scanVal = blockScan(threadIdx, pred, totalCount);

    uint4 rank;
    rank.x = bool(pred.x) ? scanVal.x - 1 : 4 * threadIdx + 0 - scanVal.x + totalCount;
    rank.y = bool(pred.y) ? scanVal.y - 1 : 4 * threadIdx + 1 - scanVal.y + totalCount;
    rank.z = bool(pred.z) ? scanVal.z - 1 : 4 * threadIdx + 2 - scanVal.z + totalCount;
    rank.w = bool(pred.w) ? scanVal.w - 1 : 4 * threadIdx + 3 - scanVal.w + totalCount;

    return rank;
}

void sort(uint threadIdx, uint keyBitCount,
    inout uint4 keyLocal, inout uint4 valLocal1, inout uint4 valLocal2)
{
    for (uint passID = 0; passID < keyBitCount; passID++)
    {
        uint4 allocVal;
        allocVal.x = ((keyLocal.x >> passID) & 1) ^ 1u;
        allocVal.y = ((keyLocal.y >> passID) & 1) ^ 1u;
        allocVal.z = ((keyLocal.z >> passID) & 1) ^ 1u;
        allocVal.w = ((keyLocal.w >> passID) & 1) ^ 1u;

        uint4 allocIdx = split4(threadIdx, allocVal);

        skey[allocIdx.x] = keyLocal.x;
        skey[allocIdx.y] = keyLocal.y;
        skey[allocIdx.z] = keyLocal.z;
        skey[allocIdx.w] = keyLocal.w;
        sval1[allocIdx.x] = valLocal1.x;
        sval1[allocIdx.y] = valLocal1.y;
        sval1[allocIdx.z] = valLocal1.z;
        sval1[allocIdx.w] = valLocal1.w;
        sval2[allocIdx.x] = valLocal2.x;
        sval2[allocIdx.y] = valLocal2.y;
        sval2[allocIdx.z] = valLocal2.z;
        sval2[allocIdx.w] = valLocal2.w;

        GroupMemoryBarrierWithGroupSync();

        keyLocal.x = skey[4 * threadIdx + 0];
        keyLocal.y = skey[4 * threadIdx + 1];
        keyLocal.z = skey[4 * threadIdx + 2];
        keyLocal.w = skey[4 * threadIdx + 3];
        valLocal1.x = sval1[4 * threadIdx + 0];
        valLocal1.y = sval1[4 * threadIdx + 1];
        valLocal1.z = sval1[4 * threadIdx + 2];
        valLocal1.w = sval1[4 * threadIdx + 3];
        valLocal2.x = sval2[4 * threadIdx + 0];
        valLocal2.y = sval2[4 * threadIdx + 1];
        valLocal2.z = sval2[4 * threadIdx + 2];
        valLocal2.w = sval2[4 * threadIdx + 3];
    }
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

uint float_to_sort_key(float v)
{
    uint raw = pnanovdb_float_as_uint32(v);
    uint mask = -int(raw >> 31) | 0x80000000;
    return raw ^ mask;
}

uint get_gaussian_sort_key(pnanovdb_address_t means, float3 ray_dir, uint id)
{
    float3 mean = float3(
        pnanovdb_read_float(buf, pnanovdb_address_offset_product(means, 3u * id + 0u, 4u)),
        pnanovdb_read_float(buf, pnanovdb_address_offset_product(means, 3u * id + 1u, 4u)),
        pnanovdb_read_float(buf, pnanovdb_address_offset_product(means, 3u * id + 2u, 4u))
    );
    float t = dot(mean, ray_dir);
    return float_to_sort_key(t);
}

bool is_in_window(uint block_idx, pnanovdb_address_t headers, uint header_idx, inout uint last_header_idx)
{
    uint window_min = 1024u * block_idx;
    uint window_max = window_min + 1023u;

    // find first non-zero count
    pnanovdb_uint32_t begin_idx = 0u;
    pnanovdb_uint32_t count = 0u;
    for (uint level = 0u; level < 12u; level++)
    {
        begin_idx = pnanovdb_read_uint32(buf,
            pnanovdb_address_offset_product(headers, 18u * header_idx + level, 4u));
        count = pnanovdb_read_uint32(buf, pnanovdb_address_offset_product(headers, 18u * header_idx + (level >> 1u) + 12u, 4u));
        count = (count >> ((level & 1) << 4u)) & 0xFFFF;
        if (count != 0u)
        {
            break;
        }
    }

    if (begin_idx >= window_min && begin_idx <= window_max &&
        begin_idx + count > window_max)
    {
        last_header_idx = header_idx;
    }
    return (begin_idx >= window_min && begin_idx + count < window_max);
}

groupshared uint s_last_header_idx;

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

float3 read_sh(pnanovdb_address_t addr_sh, uint idx)
{
    return float3(
        pnanovdb_read_float(buf, pnanovdb_address_offset_product(addr_sh, 3u * idx + 0u, 4u)),
        pnanovdb_read_float(buf, pnanovdb_address_offset_product(addr_sh, 3u * idx + 1u, 4u)),
        pnanovdb_read_float(buf, pnanovdb_address_offset_product(addr_sh, 3u * idx + 2u, 4u))
    );
}

// Evaluate spherical harmonics bases at unit direction for high orders using
// approach described by Efficient Spherical Harmonic Evaluation, Peter-Pike
// Sloan, JCGT 2013 See https://jcgt.org/published/0002/02/06/ for reference
// implementation
float3 eval_sh_function(pnanovdb_address_t addr_shs, uint degree, uint sh_idx_begin, float3 viewDir)
{
    float3 cSH0 = read_sh(addr_shs, sh_idx_begin + 0u);

    float3 result = 0.2820947917738781f * cSH0;

    if (degree >= 1) {
        float inorm = rsqrt(viewDir.x * viewDir.x + viewDir.y * viewDir.y + viewDir.z * viewDir.z);
        float x = viewDir.x * inorm;
        float y = viewDir.y * inorm;
        float z = viewDir.z * inorm;

        float3 cSH1 = read_sh(addr_shs, sh_idx_begin + 1u);
        float3 cSH2 = read_sh(addr_shs, sh_idx_begin + 2u);
        float3 cSH3 = read_sh(addr_shs, sh_idx_begin + 3u);

        result += 0.48860251190292f * (-y * cSH1 + z * cSH2 - x * cSH3);

        if (degree >= 2) {
            float z2 = z * z;

            float fTmp0B = -1.092548430592079f * z;
            float fC1    = x * x - y * y;
            float fS1    = 2.f * x * y;
            float pSH6   = (0.9461746957575601f * z2 - 0.3153915652525201f);
            float pSH7   = fTmp0B * x;
            float pSH5   = fTmp0B * y;
            float pSH8   = 0.5462742152960395f * fC1;
            float pSH4   = 0.5462742152960395f * fS1;

            float3 cSH4 = read_sh(addr_shs, sh_idx_begin + 4u);
            float3 cSH5 = read_sh(addr_shs, sh_idx_begin + 5u);
            float3 cSH6 = read_sh(addr_shs, sh_idx_begin + 6u);
            float3 cSH7 = read_sh(addr_shs, sh_idx_begin + 7u);
            float3 cSH8 = read_sh(addr_shs, sh_idx_begin + 8u);

            result += (pSH4 * cSH4) + (pSH5 * cSH5) + (pSH6 * cSH6) + (pSH7 * cSH7) + (pSH8 * cSH8);

            if (degree >= 3) {
                float fTmp0C = -2.285228997322329f * z2 + 0.4570457994644658f;
                float fTmp1B = 1.445305721320277f * z;
                float fC2    = x * fC1 - y * fS1;
                float fS2    = x * fS1 + y * fC1;
                float pSH12  = z * (1.865881662950577f * z2 - 1.119528997770346f);
                float pSH13  = fTmp0C * x;
                float pSH11  = fTmp0C * y;
                float pSH14  = fTmp1B * fC1;
                float pSH10  = fTmp1B * fS1;
                float pSH15  = -0.5900435899266435f * fC2;
                float pSH9   = -0.5900435899266435f * fS2;

                float3 cSH9  = read_sh(addr_shs, sh_idx_begin + 9u);
                float3 cSH10 = read_sh(addr_shs, sh_idx_begin + 10u);
                float3 cSH11 = read_sh(addr_shs, sh_idx_begin + 11u);
                float3 cSH12 = read_sh(addr_shs, sh_idx_begin + 12u);
                float3 cSH13 = read_sh(addr_shs, sh_idx_begin + 13u);
                float3 cSH14 = read_sh(addr_shs, sh_idx_begin + 14u);
                float3 cSH15 = read_sh(addr_shs, sh_idx_begin + 15u);

                result += (pSH9 * cSH9) + (pSH10 * cSH10) + (pSH11 * cSH11) + (pSH12 * cSH12) +
                          (pSH13 * cSH13) + (pSH14 * cSH14) + (pSH15 * cSH15);

                if (degree >= 4) {
                    float fTmp0D = z * (-4.683325804901025f * z2 + 2.007139630671868f);
                    float fTmp1C = 3.31161143515146f * z2 - 0.47308734787878f;
                    float fTmp2B = -1.770130769779931f * z;
                    float fC3    = x * fC2 - y * fS2;
                    float fS3    = x * fS2 + y * fC2;
                    float pSH20  = (1.984313483298443f * z * pSH12 - 1.006230589874905f * pSH6);
                    float pSH21  = fTmp0D * x;
                    float pSH19  = fTmp0D * y;
                    float pSH22  = fTmp1C * fC1;
                    float pSH18  = fTmp1C * fS1;
                    float pSH23  = fTmp2B * fC2;
                    float pSH17  = fTmp2B * fS2;
                    float pSH24  = 0.6258357354491763f * fC3;
                    float pSH16  = 0.6258357354491763f * fS3;

                    float3 cSH16 = read_sh(addr_shs, sh_idx_begin + 16u);
                    float3 cSH17 = read_sh(addr_shs, sh_idx_begin + 17u);
                    float3 cSH18 = read_sh(addr_shs, sh_idx_begin + 18u);
                    float3 cSH19 = read_sh(addr_shs, sh_idx_begin + 19u);
                    float3 cSH20 = read_sh(addr_shs, sh_idx_begin + 20u);
                    float3 cSH21 = read_sh(addr_shs, sh_idx_begin + 21u);
                    float3 cSH22 = read_sh(addr_shs, sh_idx_begin + 22u);
                    float3 cSH23 = read_sh(addr_shs, sh_idx_begin + 23u);
                    float3 cSH24 = read_sh(addr_shs, sh_idx_begin + 24u);

                    result += (pSH16 * cSH16) + (pSH17 * cSH17) + (pSH18 * cSH18) +
                              (pSH19 * cSH19) + (pSH20 * cSH20) + (pSH21 * cSH21) +
                              (pSH22 * cSH22) + (pSH23 * cSH23) + (pSH24 * cSH24);
                }
            }
        }
    }

    return result + 0.5f;
}

[shader("compute")]
[numthreads(256, 1, 1)]
void main(uint3 workgroup_idx : SV_GroupID, uint3 thread_idx : SV_GroupThreadID)
{
    int2 workgroup_dim = int2((editor_params.width + 31) / 32, (editor_params.height + 3) / 4);

    int workgroup_idx1d = workgroup_idx.y * workgroup_dim.x + workgroup_idx.x;
    int workgroup_count = workgroup_dim.x * workgroup_dim.y;

    int thread_idx1d = thread_idx.x;

    pnanovdb_grid_handle_t grid = { pnanovdb_address_null() };
    pnanovdb_tree_handle_t tree = pnanovdb_grid_get_tree(buf, grid);
    pnanovdb_root_handle_t root = pnanovdb_tree_get_root(buf, tree);
    pnanovdb_grid_type_t grid_type = pnanovdb_grid_get_grid_type(buf, grid);

    if (grid_type != PNANOVDB_GRID_TYPE_UINT32)
    {
        return;
    }
    uint metadata_count = pnanovdb_grid_get_blind_metadata_count(buf, grid);
    if (metadata_count < 10u)
    {
        return;
    }

    pnanovdb_gridblindmetadata_handle_t metadata = pnanovdb_grid_get_gridblindmetadata(buf, grid, 1u);
    uint ids_count = uint(pnanovdb_gridblindmetadata_get_value_count(buf, metadata));

    pnanovdb_address_t addr_headers = pnanovdb_grid_get_gridblindmetadata_value_address(buf, grid, 0u);
    pnanovdb_address_t addr_ids = pnanovdb_grid_get_gridblindmetadata_value_address(buf, grid, 1u);
    pnanovdb_address_t addr_means = pnanovdb_grid_get_gridblindmetadata_value_address(buf, grid, 2u);
    pnanovdb_address_t addr_gaussian_header_ids = pnanovdb_grid_get_gridblindmetadata_value_address(buf, grid, 7u);
    pnanovdb_address_t addr_sort_keys = pnanovdb_grid_get_gridblindmetadata_value_address(buf, grid, 8u);

    float3 ray_dir;
    get_center_ray_dir(ray_dir);

    if (thread_idx1d == 0u)
    {
        s_last_header_idx = ~0u;
    }

    uint total_blocks = (ids_count + 1023u) / 1024u;
    for (uint block_idx = workgroup_idx1d; block_idx < total_blocks; block_idx += workgroup_count)
    {
        uint array_idx = 1024u * block_idx + 4u * thread_idx1d;
        uint4 ids = uint4(
            (array_idx + 0u >= ids_count) ? ~0u : pnanovdb_read_uint32(buf, pnanovdb_address_offset_product(addr_ids, array_idx + 0u, 4u)),
            (array_idx + 1u >= ids_count) ? ~0u : pnanovdb_read_uint32(buf, pnanovdb_address_offset_product(addr_ids, array_idx + 1u, 4u)),
            (array_idx + 2u >= ids_count) ? ~0u : pnanovdb_read_uint32(buf, pnanovdb_address_offset_product(addr_ids, array_idx + 2u, 4u)),
            (array_idx + 3u >= ids_count) ? ~0u : pnanovdb_read_uint32(buf, pnanovdb_address_offset_product(addr_ids, array_idx + 3u, 4u))
        );
        uint4 header_ids = uint4(
            (array_idx + 0u >= ids_count) ? ~0u : pnanovdb_read_uint32(buf, pnanovdb_address_offset_product(addr_gaussian_header_ids, array_idx + 0u, 4u)),
            (array_idx + 1u >= ids_count) ? ~0u : pnanovdb_read_uint32(buf, pnanovdb_address_offset_product(addr_gaussian_header_ids, array_idx + 1u, 4u)),
            (array_idx + 2u >= ids_count) ? ~0u : pnanovdb_read_uint32(buf, pnanovdb_address_offset_product(addr_gaussian_header_ids, array_idx + 2u, 4u)),
            (array_idx + 3u >= ids_count) ? ~0u : pnanovdb_read_uint32(buf, pnanovdb_address_offset_product(addr_gaussian_header_ids, array_idx + 3u, 4u))
        );
        uint4 depth_key = uint4(
            (array_idx + 0u >= ids_count) ? ~0u : get_gaussian_sort_key(addr_means, ray_dir, ids.x),
            (array_idx + 1u >= ids_count) ? ~0u : get_gaussian_sort_key(addr_means, ray_dir, ids.y),
            (array_idx + 2u >= ids_count) ? ~0u : get_gaussian_sort_key(addr_means, ray_dir, ids.z),
            (array_idx + 3u >= ids_count) ? ~0u : get_gaussian_sort_key(addr_means, ray_dir, ids.w)
        );

        sort(thread_idx1d, 32u, depth_key, ids, header_ids);
        sort(thread_idx1d, 32u, header_ids, ids, depth_key);

        uint last_header_idx = ~0u;
        bool4 should_write = bool4(
            is_in_window(block_idx, addr_headers, header_ids.x, last_header_idx),
            is_in_window(block_idx, addr_headers, header_ids.y, last_header_idx),
            is_in_window(block_idx, addr_headers, header_ids.z, last_header_idx),
            is_in_window(block_idx, addr_headers, header_ids.w, last_header_idx)
        );
        if (last_header_idx != ~0u)
        {
            s_last_header_idx = last_header_idx;
        }

        if (should_write.x && (array_idx + 0u) < ids_count)
        {
            pnanovdb_write_uint32(buf, pnanovdb_address_offset_product(addr_ids, array_idx + 0u, 4u), ids.x);
            pnanovdb_write_uint32(buf, pnanovdb_address_offset_product(addr_sort_keys, array_idx + 0u, 4u), depth_key.x);
        }
        if (should_write.y && (array_idx + 1u) < ids_count)
        {
            pnanovdb_write_uint32(buf, pnanovdb_address_offset_product(addr_ids, array_idx + 1u, 4u), ids.y);
            pnanovdb_write_uint32(buf, pnanovdb_address_offset_product(addr_sort_keys, array_idx + 1u, 4u), depth_key.y);
        }
        if (should_write.z && (array_idx + 2u) < ids_count)
        {
            pnanovdb_write_uint32(buf, pnanovdb_address_offset_product(addr_ids, array_idx + 2u, 4u), ids.z);
            pnanovdb_write_uint32(buf, pnanovdb_address_offset_product(addr_sort_keys, array_idx + 2u, 4u), depth_key.z);
        }
        if (should_write.w && (array_idx + 3u) < ids_count)
        {
            pnanovdb_write_uint32(buf, pnanovdb_address_offset_product(addr_ids, array_idx + 3u, 4u), ids.w);
            pnanovdb_write_uint32(buf, pnanovdb_address_offset_product(addr_sort_keys, array_idx + 3u, 4u), depth_key.w);
        }

        GroupMemoryBarrierWithGroupSync();

        if (s_last_header_idx != ~0u)
        {
            // find first non-zero count
            pnanovdb_uint32_t begin_idx = 0u;
            pnanovdb_uint32_t count = 0u;
            for (uint level = 0u; level < 12u; level++)
            {
                begin_idx = pnanovdb_read_uint32(buf,
                    pnanovdb_address_offset_product(addr_headers, 18u * s_last_header_idx + level, 4u));
                count = pnanovdb_read_uint32(buf, pnanovdb_address_offset_product(addr_headers, 18u * s_last_header_idx + (level >> 1u) + 12u, 4u));
                count = (count >> ((level & 1) << 4u)) & 0xFFFF;
                if (count != 0u)
                {
                    break;
                }
            }

            ids = uint4(
                (4u * thread_idx1d + 0u >= count) ? ~0u : pnanovdb_read_uint32(buf, pnanovdb_address_offset_product(addr_ids, begin_idx + 4u * thread_idx1d + 0u, 4u)),
                (4u * thread_idx1d + 1u >= count) ? ~0u : pnanovdb_read_uint32(buf, pnanovdb_address_offset_product(addr_ids, begin_idx + 4u * thread_idx1d + 1u, 4u)),
                (4u * thread_idx1d + 2u >= count) ? ~0u : pnanovdb_read_uint32(buf, pnanovdb_address_offset_product(addr_ids, begin_idx + 4u * thread_idx1d + 2u, 4u)),
                (4u * thread_idx1d + 3u >= count) ? ~0u : pnanovdb_read_uint32(buf, pnanovdb_address_offset_product(addr_ids, begin_idx + 4u * thread_idx1d + 3u, 4u))
            );
            header_ids = uint4(
                (4u * thread_idx1d + 0u >= count) ? ~0u : pnanovdb_read_uint32(buf, pnanovdb_address_offset_product(addr_gaussian_header_ids, begin_idx + 4u * thread_idx1d + 0u, 4u)),
                (4u * thread_idx1d + 1u >= count) ? ~0u : pnanovdb_read_uint32(buf, pnanovdb_address_offset_product(addr_gaussian_header_ids, begin_idx + 4u * thread_idx1d + 1u, 4u)),
                (4u * thread_idx1d + 2u >= count) ? ~0u : pnanovdb_read_uint32(buf, pnanovdb_address_offset_product(addr_gaussian_header_ids, begin_idx + 4u * thread_idx1d + 2u, 4u)),
                (4u * thread_idx1d + 3u >= count) ? ~0u : pnanovdb_read_uint32(buf, pnanovdb_address_offset_product(addr_gaussian_header_ids, begin_idx + 4u * thread_idx1d + 3u, 4u))
            );
            depth_key = uint4(
                (4u * thread_idx1d + 0u >= count) ? ~0u : get_gaussian_sort_key(addr_means, ray_dir, ids.x),
                (4u * thread_idx1d + 1u >= count) ? ~0u : get_gaussian_sort_key(addr_means, ray_dir, ids.y),
                (4u * thread_idx1d + 2u >= count) ? ~0u : get_gaussian_sort_key(addr_means, ray_dir, ids.z),
                (4u * thread_idx1d + 3u >= count) ? ~0u : get_gaussian_sort_key(addr_means, ray_dir, ids.w)
            );

            sort(thread_idx1d, 32u, depth_key, ids, header_ids);
            sort(thread_idx1d, 32u, header_ids, ids, depth_key);

            if (4u * thread_idx1d + 0u < count)
            {
                pnanovdb_write_uint32(buf, pnanovdb_address_offset_product(addr_ids, begin_idx + 4u * thread_idx1d + 0u, 4u), ids.x);
                pnanovdb_write_uint32(buf, pnanovdb_address_offset_product(addr_sort_keys, begin_idx + 4u * thread_idx1d + 0u, 4u), depth_key.x);
            }
            if (4u * thread_idx1d + 1u < count)
            {
                pnanovdb_write_uint32(buf, pnanovdb_address_offset_product(addr_ids, begin_idx + 4u * thread_idx1d + 1u, 4u), ids.y);
                pnanovdb_write_uint32(buf, pnanovdb_address_offset_product(addr_sort_keys, begin_idx + 4u * thread_idx1d + 1u, 4u), depth_key.y);
            }
            if (4u * thread_idx1d + 2u < count)
            {
                pnanovdb_write_uint32(buf, pnanovdb_address_offset_product(addr_ids, begin_idx + 4u * thread_idx1d + 2u, 4u), ids.z);
                pnanovdb_write_uint32(buf, pnanovdb_address_offset_product(addr_sort_keys, begin_idx + 4u * thread_idx1d + 2u, 4u), depth_key.z);
            }
            if (4u * thread_idx1d + 3u < count)
            {
                pnanovdb_write_uint32(buf, pnanovdb_address_offset_product(addr_ids, begin_idx + 4u * thread_idx1d + 3u, 4u), ids.w);
                pnanovdb_write_uint32(buf, pnanovdb_address_offset_product(addr_sort_keys, begin_idx + 4u * thread_idx1d + 3u, 4u), depth_key.w);
            }
        }
    }

    // projection
    pnanovdb_address_t addr_quats = pnanovdb_grid_get_gridblindmetadata_value_address(buf, grid, 3u);
    pnanovdb_address_t addr_scales = pnanovdb_grid_get_gridblindmetadata_value_address(buf, grid, 4u);
    pnanovdb_address_t addr_shs = pnanovdb_grid_get_gridblindmetadata_value_address(buf, grid, 5u);
    pnanovdb_address_t addr_opacities = pnanovdb_grid_get_gridblindmetadata_value_address(buf, grid, 6u);
    pnanovdb_address_t addr_conics = pnanovdb_grid_get_gridblindmetadata_value_address(buf, grid, 9u);
    pnanovdb_address_t addr_colors = pnanovdb_grid_get_gridblindmetadata_value_address(buf, grid, 10u);

    pnanovdb_gridblindmetadata_handle_t metadata_means = pnanovdb_grid_get_gridblindmetadata(buf, grid, 2u);
    uint point_count = uint(pnanovdb_gridblindmetadata_get_value_count(buf, metadata_means)) / 3u;

    total_blocks = (point_count + 255u) / 256u;
    for (uint block_idx = workgroup_idx1d; block_idx < total_blocks; block_idx += workgroup_count)
    {
        uint id = 256u * block_idx + thread_idx1d;

        if (id >= point_count)
        {
            break;
        }

        // compute distance to plane defined by means and raydir
        float3 meanf = pnanovdb_read_vec3(buf,
                                          pnanovdb_address_offset_product(addr_means, id, 12u));

        float4 quatf;
        quatf.x = pnanovdb_read_float(buf,
                                      pnanovdb_address_offset_product(addr_quats, 4u * id + 1u, 4u));
        quatf.y = pnanovdb_read_float(buf,
                                      pnanovdb_address_offset_product(addr_quats, 4u * id + 2u, 4u));
        quatf.z = pnanovdb_read_float(buf,
                                      pnanovdb_address_offset_product(addr_quats, 4u * id + 3u, 4u));
        quatf.w = pnanovdb_read_float(buf,
                                      pnanovdb_address_offset_product(addr_quats, 4u * id + 0u, 4u));

        float3 scalef = pnanovdb_read_vec3(buf,
                                           pnanovdb_address_offset_product(addr_scales, id, 12u));

        float opacity = pnanovdb_read_float(buf,
                                            pnanovdb_address_offset_product(addr_opacities, 1u * id + 0u, 4u));

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

        float3 conic = float3(covar2d_inv[0][0], covar2d_inv[0][1], covar2d_inv[1][1]);

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

        float3 color = eval_sh_function(addr_shs, 3, 16u * id, ray_dir);

        pnanovdb_write_float(buf, pnanovdb_address_offset_product(addr_conics, 3u * id + 0u, 4u), conic.x);
        pnanovdb_write_float(buf, pnanovdb_address_offset_product(addr_conics, 3u * id + 1u, 4u), conic.y);
        pnanovdb_write_float(buf, pnanovdb_address_offset_product(addr_conics, 3u * id + 2u, 4u), conic.z);
        pnanovdb_write_float(buf, pnanovdb_address_offset_product(addr_colors, 3u * id + 0u, 4u), color.x);
        pnanovdb_write_float(buf, pnanovdb_address_offset_product(addr_colors, 3u * id + 1u, 4u), color.y);
        pnanovdb_write_float(buf, pnanovdb_address_offset_product(addr_colors, 3u * id + 2u, 4u), color.z);
    }
}
