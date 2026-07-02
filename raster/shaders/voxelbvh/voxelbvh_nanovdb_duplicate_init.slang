// voxelbvh_duplicate_init.slang

#define PNANOVDB_HLSL
#define PNANOVDB_ADDRESS_64
#define PNANOVDB_BUF_HLSL_64
#define PNANOVDB_BUF_HLSL_RW
#include "PNanoVDB.h"

struct constants_t
{
    uint upsample_factor;
    uint dst_grid_type;
};

ConstantBuffer<constants_t> constants;

RWStructuredBuffer<uint2> src_buf;
RWStructuredBuffer<uint2> dst_buf;

uint64_t convert_to_double(float v)
{
    uint raw = asuint(v);
    uint sign = (raw >> 31) & 1;
    uint exponent = (raw >> 23) & 0xFF;
    uint mantissa = raw & 0x7FFFFF;
    uint exponent_fp64 = exponent + 896;
    uint64_t raw64 = (uint64_t(sign) << 63) |
                     (uint64_t(exponent_fp64) << 52) |
                     (uint64_t(mantissa) << 29);
    return raw64;
}

PNANOVDB_FORCE_INLINE void pnanovdb_write_double_float(pnanovdb_buf_t buf, pnanovdb_address_t address, float value)
{
    pnanovdb_write_uint64(buf, address, convert_to_double(value));
}

PNANOVDB_FORCE_INLINE void pnanovdb_grid_set_world_bbox_float(pnanovdb_buf_t buf, pnanovdb_grid_handle_t p, pnanovdb_uint32_t index, float world_bbox) {
    pnanovdb_write_double_float(buf, pnanovdb_address_offset(p.address, PNANOVDB_GRID_OFF_WORLD_BBOX + 8u * index), world_bbox);
}
PNANOVDB_FORCE_INLINE void pnanovdb_grid_set_voxel_size_float(pnanovdb_buf_t buf, pnanovdb_grid_handle_t p, pnanovdb_uint32_t index, float voxel_size) {
    pnanovdb_write_double_float(buf, pnanovdb_address_offset(p.address, PNANOVDB_GRID_OFF_VOXEL_SIZE + 8u * index), voxel_size);
}
PNANOVDB_FORCE_INLINE void pnanovdb_map_set_matd_float(pnanovdb_buf_t buf, pnanovdb_map_handle_t p, pnanovdb_uint32_t index, float matd) {
    pnanovdb_write_double_float(buf, pnanovdb_address_offset(p.address, PNANOVDB_MAP_OFF_MATD + 8u * index), matd);
}
PNANOVDB_FORCE_INLINE void pnanovdb_map_set_invmatd_float(pnanovdb_buf_t buf, pnanovdb_map_handle_t p, pnanovdb_uint32_t index, float invmatd) {
    pnanovdb_write_double_float(buf, pnanovdb_address_offset(p.address, PNANOVDB_MAP_OFF_INVMATD + 8u * index), invmatd);
}
PNANOVDB_FORCE_INLINE void pnanovdb_map_set_vecd_float(pnanovdb_buf_t buf, pnanovdb_map_handle_t p, pnanovdb_uint32_t index, float vecd) {
    pnanovdb_write_double_float(buf, pnanovdb_address_offset(p.address, PNANOVDB_MAP_OFF_VECD + 8u * index), vecd);
}

