// voxelbvh_integral_init.slang

#define PNANOVDB_HLSL
#define PNANOVDB_ADDRESS_64
#define PNANOVDB_BUF_HLSL_64
#define PNANOVDB_BUF_HLSL_RW
#include "PNanoVDB.h"

struct constants_t
{
    float4x4 view;
    float4x4 view_inv;
    uint resolution;
    uint dst_grid_type;
    uint pass_id;
};

ConstantBuffer<constants_t> constants;

RWStructuredBuffer<uint2> src_buf;
RWStructuredBuffer<uint2> buf;

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

struct mat_t
{
    float4 x, y, z, w;
};

float4x4 mat_inverse(const mat_t m)
{
    float f = (float(1.0) /
               (m.x.x * m.y.y * m.z.z * m.w.w + m.x.x * m.y.z * m.z.w * m.w.y + m.x.x * m.y.w * m.z.y * m.w.z +
                m.x.y * m.y.x * m.z.w * m.w.z + m.x.y * m.y.z * m.z.x * m.w.w + m.x.y * m.y.w * m.z.z * m.w.x +
                m.x.z * m.y.x * m.z.y * m.w.w + m.x.z * m.y.y * m.z.w * m.w.x + m.x.z * m.y.w * m.z.x * m.w.y +
                m.x.w * m.y.x * m.z.z * m.w.y + m.x.w * m.y.y * m.z.x * m.w.z + m.x.w * m.y.z * m.z.y * m.w.x +
                -m.x.x * m.y.y * m.z.w * m.w.z + -m.x.x * m.y.z * m.z.y * m.w.w + -m.x.x * m.y.w * m.z.z * m.w.y +
                -m.x.y * m.y.x * m.z.z * m.w.w + -m.x.y * m.y.z * m.z.w * m.w.x + -m.x.y * m.y.w * m.z.x * m.w.z +
                -m.x.z * m.y.x * m.z.w * m.w.y + -m.x.z * m.y.y * m.z.x * m.w.w + -m.x.z * m.y.w * m.z.y * m.w.x +
                -m.x.w * m.y.x * m.z.y * m.w.z + -m.x.w * m.y.y * m.z.z * m.w.x + -m.x.w * m.y.z * m.z.x * m.w.y));

    float a00 = (m.y.y * m.z.z * m.w.w + m.y.z * m.z.w * m.w.y + m.y.w * m.z.y * m.w.z + -m.y.y * m.z.w * m.w.z +
                 -m.y.z * m.z.y * m.w.w + -m.y.w * m.z.z * m.w.y);

    float a10 = (m.x.y * m.z.w * m.w.z + m.x.z * m.z.y * m.w.w + m.x.w * m.z.z * m.w.y + -m.x.y * m.z.z * m.w.w +
                 -m.x.z * m.z.w * m.w.y + -m.x.w * m.z.y * m.w.z);

    float a20 = (m.x.y * m.y.z * m.w.w + m.x.z * m.y.w * m.w.y + m.x.w * m.y.y * m.w.z + -m.x.y * m.y.w * m.w.z +
                 -m.x.z * m.y.y * m.w.w + -m.x.w * m.y.z * m.w.y);

    float a30 = (m.x.y * m.y.w * m.z.z + m.x.z * m.y.y * m.z.w + m.x.w * m.y.z * m.z.y + -m.x.y * m.y.z * m.z.w +
                 -m.x.z * m.y.w * m.z.y + -m.x.w * m.y.y * m.z.z);

    float a01 = (m.y.x * m.z.w * m.w.z + m.y.z * m.z.x * m.w.w + m.y.w * m.z.z * m.w.x + -m.y.x * m.z.z * m.w.w +
                 -m.y.z * m.z.w * m.w.x + -m.y.w * m.z.x * m.w.z);

    float a11 = (m.x.x * m.z.z * m.w.w + m.x.z * m.z.w * m.w.x + m.x.w * m.z.x * m.w.z + -m.x.x * m.z.w * m.w.z +
                 -m.x.z * m.z.x * m.w.w + -m.x.w * m.z.z * m.w.x);

    float a21 = (m.x.x * m.y.w * m.w.z + m.x.z * m.y.x * m.w.w + m.x.w * m.y.z * m.w.x + -m.x.x * m.y.z * m.w.w +
                 -m.x.z * m.y.w * m.w.x + -m.x.w * m.y.x * m.w.z);

    float a31 = (m.x.x * m.y.z * m.z.w + m.x.z * m.y.w * m.z.x + m.x.w * m.y.x * m.z.z + -m.x.x * m.y.w * m.z.z +
                 -m.x.z * m.y.x * m.z.w + -m.x.w * m.y.z * m.z.x);

    float a02 = (m.y.x * m.z.y * m.w.w + m.y.y * m.z.w * m.w.x + m.y.w * m.z.x * m.w.y + -m.y.x * m.z.w * m.w.y +
                 -m.y.y * m.z.x * m.w.w + -m.y.w * m.z.y * m.w.x);

    float a12 = (-m.x.x * m.z.y * m.w.w + -m.x.y * m.z.w * m.w.x + -m.x.w * m.z.x * m.w.y + m.x.x * m.z.w * m.w.y +
                 m.x.y * m.z.x * m.w.w + m.x.w * m.z.y * m.w.x);

    float a22 = (m.x.x * m.y.y * m.w.w + m.x.y * m.y.w * m.w.x + m.x.w * m.y.x * m.w.y + -m.x.x * m.y.w * m.w.y +
                 -m.x.y * m.y.x * m.w.w + -m.x.w * m.y.y * m.w.x);

    float a32 = (m.x.x * m.y.w * m.z.y + m.x.y * m.y.x * m.z.w + m.x.w * m.y.y * m.z.x + -m.x.y * m.y.w * m.z.x +
                 -m.x.w * m.y.x * m.z.y + -m.x.x * m.y.y * m.z.w);

    float a03 = (m.y.x * m.z.z * m.w.y + m.y.y * m.z.x * m.w.z + m.y.z * m.z.y * m.w.x + -m.y.x * m.z.y * m.w.z +
                 -m.y.y * m.z.z * m.w.x + -m.y.z * m.z.x * m.w.y);

    float a13 = (m.x.x * m.z.y * m.w.z + m.x.y * m.z.z * m.w.x + m.x.z * m.z.x * m.w.y + -m.x.x * m.z.z * m.w.y +
                 -m.x.y * m.z.x * m.w.z + -m.x.z * m.z.y * m.w.x);

    float a23 = (m.x.x * m.y.z * m.w.y + m.x.y * m.y.x * m.w.z + m.x.z * m.y.y * m.w.x + -m.x.x * m.y.y * m.w.z +
                 -m.x.y * m.y.z * m.w.x + -m.x.z * m.y.x * m.w.y);

    float a33 = (m.x.x * m.y.y * m.z.z + m.x.y * m.y.z * m.z.x + m.x.z * m.y.x * m.z.y + -m.x.x * m.y.z * m.z.y +
                 -m.x.y * m.y.x * m.z.z + -m.x.z * m.y.y * m.z.x);

    float4x4 ret = { a00 * f, a10 * f, a20 * f, a30 * f, a01 * f, a11 * f, a21 * f, a31 * f,
                                  a02 * f, a12 * f, a22 * f, a32 * f, a03 * f, a13 * f, a23 * f, a33 * f };
    return ret;
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

    pnanovdb_grid_handle_t grid = {};
    pnanovdb_tree_handle_t tree = pnanovdb_grid_get_tree(buf, grid);
    pnanovdb_root_handle_t root = pnanovdb_tree_get_root(buf, tree);

    uint copy_size = PNANOVDB_GRID_SIZE + PNANOVDB_TREE_SIZE;
    if (8u * thread_idx.x < copy_size)
    {
        buf[thread_idx.x] = src_buf[thread_idx.x];
    }

    GroupMemoryBarrierWithGroupSync();

    pnanovdb_grid_type_t grid_type = constants.dst_grid_type;
    pnanovdb_grid_set_grid_type(buf, grid, grid_type);

    if (thread_idx.x == 0)
    {
        // based on root bbox and upsample_factor, establish root tiles
        pnanovdb_coord_t bbox_min = pnanovdb_root_get_bbox_min(src_buf, src_root);
        pnanovdb_coord_t bbox_max = pnanovdb_root_get_bbox_max(src_buf, src_root);

        float3 aabb_min = pnanovdb_grid_index_to_worldf(src_buf, src_grid, float3(bbox_min));
        float3 aabb_max = pnanovdb_grid_index_to_worldf(src_buf, src_grid, float3(bbox_max) + 1.f);

        // view matrix takes world to camera space
        // voxels are in camera space
        // NanoVDB map describes index to world
        // So view aligns to map_inv

        // Take bounding sphere to NDC space
        float3 aabb_center = 0.5f * (aabb_max + aabb_min);
        float3 aabb_halfsize = 0.5f * (aabb_max - aabb_min);
        float radius = length(aabb_halfsize);
        float r_inv = 1.f / radius;
        float4x4 src_to_ndc = float4x4(
            r_inv, 0.f, 0.f, 0.f,
            0.f, r_inv, 0.f, 0.f,
            0.f, 0.f, r_inv, 0.f,
            -r_inv * aabb_center.x, -r_inv * aabb_center.y, -r_inv * aabb_center.z, 1.f
        );

        // Take camera space to index space
        float resf_half = 0.5f * float(constants.resolution);
        float4x4 view_to_index = float4x4(
            resf_half, 0.f, 0.f, 0.f,
            0.f, resf_half, 0.f, 0.f,
            0.f, 0.f, resf_half, 0.f,
            resf_half, resf_half, resf_half, 1.f
        );

        float4x4 world_to_index = mul(src_to_ndc, mul(constants.view, view_to_index));

        mat_t tmp_mat = {world_to_index[0], world_to_index[1], world_to_index[2], world_to_index[3]};
        float4x4 index_to_world = mat_inverse(tmp_mat);

        // TODO
        float voxel_size = 1.f;
        float voxel_size_inv = 1.f;

        pnanovdb_grid_set_voxel_size_float(buf, grid, 0u, voxel_size);
        pnanovdb_grid_set_voxel_size_float(buf, grid, 1u, voxel_size);
        pnanovdb_grid_set_voxel_size_float(buf, grid, 2u, voxel_size);

        // Slang operator[] does [row][column]
        // NanoVDB does column major in practice

        pnanovdb_map_handle_t map = pnanovdb_grid_get_map(buf, grid);
        pnanovdb_map_set_matf(buf, map, 0u, index_to_world[0][0]);
        pnanovdb_map_set_matf(buf, map, 1u, index_to_world[1][0]);
        pnanovdb_map_set_matf(buf, map, 2u, index_to_world[2][0]);
        pnanovdb_map_set_matf(buf, map, 3u, index_to_world[0][1]);
        pnanovdb_map_set_matf(buf, map, 4u, index_to_world[1][1]);
        pnanovdb_map_set_matf(buf, map, 5u, index_to_world[2][1]);
        pnanovdb_map_set_matf(buf, map, 6u, index_to_world[0][2]);
        pnanovdb_map_set_matf(buf, map, 7u, index_to_world[1][2]);
        pnanovdb_map_set_matf(buf, map, 8u, index_to_world[2][2]);

        pnanovdb_map_set_vecf(buf, map, 0u, index_to_world[3][0]);
        pnanovdb_map_set_vecf(buf, map, 1u, index_to_world[3][1]);
        pnanovdb_map_set_vecf(buf, map, 2u, index_to_world[3][2]);

        pnanovdb_map_set_invmatf(buf, map, 0u, world_to_index[0][0]);
        pnanovdb_map_set_invmatf(buf, map, 1u, world_to_index[1][0]);
        pnanovdb_map_set_invmatf(buf, map, 2u, world_to_index[2][0]);
        pnanovdb_map_set_invmatf(buf, map, 3u, world_to_index[0][1]);
        pnanovdb_map_set_invmatf(buf, map, 4u, world_to_index[1][1]);
        pnanovdb_map_set_invmatf(buf, map, 5u, world_to_index[2][1]);
        pnanovdb_map_set_invmatf(buf, map, 6u, world_to_index[0][2]);
        pnanovdb_map_set_invmatf(buf, map, 7u, world_to_index[1][2]);
        pnanovdb_map_set_invmatf(buf, map, 8u, world_to_index[2][2]);

        // set double precision variants
        pnanovdb_map_set_matd_float(buf, map, 0u, index_to_world[0][0]);
        pnanovdb_map_set_matd_float(buf, map, 1u, index_to_world[1][0]);
        pnanovdb_map_set_matd_float(buf, map, 2u, index_to_world[2][0]);
        pnanovdb_map_set_matd_float(buf, map, 3u, index_to_world[0][1]);
        pnanovdb_map_set_matd_float(buf, map, 4u, index_to_world[1][1]);
        pnanovdb_map_set_matd_float(buf, map, 5u, index_to_world[2][1]);
        pnanovdb_map_set_matd_float(buf, map, 6u, index_to_world[0][2]);
        pnanovdb_map_set_matd_float(buf, map, 7u, index_to_world[1][2]);
        pnanovdb_map_set_matd_float(buf, map, 8u, index_to_world[2][2]);

        pnanovdb_map_set_vecd_float(buf, map, 0u, index_to_world[3][0]);
        pnanovdb_map_set_vecd_float(buf, map, 1u, index_to_world[3][1]);
        pnanovdb_map_set_vecd_float(buf, map, 2u, index_to_world[3][2]);

        pnanovdb_map_set_invmatd_float(buf, map, 0u, world_to_index[0][0]);
        pnanovdb_map_set_invmatd_float(buf, map, 1u, world_to_index[1][0]);
        pnanovdb_map_set_invmatd_float(buf, map, 2u, world_to_index[2][0]);
        pnanovdb_map_set_invmatd_float(buf, map, 3u, world_to_index[0][1]);
        pnanovdb_map_set_invmatd_float(buf, map, 4u, world_to_index[1][1]);
        pnanovdb_map_set_invmatd_float(buf, map, 5u, world_to_index[2][1]);
        pnanovdb_map_set_invmatd_float(buf, map, 6u, world_to_index[0][2]);
        pnanovdb_map_set_invmatd_float(buf, map, 7u, world_to_index[1][2]);
        pnanovdb_map_set_invmatd_float(buf, map, 8u, world_to_index[2][2]);

        pnanovdb_grid_set_world_bbox_float(buf, grid, 0u, aabb_min.x);
        pnanovdb_grid_set_world_bbox_float(buf, grid, 1u, aabb_min.y);
        pnanovdb_grid_set_world_bbox_float(buf, grid, 2u, aabb_min.z);
        pnanovdb_grid_set_world_bbox_float(buf, grid, 3u, abb_max.x);
        pnanovdb_grid_set_world_bbox_float(buf, grid, 4u, aabb_max.y);
        pnanovdb_grid_set_world_bbox_float(buf, grid, 5u, aabb_max.z);
    }

    // compute tile grid based on resolution
    pnanovdb_coord_t root_ijk_min = {};
    pnanovdb_coord_t root_ijk_max = (constants.resolution + 4095) & ~4095;

    int3 root_dim = (root_ijk_max - root_ijk_min) >> 12;
    int root_dim1d = root_dim.x * root_dim.y * root_dim.z;
    if (root_dim1d >= 256u)
    {
        root_dim1d = 256u;
    }

    pnanovdb_root_set_tile_count(buf, root, root_dim1d);
    if (thread_idx.x < root_dim1d)
    {
        uint n = thread_idx.x;
        uint root_i = n / (root_dim.z * root_dim.y);
        uint root_j = (n / root_dim.z) % root_dim.y;
        uint root_k = n % root_dim.z;

        pnanovdb_root_tile_handle_t root_tile = pnanovdb_root_get_tile(grid_type, root, n);

        int3 root_coord = int3(
            root_i * 4096u + root_ijk_min.x,
            root_j * 4096u + root_ijk_min.y,
            root_k * 4096 + root_ijk_min.z
        );
        uint64_t key = pnanovdb_coord_to_key(root_coord);
        pnanovdb_root_tile_set_key(buf, root_tile, key);
    }

    if (thread_idx.x == 0u)
    {
        uint64_t grid_size = copy_size;
        grid_size += root_dim1d * PNANOVDB_GRID_TYPE_GET(grid_type, root_tile_size);
        pnanovdb_grid_set_grid_size(buf, grid, grid_size);
    }
}
