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
    float slice_z;
    float x_pos;
    float y_pos;
    float zoom;
    uint axis;
    uint show_first_active_level;
    uint show_list_ids;
    uint show_nanovdb_level;
    uint show_inactive;
};

StructuredBuffer<uint2> buf;
RWStructuredBuffer<uint> image_out;
RWTexture2D<float4> texture_out;
ConstantBuffer<EditorParams> editor_params;
ConstantBuffer<shader_params_t> shader_params;

float4 mat_mul(float4 v, float4 x, float4 y, float4 z, float4 w)
{
    return float4(dot(v, x), dot(v, y), dot(v, z), dot(v, w));
}

[shader("compute")][numthreads(32, 4, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    int2 tidx = int2(dispatchThreadID.xy);

    pnanovdb_grid_handle_t grid = { pnanovdb_address_null() };
    pnanovdb_tree_handle_t tree = pnanovdb_grid_get_tree(buf, grid);
    pnanovdb_root_handle_t root = pnanovdb_tree_get_root(buf, tree);
    pnanovdb_grid_type_t grid_type = pnanovdb_grid_get_grid_type(buf, grid);

    pnanovdb_readaccessor_t acc;
    pnanovdb_readaccessor_init(PNANOVDB_REF(acc), root);

    pnanovdb_coord_t ijk_min = pnanovdb_root_get_bbox_min(buf, root);
    pnanovdb_coord_t ijk_max = pnanovdb_root_get_bbox_max(buf, root);

    float3 uvw = float3(
        (float(tidx.x) + 0.5f) / float(editor_params.width),
        1.f - ((float(tidx.y) + 0.5f) / float(editor_params.height)),
        shader_params.slice_z
    );
    // put in ndc space
    uvw.xy = 2.f * uvw.xy - 0.5f;
    uvw.y *= (float(editor_params.height) / float(editor_params.width));

    uvw.xy *= (1.f / shader_params.zoom);
    uvw.x += shader_params.x_pos;
    uvw.y += shader_params.y_pos;

    if (shader_params.axis == 1)
    {
        uvw.xyz = uvw.xzy;
    }
    else if (shader_params.axis == 2)
    {
        uvw.xyz = uvw.zxy;
    }

    uvw.xyz = uvw.xzy;

    int3 ijk = int3(
        int(float(ijk_max.x - ijk_min.x) * uvw.x) + ijk_min.x,
        int(float(ijk_max.y - ijk_min.y) * uvw.y) + ijk_min.y,
        int(float(ijk_max.z - ijk_min.z) * uvw.z) + ijk_min.z
    );

    pnanovdb_address_t addr = pnanovdb_readaccessor_get_value_address(grid_type, buf, acc, ijk);
    uint64_t val_raw = pnanovdb_read_uint64(buf, addr);
    uint active_level_mask = uint(val_raw) & 0xFFFF;
    uint skip_level = (uint(val_raw) >> 16u) & 0xFFFF;
    uint list_begin_idx = uint(val_raw >> 32u);

    float4 value = float4(0.f, 0.f, 0.f, 0.f);

    if (shader_params.show_first_active_level != 0u)
    {
        skip_level = firstbitlow(active_level_mask);
    }
    if (shader_params.show_list_ids != 0u)
    {
        skip_level = list_begin_idx % 12;
    }
    if (shader_params.show_nanovdb_level != 0u)
    {
        skip_level = 3u;
        if (acc.leaf.address.byte_offset != 0u)
        {
            skip_level = 0u;
        }
        else if (acc.lower.address.byte_offset != 0u)
        {
            skip_level = 1u;
        }
        else if (acc.upper.address.byte_offset != 0u)
        {
            skip_level = 2u;
        }
    }

    if (skip_level == 0u) { value = float4(1.f, 0.f, 0.f, 0.f);}
    if (skip_level == 1u) { value = float4(1.f, 1.f, 0.f, 0.f);}
    if (skip_level == 2u) { value = float4(0.f, 1.f, 0.f, 0.f);}
    if (skip_level == 3u) { value = float4(0.f, 1.f, 1.f, 0.f);}
    if (skip_level == 4u) { value = float4(0.f, 0.f, 1.f, 0.f);}
    if (skip_level == 5u) { value = float4(1.f, 0.f, 1.f, 0.f);}
    if (skip_level == 6u) { value = float4(0.5f, 0.f, 0.f, 0.f);}
    if (skip_level == 7u) { value = float4(0.5f, 0.5f, 0.f, 0.f);}
    if (skip_level == 8u) { value = float4(0.f, 0.5f, 0.f, 0.f);}
    if (skip_level == 9u) { value = float4(0.f, 0.5f, 0.5f, 0.f);}
    if (skip_level == 10u) { value = float4(0.0f, 0.f, 0.5f, 0.f);}
    if (skip_level == 11u) { value = float4(0.5f, 0.f, 0.5f, 0.f);}
    if (skip_level == 12u) { value = float4(0.33f, 0.33f, 0.33f, 0.f);}

    if (shader_params.show_inactive != 0u)
    {
        if (active_level_mask == 0u)
        {
            value = float4(1.f, 0.5f, 0.f, 0.f);
        }
    }

    if (((ijk.x ^ ijk.y ^ ijk.z) & 1) != 0u)
    {
        value *= 0.9f;
    }
    if (((ijk.x ^ ijk.y ^ ijk.z) & 2) != 0u)
    {
        value *= 0.7f;
    }

    texture_out[tidx] = value;
}