[shader("compute")][numthreads(256, 1, 1)]
void main(uint3 group_idx : SV_GroupID, uint3 thread_idx : SV_GroupThreadID)
{
    if (group_idx.x != 0u)
    {
        return;
    }

    pnanovdb_grid_handle_t src_grid = {};
    pnanovdb_tree_handle_t src_tree = pnanovdb_grid_get_tree(src_buf, src_grid);
    pnanovdb_root_handle_t src_root = pnanovdb_tree_get_root(src_buf, src_tree);
    pnanovdb_grid_type_t src_grid_type = pnanovdb_grid_get_grid_type(src_buf, src_grid);

    uint copy_size = PNANOVDB_GRID_SIZE + PNANOVDB_TREE_SIZE;
    if (8u * thread_idx.x < copy_size)
    {
        dst_buf[thread_idx.x] = src_buf[thread_idx.x];
    }

    GroupMemoryBarrierWithGroupSync();

    pnanovdb_grid_handle_t dst_grid = {};
    pnanovdb_tree_handle_t dst_tree = pnanovdb_grid_get_tree(dst_buf, dst_grid);
    pnanovdb_root_handle_t dst_root = pnanovdb_tree_get_root(dst_buf, dst_tree);

    pnanovdb_grid_type_t dst_grid_type = constants.dst_grid_type;
    pnanovdb_grid_set_grid_type(dst_buf, dst_grid, constants.dst_grid_type);

    // need to rescale map based on upsample factor
    float inv_scale = float(constants.upsample_factor);
    float scale = 1.f / inv_scale;
    pnanovdb_map_handle_t map = pnanovdb_grid_get_map(dst_buf, dst_grid);
    if (thread_idx.x < 9u)
    {
        uint i = thread_idx.x;
        float v = scale * pnanovdb_map_get_matf(dst_buf, map, i);
        pnanovdb_map_set_matf(dst_buf, map, i, v);
        pnanovdb_map_set_matd_float(dst_buf, map, i, v);
    }
    if (thread_idx.x < 9u)
    {
        uint i = thread_idx.x;
        float v = inv_scale * pnanovdb_map_get_invmatf(dst_buf, map, i);
        pnanovdb_map_set_invmatf(dst_buf, map, i, v);
        pnanovdb_map_set_invmatd_float(dst_buf, map, i, v);
    }

    // based on root bbox and upsample_factor, establish root tiles
    pnanovdb_coord_t bbox_min = pnanovdb_root_get_bbox_min(src_buf, src_root);
    pnanovdb_coord_t bbox_max = pnanovdb_root_get_bbox_max(src_buf, src_root);

    // upsample
    bbox_min = int(constants.upsample_factor) * bbox_min;
    bbox_max = int(constants.upsample_factor) * bbox_max;

    // compute tile grid
    pnanovdb_coord_t root_ijk_min = bbox_min & ~4095;
    pnanovdb_coord_t root_ijk_max = (bbox_max + 4096) & ~4095;

    int3 root_dim = (root_ijk_max - root_ijk_min) >> 12;
    int root_dim1d = root_dim.x * root_dim.y * root_dim.z;
    if (root_dim1d >= 256u)
    {
        root_dim1d = 256u;
    }

    if (thread_idx.x == 0u)
    {
        pnanovdb_root_set_tile_count(dst_buf, dst_root, root_dim1d);
    }
    if (thread_idx.x < root_dim1d)
    {
        uint n = thread_idx.x;
        uint root_i = n / (root_dim.z * root_dim.y);
        uint root_j = (n / root_dim.z) % root_dim.y;
        uint root_k = n % root_dim.z;

        pnanovdb_root_tile_handle_t root_tile = pnanovdb_root_get_tile(dst_grid_type, dst_root, n);

        int3 root_coord = int3(
            root_i * 4096u + root_ijk_min.x,
            root_j * 4096u + root_ijk_min.y,
            root_k * 4096 + root_ijk_min.z
        );
        uint64_t key = pnanovdb_coord_to_key(root_coord);
        pnanovdb_root_tile_set_key(dst_buf, root_tile, key);
    }

    if (thread_idx.x == 0u)
    {
        uint64_t grid_size = copy_size;
        grid_size += root_dim1d * PNANOVDB_GRID_TYPE_GET(dst_grid_type, root_tile_size);
        pnanovdb_grid_set_grid_size(dst_buf, dst_grid, grid_size);
    }
}
