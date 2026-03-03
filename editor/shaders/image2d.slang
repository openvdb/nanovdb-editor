// wireframe.slang
#define PNANOVDB_HLSL
#define PNANOVDB_ADDRESS_64
#define PNANOVDB_BUF_HLSL_64
#include "PNanoVDB.h"
#include "PNanoVDBExt.h"
#include "editor_params.slang"

struct shader_params_t
{
    uint auto_center;
};

StructuredBuffer<uint2> buf;
RWStructuredBuffer<uint> image_out;
RWTexture2D<float4> texture_out;
ConstantBuffer<EditorParams> editor_params;
ConstantBuffer<shader_params_t> shader_params;

[shader("compute")][numthreads(32, 4, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    int2 tidx = int2(dispatchThreadID.xy);
    if (tidx.x >= int(editor_params.width) || tidx.y >= int(editor_params.height))
    {
        return;
    }

    pnanovdb_grid_handle_t grid = { pnanovdb_address_null() };
    pnanovdb_tree_handle_t tree = pnanovdb_grid_get_tree(buf, grid);
    pnanovdb_root_handle_t root = pnanovdb_tree_get_root(buf, tree);
    float4 value = float4(1.f, 0.f, 0.f, 1.f);
    pnanovdb_uint32_t meta_count = pnanovdb_grid_get_blind_metadata_count(buf, grid);

    if (meta_count >= 1u)
    {
        int3 ijk_min = pnanovdb_root_get_bbox_min(buf, root);
        int3 ijk_max = pnanovdb_root_get_bbox_max(buf, root);

        int3 image_dim = ijk_max + int3(1, 1, 1) - ijk_min;

        pnanovdb_gridblindmetadata_handle_t meta = pnanovdb_grid_get_gridblindmetadata(buf, grid, 0u);
        pnanovdb_int64_t byte_offset = pnanovdb_gridblindmetadata_get_data_offset(buf, meta);
        pnanovdb_address_t val_addr = pnanovdb_address_offset64(meta.address, pnanovdb_int64_as_uint64(byte_offset));
        pnanovdb_grid_type_t blind_type = pnanovdb_gridblindmetadata_get_data_type(buf, meta);

        if (blind_type == PNANOVDB_GRID_TYPE_RGBA8)
        {
            uint image_w = uint(max(image_dim.x, 1));
            uint image_h = uint(max(image_dim.y, 1));
            uint idx = (uint(tidx.y) % image_h) * image_w + (uint(tidx.x) % image_w);

            pnanovdb_uint64_t value_count64 = pnanovdb_gridblindmetadata_get_value_count(buf, meta);
            uint value_count = pnanovdb_uint64_low(value_count64);
            if (value_count > 0u)
            {
                idx = idx % value_count;

                uint val_raw = pnanovdb_read_uint32(buf, pnanovdb_address_offset_product(val_addr, idx, 4u));

                value = float4(
                    (1.f / 255.f) * float((val_raw) & 255),
                    (1.f / 255.f) * float((val_raw >> 8u) & 255),
                    (1.f / 255.f) * float((val_raw >> 16u) & 255),
                    (1.f / 255.f) * float((val_raw >> 24u) & 255)
                );
            }
        }
    }

    // Note: BGRA target
    uint rawValue = uint(255.f * max(0.f, min(1.f, value.z))) | (uint(255.f * max(0.f, min(1.f, value.y))) << 8u) |
                    (uint(255.f * max(0.f, min(1.f, value.x))) << 16u) |
                    (uint(255.f * max(0.f, min(1.f, value.w))) << 24u);
    image_out[(editor_params.height - 1 - tidx.y) * editor_params.width + tidx.x] = rawValue;

    float4 background = (editor_params.composite != 0u) ? texture_out[tidx] : float4(0.f, 0.f, 0.f, 1.f);
    float alpha = max(0.f, min(1.f, value.a));
    float4 image_color;
    image_color.rgb = value.rgb * alpha + background.rgb * (1.f - alpha);
    image_color.a = alpha + background.a * (1.f - alpha);
    texture_out[tidx] = image_color;
}
