// voxelbvh_nanovdb_init.slang

#define PNANOVDB_HLSL
#define PNANOVDB_ADDRESS_64
#define PNANOVDB_BUF_HLSL_64
#define PNANOVDB_BUF_HLSL_RW
#include "PNanoVDB.h"

struct constants_t
{
    uint grid_size;
    uint resolution;
};
ConstantBuffer<constants_t> constants;

StructuredBuffer<uint2> upload;

RWStructuredBuffer<uint2> buf;

StructuredBuffer<float> world_bbox_in;

float4 compute_world_to_voxel(float3 world_min, float3 world_max)
{
    float3 world_delta = world_max - world_min;
    float world_delta_max = max(world_delta.x, max(world_delta.y, world_delta.z));
    float scale = float(constants.resolution - 1u) / (world_delta_max);
    float3 offset = -world_min * scale + 0.5f;
    return float4(offset, scale);
}

uint64_t convert_to_double(float v)
{
    uint raw = asuint(v);
    uint sign = (raw >> 31) & 1;
    uint exponent = (raw >> 23) & 0xFF;
    uint mantissa = raw & 0x7FFFFF;
    uint64_t raw64 = (uint64_t(sign) << 63) |
                     (uint64_t(exponent + 896) << 52) |
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
    uint idx = group_idx.x * 256u + thread_idx.x;

    if (8u * idx < constants.grid_size)
    {
        buf[idx] = upload[idx];
    }

    GroupMemoryBarrierWithGroupSync();

    if (idx == 0u)
    {
        pnanovdb_grid_handle_t grid = {};

        float3 g_aabb_min = float3(world_bbox_in[0], world_bbox_in[1], world_bbox_in[2]);
        float3 g_aabb_max = float3(world_bbox_in[3], world_bbox_in[4], world_bbox_in[5]);
        float4 world_to_voxel = compute_world_to_voxel(g_aabb_min, g_aabb_max);

        float voxel_size = 1.f / world_to_voxel.w;
        float voxel_size_inv = 1.f / voxel_size;
        float3 voxel_offset = -world_to_voxel.xyz * voxel_size;

        pnanovdb_grid_set_voxel_size_float(buf, grid, 0u, voxel_size);
        pnanovdb_grid_set_voxel_size_float(buf, grid, 1u, voxel_size);
        pnanovdb_grid_set_voxel_size_float(buf, grid, 2u, voxel_size);

        pnanovdb_map_handle_t map = pnanovdb_grid_get_map(buf, grid);
        pnanovdb_map_set_matf(buf, map, 0u, voxel_size);
        pnanovdb_map_set_matf(buf, map, 4u, voxel_size);
        pnanovdb_map_set_matf(buf, map, 8u, voxel_size);
        pnanovdb_map_set_vecf(buf, map, 0u, voxel_offset.x);
        pnanovdb_map_set_vecf(buf, map, 1u, voxel_offset.y);
        pnanovdb_map_set_vecf(buf, map, 2u, voxel_offset.z);
        pnanovdb_map_set_invmatf(buf, map, 0u, voxel_size_inv);
        pnanovdb_map_set_invmatf(buf, map, 4u, voxel_size_inv);
        pnanovdb_map_set_invmatf(buf, map, 8u, voxel_size_inv);
        pnanovdb_map_set_matd_float(buf, map, 0u, voxel_size);
        pnanovdb_map_set_matd_float(buf, map, 4u, voxel_size);
        pnanovdb_map_set_matd_float(buf, map, 8u, voxel_size);
        pnanovdb_map_set_vecd_float(buf, map, 0u, voxel_offset.x);
        pnanovdb_map_set_vecd_float(buf, map, 1u, voxel_offset.y);
        pnanovdb_map_set_vecd_float(buf, map, 2u, voxel_offset.z);
        pnanovdb_map_set_invmatd_float(buf, map, 0u, voxel_size_inv);
        pnanovdb_map_set_invmatd_float(buf, map, 4u, voxel_size_inv);
        pnanovdb_map_set_invmatd_float(buf, map, 8u, voxel_size_inv);

        pnanovdb_grid_set_world_bbox_float(buf, grid, 0u, g_aabb_min.x);
        pnanovdb_grid_set_world_bbox_float(buf, grid, 1u, g_aabb_min.y);
        pnanovdb_grid_set_world_bbox_float(buf, grid, 2u, g_aabb_min.z);
        pnanovdb_grid_set_world_bbox_float(buf, grid, 3u, g_aabb_max.x);
        pnanovdb_grid_set_world_bbox_float(buf, grid, 4u, g_aabb_max.y);
        pnanovdb_grid_set_world_bbox_float(buf, grid, 5u, g_aabb_max.z);

        pnanovdb_tree_handle_t tree = pnanovdb_grid_get_tree(buf, grid);
        pnanovdb_root_handle_t root = pnanovdb_tree_get_root(buf, tree);
        pnanovdb_grid_type_t grid_type = pnanovdb_grid_get_grid_type(buf, grid);

        uint root_dim = (constants.resolution + 4095u) / 4096u;
        uint root_tile_count = root_dim * root_dim * root_dim;

        pnanovdb_root_set_tile_count(buf, root, root_tile_count);
        for (uint n = 0u; n < root_tile_count; n++)
        {
            uint root_i = n / (root_dim * root_dim);
            uint root_j = (n / root_dim) % root_dim;
            uint root_k = n % root_dim;

            pnanovdb_root_tile_handle_t root_tile = pnanovdb_root_get_tile(grid_type, root, n);

            int3 root_coord = int3(
                root_i * 4096u,
                root_j * 4096u,
                root_k * 4096
            );
            uint64_t key = pnanovdb_coord_to_key(root_coord);
            pnanovdb_root_tile_set_key(buf, root_tile, key);
        }

        uint64_t grid_size = constants.grid_size;
        grid_size += root_tile_count * PNANOVDB_GRID_TYPE_GET(grid_type, root_tile_size);
        pnanovdb_grid_set_grid_size(buf, grid, grid_size);
    }
}
