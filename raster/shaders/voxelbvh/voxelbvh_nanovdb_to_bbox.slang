// voxelbvh_nanovdb_to_bbox.slang

#define PNANOVDB_HLSL
#define PNANOVDB_ADDRESS_64
#define PNANOVDB_BUF_HLSL_64
//#define PNANOVDB_BUF_HLSL_RW
#include "PNanoVDB.h"

StructuredBuffer<uint2> buf;

RWStructuredBuffer<float> world_bbox_out;

float convert_to_float(uint64_t raw)
{
    uint sign = uint((raw >> 63) & 1);
    uint exponent = uint((raw >> 52) & 0x7FF);
    uint mantissa_fp32 = uint(raw >> 29) & 0x7FFFFF;
    uint raw32 = (sign << 31) |
                 (((exponent - 896) & 255) << 23) |
                 mantissa_fp32;
    return asfloat(raw32);
}

PNANOVDB_FORCE_INLINE uint64_t pnanovdb_grid_get_world_bbox_uint64(pnanovdb_buf_t buf, pnanovdb_grid_handle_t p, pnanovdb_uint32_t index) {
    return pnanovdb_read_uint64(buf, pnanovdb_address_offset(p.address, PNANOVDB_GRID_OFF_WORLD_BBOX + 8u * index));
}

[shader("compute")][numthreads(32, 1, 1)]
void main(uint3 group_idx : SV_GroupID, uint3 thread_idx : SV_GroupThreadID)
{
    pnanovdb_grid_handle_t grid = {};
    pnanovdb_tree_handle_t tree = pnanovdb_grid_get_tree(buf, grid);
    pnanovdb_root_handle_t root = pnanovdb_tree_get_root(buf, tree);
    pnanovdb_grid_type_t grid_type = pnanovdb_grid_get_grid_type(buf, grid);

    if (thread_idx.x < 6u)
    {
        world_bbox_out[thread_idx.x] = convert_to_float(
            pnanovdb_grid_get_world_bbox_uint64(buf, grid, thread_idx.x));
    }
}
