// voxelbvh_nanovdb_iterate.slang
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
RWStructuredBuffer<uint2> buf_scratch;

uint64_t uint2_to_uint64_t(uint2 v)
{
    return (uint64_t(v.y) << 32u) | uint64_t(v.x);
}

uint2 uint64_t_to_uint2(uint64_t v)
{
    return uint2(uint(v), uint(v >> 32u));
}

pnanovdb_coord_t key_to_coord(pnanovdb_uint64_t key)
{
    pnanovdb_coord_t ijk = { pnanovdb_uint32_as_int32(pnanovdb_uint64_to_uint32_lsr(key, 42u) << 12u),
                             pnanovdb_uint32_as_int32(pnanovdb_uint64_to_uint32_lsr(key, 21u) << 12u),
                             pnanovdb_uint32_as_int32(pnanovdb_uint64_to_uint32_lsr(key, 0u) << 12u) };
    return ijk;
}

// Forward declare callback
void iterate_main(pnanovdb_address_t node_addr, pnanovdb_address_t val_addr, uint level, uint n,
                  pnanovdb_coord_t ijk,  inout user_state_t user_state);

groupshared uint s_types[256u];

void do_iteration(uint group_idx, uint thread_idx, inout user_state_t user_state)
{
    uint pass_count = (constants.nanovdb_chunk_count + 65535u) / 65536u;
    for (uint pass_idx = 0u; pass_idx < pass_count; pass_idx++)
    {
        // fetch chunks for this worker, strided 256u
        // 4-bits per 64-bits
        uint chunk_idx = pass_idx * 65536u + group_idx + 256u * thread_idx;
        uint word_idx = chunk_idx >> 4u;
        uint shift = uint(chunk_idx & 15) << 2;
        uint64_t node_mask = 0llu;
        if (word_idx < constants.node_mask_uint64_count)
        {
            node_mask = uint2_to_uint64_t(node_mask_in[word_idx]);
        }
        uint type = uint(node_mask >> shift) & 15;

        s_types[thread_idx] = type;

        GroupMemoryBarrierWithGroupSync();

        for (uint s_idx = 0u; s_idx < 256u; s_idx++)
        {
            type = s_types[s_idx];
            if (type >= 1u && type <= 4u)
            {
                chunk_idx = pass_idx * 65536u + group_idx + 256u * s_idx;

                // Should this be avoided?
                pnanovdb_grid_handle_t grid = { pnanovdb_address_null() };
                pnanovdb_uint32_t grid_type = pnanovdb_grid_get_grid_type(buf, grid);

                if (type == 1u)
                {
                    pnanovdb_root_handle_t root = {uint64_t(chunk_idx) << 5u};

                    // background, workgroup wide callback, but only one is expected to write
                    {
                        pnanovdb_address_t val_addr = pnanovdb_address_null();
                        if (thread_idx == 0u)
                        {
                            val_addr = pnanovdb_root_get_background_address(grid_type, buf, root);
                        }

                        pnanovdb_coord_t ijk = {0, 0, 0};
                        iterate_main(root.address, val_addr, 4u, thread_idx, ijk, user_state);
                    }
                    pnanovdb_uint32_t root_tile_count = pnanovdb_root_get_tile_count(buf, root);
                    pnanovdb_uint32_t count = pnanovdb_root_get_tile_count(buf, root);
                    pnanovdb_uint32_t pass_count = (count + 255u) / 256u;
                    for (uint pass_idx = 0u; pass_idx < pass_count; pass_idx++)
                    {
                        uint n = thread_idx + 256u * pass_idx;

                        pnanovdb_coord_t ijk = {0, 0, 0};
                        pnanovdb_address_t val_addr = pnanovdb_address_null();
                        if (n < count)
                        {
                            pnanovdb_root_tile_handle_t tile = pnanovdb_root_get_tile(grid_type, root, n);

                            pnanovdb_uint64_t key = pnanovdb_root_tile_get_key(buf, tile);
                            ijk = key_to_coord(key);

                            val_addr = pnanovdb_root_tile_get_value_address(grid_type, buf, tile);
                            if (pnanovdb_root_tile_get_child_mask(buf, tile))
                            {
                                val_addr = pnanovdb_address_null();
                            }
                        }

                        iterate_main(root.address, val_addr, 3u, n, ijk, user_state);
                    }
                }
                else if (type == 2u)
                {
                    pnanovdb_upper_handle_t upper = {uint64_t(chunk_idx) << 5u};
                    for (uint n = thread_idx; n < PNANOVDB_UPPER_TABLE_COUNT; n+= 256u)
                    {
                        pnanovdb_coord_t ijk = pnanovdb_upper_get_bbox_min(buf, upper);
                        ijk.x += int(((n >> 10u) & 31) << 7u);
                        ijk.y += int(((n >> 5u) & 31) << 7u);
                        ijk.z += int((n & 31) << 7u);

                        pnanovdb_address_t val_addr = pnanovdb_upper_get_table_address(grid_type, buf, upper, n);
                        if (pnanovdb_upper_get_child_mask(buf, upper, n))
                        {
                            val_addr = pnanovdb_address_null();
                        }

                        iterate_main(upper.address, val_addr, 2u, n, ijk, user_state);
                    }
                }
                else if (type == 3u)
                {
                    pnanovdb_lower_handle_t lower = {uint64_t(chunk_idx) << 5u};
                    for (uint n = thread_idx; n < PNANOVDB_LOWER_TABLE_COUNT; n+= 256u)
                    {
                        pnanovdb_coord_t ijk = pnanovdb_lower_get_bbox_min(buf, lower);
                        ijk.x += int(((n >> 8u) & 15) << 3u);
                        ijk.y += int(((n >> 4u) & 15) << 3u);
                        ijk.z += int((n & 15) << 3u);

                        pnanovdb_address_t val_addr = pnanovdb_lower_get_table_address(grid_type, buf, lower, n);
                        if (pnanovdb_lower_get_child_mask(buf, lower, n))
                        {
                            val_addr = pnanovdb_address_null();
                        }

                        iterate_main(lower.address, val_addr, 1u, n, ijk, user_state);
                    }
                }
                else if (type == 4u)
                {
                    pnanovdb_leaf_handle_t leaf = {uint64_t(chunk_idx) << 5u};
                    for (uint n = thread_idx; n < PNANOVDB_LEAF_TABLE_COUNT; n+= 256u)
                    {
                        pnanovdb_coord_t ijk = pnanovdb_leaf_get_bbox_min(buf, leaf);
                        ijk.x += int((n >> 6u) & 7);
                        ijk.y += int((n >> 3u) & 7);
                        ijk.z += int(n & 7);

                        pnanovdb_address_t val_addr = pnanovdb_leaf_get_table_address(grid_type, buf, leaf, n);

                        iterate_main(leaf.address, val_addr, 0u, n, ijk, user_state);
                    }
                }
            }
        }

        GroupMemoryBarrierWithGroupSync();
    }
}
