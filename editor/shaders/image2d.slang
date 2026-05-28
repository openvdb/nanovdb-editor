// image2d.slang
#define PNANOVDB_HLSL
#define PNANOVDB_ADDRESS_64
#define PNANOVDB_BUF_HLSL_64
#include "PNanoVDB.h"

#include "editor_params.slang"

struct shader_params_t
{
    uint auto_center;
};

StructuredBuffer<uint2> buf;
RWStructuredBuffer<uint> image_out_deprecated;
RWTexture2D<float4> texture_out;
ConstantBuffer<EditorParams> editor_params;
ConstantBuffer<shader_params_t> shader_params;

[shader("compute")][numthreads(16, 8, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    int2 tidx = int2(dispatchThreadID.xy);

    pnanovdb_grid_handle_t grid = { pnanovdb_address_null() };
    pnanovdb_tree_handle_t tree = pnanovdb_grid_get_tree(buf, grid);
    pnanovdb_root_handle_t root = pnanovdb_tree_get_root(buf, tree);
    pnanovdb_grid_type_t grid_type = pnanovdb_grid_get_grid_type(buf, grid);

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

        uint idx = (tidx.y % image_dim.y) * image_dim.x + (tidx.x % image_dim.x);

        uint val_raw = pnanovdb_read_uint32(buf, pnanovdb_address_offset_product(val_addr, idx, 4u));

        value = float4(
            (1.f / 255.f) * float((val_raw) & 255),
            (1.f / 255.f) * float((val_raw >> 8u) & 255),
            (1.f / 255.f) * float((val_raw >> 16u) & 255),
            (1.f / 255.f) * float((val_raw >> 24u) & 255)
        );
    }

    texture_out[tidx] = value;
}
