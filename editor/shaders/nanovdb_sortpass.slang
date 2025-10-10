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

bool is_in_window(uint block_idx, pnanovdb_address_t addr_headers, uint header_idx, inout uint last_header_idx)
{
    uint window_min = 1024u * block_idx;
    uint window_max = window_min + 1023u;

    // find first non-zero count
    pnanovdb_uint32_t begin_idx = pnanovdb_read_uint32(buf,
            pnanovdb_address_offset_product(addr_headers, 5u * header_idx + 0u, 4u));
    pnanovdb_uint32_t count = pnanovdb_read_uint32(buf,
            pnanovdb_address_offset_product(addr_headers, 5u * header_idx + 1u, 4u));

    if (begin_idx >= window_min && begin_idx <= window_max &&
        begin_idx + count > window_max)
    {
        last_header_idx = header_idx;
    }
    return (begin_idx >= window_min && begin_idx + count < window_max);
}

groupshared uint s_last_header_idx;

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

    pnanovdb_address_t addr_headers = pnanovdb_grid_get_gridblindmetadata_value_address(buf, grid, 20u);
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
            pnanovdb_uint32_t begin_idx = pnanovdb_read_uint32(buf,
                    pnanovdb_address_offset_product(addr_headers, 5u * s_last_header_idx + 0u, 4u));
            pnanovdb_uint32_t count = pnanovdb_read_uint32(buf,
                    pnanovdb_address_offset_product(addr_headers, 5u * s_last_header_idx + 1u, 4u));

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
}
