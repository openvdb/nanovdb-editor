// voxelbvh_nanovdb_init.slang

#define PNANOVDB_HLSL
#define PNANOVDB_ADDRESS_64
#define PNANOVDB_BUF_HLSL_64
#define PNANOVDB_BUF_HLSL_RW
#include "PNanoVDB.h"

StructuredBuffer<uint2> upload;

RWStructuredBuffer<uint2> buf;

StructuredBuffer<float> world_bbox_in;

[shader("compute")][numthreads(256, 1, 1)]
void main(uint3 group_idx : SV_GroupID, uint3 thread_idx : SV_GroupThreadID)
{
    uint idx = group_idx.x * 256u + thread_idx.x;

    // grid size in NanoVDB is at 64-bit word index 4
    uint2 grid_size = upload[4u];

    if (8u * idx < grid_size.x)
    {
        buf[idx] = upload[idx];
    }

    GroupMemoryBarrierWithGroupSync();

    if (idx == 0u)
    {
        pnanovdb_grid_handle_t grid = {};

        const uint integer_space_max = 4095;
        float3 g_aabb_min = float3(world_bbox_in[0], world_bbox_in[1], world_bbox_in[2]);
        float3 g_aabb_max = float3(world_bbox_in[3], world_bbox_in[4], world_bbox_in[5]);
        float3 g_aabb_delta = g_aabb_max - g_aabb_min;
        float g_aabb_delta_max = max(g_aabb_delta.x, max(g_aabb_delta.y, g_aabb_delta.z));
        float scale = float(integer_space_max) / (g_aabb_delta_max);
        float3 offset = -g_aabb_min * scale;

        float voxel_size = 1.f / scale;
        float voxel_size_inv = 1.f / voxel_size;
        float3 voxel_offset = -offset * voxel_size;

        pnanovdb_grid_set_voxel_size(buf, grid, 0u, voxel_size);
        pnanovdb_grid_set_voxel_size(buf, grid, 1u, voxel_size);
        pnanovdb_grid_set_voxel_size(buf, grid, 2u, voxel_size);

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
        pnanovdb_map_set_matd(buf, map, 0u, voxel_size);
        pnanovdb_map_set_matd(buf, map, 4u, voxel_size);
        pnanovdb_map_set_matd(buf, map, 8u, voxel_size);
        pnanovdb_map_set_vecd(buf, map, 0u, voxel_offset.x);
        pnanovdb_map_set_vecd(buf, map, 1u, voxel_offset.y);
        pnanovdb_map_set_vecd(buf, map, 2u, voxel_offset.z);
        pnanovdb_map_set_invmatd(buf, map, 0u, voxel_size_inv);
        pnanovdb_map_set_invmatd(buf, map, 4u, voxel_size_inv);
        pnanovdb_map_set_invmatd(buf, map, 8u, voxel_size_inv);

        pnanovdb_grid_set_world_bbox(buf, grid, 0u, 0.0);
        pnanovdb_grid_set_world_bbox(buf, grid, 1u, 0.0);
        pnanovdb_grid_set_world_bbox(buf, grid, 2u, 0.0);
        pnanovdb_grid_set_world_bbox(buf, grid, 3u, 0.0);
        pnanovdb_grid_set_world_bbox(buf, grid, 4u, 0.0);
        pnanovdb_grid_set_world_bbox(buf, grid, 5u, 0.0);
    }
}
