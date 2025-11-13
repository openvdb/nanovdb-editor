// wireframe.slang
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

    // Note: BGRA target
    uint rawValue = uint(255.f * max(0.f, min(1.f, value.z))) | (uint(255.f * max(0.f, min(1.f, value.y))) << 8u) |
                    (uint(255.f * max(0.f, min(1.f, value.x))) << 16u) |
                    (uint(255.f * max(0.f, min(1.f, value.w))) << 24u);
    image_out[(editor_params.height - 1 - tidx.y) * editor_params.width + tidx.x] = rawValue;

    texture_out[tidx] = value;
}
