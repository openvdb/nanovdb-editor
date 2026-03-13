// voxelbvh_nanovdb_level_list_flatten.slang
#define PNANOVDB_HLSL
#define PNANOVDB_ADDRESS_64
#define PNANOVDB_BUF_HLSL_64
#define PNANOVDB_BUF_HLSL_RW
#include "PNanoVDB.h"

struct constants_t
{
    uint nanovdb_word_count;
    uint pad0;
    uint nanovdb_chunk_count;
    uint node_mask_uint64_count;
};

ConstantBuffer<constants_t> constants;
RWStructuredBuffer<uint2> buf;
StructuredBuffer<uint2> node_mask_in;

uint64_t uint2_to_uint64_t(uint2 v)
{
    return (uint64_t(v.y) << 32u) | uint64_t(v.x);
}

uint2 uint64_t_to_uint2(uint64_t v)
{
    return uint2(uint(v), uint(v >> 32u));
}

groupshared uint s_types[256u];

[shader("compute")][numthreads(256, 1, 1)]
void main(uint3 group_idx : SV_GroupID, uint3 thread_idx : SV_GroupThreadID)
{
    uint pass_count = (constants.nanovdb_chunk_count + 65535u) / 65536u;
    for (uint pass_idx = 0u; pass_idx < pass_count; pass_idx++)
    {
        // fetch chunks for this worker, strided 256u
        // 4-bits per 64-bits
        uint chunk_idx = pass_idx * 65536u + group_idx.x + 256u * thread_idx.x;
        uint word_idx = chunk_idx >> 4u;
        uint shift = uint(chunk_idx & 15) << 2;
        uint64_t node_mask = 0llu;
        if (word_idx < constants.node_mask_uint64_count)
        {
            node_mask = uint2_to_uint64_t(node_mask_in[word_idx]);
        }
        uint type = uint(node_mask >> shift) & 15;

        s_types[thread_idx.x] = type;

        GroupMemoryBarrierWithGroupSync();

        for (uint s_idx = 0u; s_idx < 256u; s_idx++)
        {
            type = s_types[s_idx];
            if (type >= 1u && type <= 4u)
            {
                chunk_idx = pass_idx * 65536u + group_idx.x + 256u * s_idx;

                // Should this be avoided?
                pnanovdb_grid_handle_t grid = { pnanovdb_address_null() };
                pnanovdb_uint32_t grid_type = pnanovdb_grid_get_grid_type(buf, grid);

                if (type == 1u)
                {
                    pnanovdb_root_handle_t root = {uint64_t(chunk_idx) << 5u};

                    // flattening root tiles isn't needed yet
                    //pnanovdb_uint32_t root_tile_count = pnanovdb_root_get_tile_count(buf, root);
                    //pnanovdb_uint32_t count = pnanovdb_root_get_tile_count(buf, root);
                    //for (uint tile_idx = thread_idx.x; tile_idx < count; tile_idx += 256u)
                    //{
                    //    pnanovdb_root_tile_handle_t tile = pnanovdb_root_get_tile(grid_type, root, tile_idx);
                    //}
                }
                else if (type == 2u)
                {
                    pnanovdb_upper_handle_t upper = {uint64_t(chunk_idx) << 5u};
                    for (uint n = thread_idx.x; n < PNANOVDB_UPPER_TABLE_COUNT; n+= 256u)
                    {
                        if (!pnanovdb_upper_get_child_mask(buf, upper, n))
                        {
                            pnanovdb_coord_t ijk = pnanovdb_upper_get_bbox_min(buf, upper);
                            ijk.x += int(((n >> 10u) & 31) << 7u);
                            ijk.y += int(((n >> 5u) & 31) << 7u);
                            ijk.z += int((n & 31) << 7u);

                        }
                    }
                }
                else if (type == 3u)
                {
                    pnanovdb_lower_handle_t lower = {uint64_t(chunk_idx) << 5u};
                    for (uint n = thread_idx.x; n < PNANOVDB_LOWER_TABLE_COUNT; n+= 256u)
                    {
                        if (!pnanovdb_lower_get_child_mask(buf, lower, n))
                        {
                            pnanovdb_coord_t ijk = pnanovdb_lower_get_bbox_min(buf, lower);
                            ijk.x += int(((n >> 8u) & 15) << 3u);
                            ijk.y += int(((n >> 4u) & 15) << 3u);
                            ijk.z += int((n & 15) << 3u);

                        }
                    }
                }
                else if (type == 4u)
                {
                    pnanovdb_leaf_handle_t leaf = {uint64_t(chunk_idx) << 5u};
                    for (uint n = thread_idx.x; n < PNANOVDB_LEAF_TABLE_COUNT; n+= 256u)
                    {
                        if (pnanovdb_leaf_get_value_mask(buf, leaf, n))
                        {
                            pnanovdb_coord_t ijk = pnanovdb_leaf_get_bbox_min(buf, leaf);
                            ijk.x += int((n >> 6u) & 7);
                            ijk.y += int((n >> 3u) & 7);
                            ijk.z += int(n & 7);

                        }
                    }
                }
            }
        }

        GroupMemoryBarrierWithGroupSync();
    }
}
