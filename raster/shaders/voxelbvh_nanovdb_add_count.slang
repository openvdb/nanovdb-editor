// voxelbvh_nanovdb_add_count.slang
#define PNANOVDB_HLSL
#define PNANOVDB_ADDRESS_64
#define PNANOVDB_BUF_HLSL_64
// #define PNANOVDB_BUF_HLSL_RW
#include "PNanoVDB.h"

struct constants_t
{
    uint nanovdb_word_count;
    uint pad0;
    uint nanovdb_chunk_count;
    uint node_mask_uint64_count;
};

ConstantBuffer<constants_t> constants;
StructuredBuffer<uint2> buf;
StructuredBuffer<uint2> node_mask_in;
RWStructuredBuffer<uint2> workgroup_counters_out;

uint64_t uint2_to_uint64_t(uint2 v)
{
    return (uint64_t(v.y) << 32u) | uint64_t(v.x);
}

uint2 uint64_t_to_uint2(uint64_t v)
{
    return uint2(uint(v), uint(v >> 32u));
}

groupshared uint s_types[256u];

#include <workgroup_scan_uint64.slang>

[shader("compute")][numthreads(256, 1, 1)]
void main(uint3 group_idx : SV_GroupID, uint3 thread_idx : SV_GroupThreadID)
{
    uint64_t total_new_bytes = 0llu;
    uint64_t max_current_addr = 0llu;

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
            if (type == 1u || type == 2u || type == 3u)
            {
                chunk_idx = pass_idx * 65536u + group_idx.x + 256u * s_idx;

                // Should this be avoided?
                pnanovdb_grid_handle_t grid = { pnanovdb_address_null() };
                pnanovdb_uint32_t grid_type = pnanovdb_grid_get_grid_type(buf, grid);

                if (type == 1u)
                {
                    pnanovdb_root_handle_t root = {uint64_t(chunk_idx) << 5u};

                    max_current_addr = max(max_current_addr,
                        root.address_byte_offset + PNANOVDB_GRID_TYPE_GET(grid_type, root_size));

                    pnanovdb_uint32_t count = pnanovdb_root_get_tile_count(buf, root);
                    for (uint tile_idx = thread_idx.x; tile_idx < count; tile_idx += 256u)
                    {
                        pnanovdb_root_tile_handle_t tile = pnanovdb_root_get_tile(grid_type, root, tile_idx);
                        if (pnanovdb_root_tile_get_child_mask(buf, tile))
                        {
                            pnanovdb_upper_handle_t child = pnanovdb_root_get_child(grid_type, buf, root, tile);
                            if (pnanovdb_address_is_null(child.address))
                            {
                                total_new_bytes += PNANOVDB_GRID_TYPE_GET(grid_type, upper_size);
                            }
                        }
                    }
                }
                else if (type == 2u)
                {
                    pnanovdb_upper_handle_t upper = {uint64_t(chunk_idx) << 5u};

                    max_current_addr = max(max_current_addr,
                        upper.address_byte_offset + PNANOVDB_GRID_TYPE_GET(grid_type, upper_size));

                    for (uint n = thread_idx.x; n < PNANOVDB_UPPER_TABLE_COUNT; n+= 256u)
                    {
                        if (pnanovdb_upper_get_child_mask(buf, upper, n))
                        {
                            pnanovdb_lower_handle_t child = pnanovdb_upper_get_child(grid_type, buf, upper, n);
                            if (pnanovdb_address_is_null(child.address))
                            {
                                total_new_bytes += PNANOVDB_GRID_TYPE_GET(grid_type, lower_size);
                            }
                        }
                    }
                }
                else if (type == 3u)
                {
                    pnanovdb_lower_handle_t lower = {uint64_t(chunk_idx) << 5u};

                    max_current_addr = max(max_current_addr,
                        lower.address_byte_offset + PNANOVDB_GRID_TYPE_GET(grid_type, lower_size));

                    for (uint n = thread_idx.x; n < PNANOVDB_LOWER_TABLE_COUNT; n+= 256u)
                    {
                        if (pnanovdb_lower_get_child_mask(buf, lower, n))
                        {
                            pnanovdb_leaf_handle_t child = pnanovdb_lower_get_child(grid_type, buf, lower, n);
                            if (pnanovdb_address_is_null(child.address))
                            {
                                total_new_bytes += PNANOVDB_GRID_TYPE_GET(grid_type, leaf_size);
                            }
                        }
                    }
                }
                else if (type == 4u)
                {
                    pnanovdb_leaf_handle_t leaf = {uint64_t(chunk_idx) << 5u};

                    max_current_addr = max(max_current_addr,
                        leaf.address_byte_offset + PNANOVDB_GRID_TYPE_GET(grid_type, leaf_size));
                }
            }
        }

        GroupMemoryBarrierWithGroupSync();
    }

    // reduce all the memory requests for this workgroup
    vector<uint64_t, 4> total_new_bytes4 = vector<uint64_t, 4>(total_new_bytes, 0llu, 0llu, 0llu);
    uint64_t workgroup_total_new_bytes = 0llu;
    workgroup_reduce_uint64(thread_idx.x, total_new_bytes4, workgroup_total_new_bytes);

    if (thread_idx.x == 0)
    {
        workgroup_counters_out[group_idx.x] = uint64_t_to_uint2(workgroup_total_new_bytes);
    }

    vector<uint64_t, 4> max_current_addr4 = vector<uint64_t, 4>(max_current_addr, 0llu, 0llu, 0llu);
    uint64_t workgroup_max_current_addr = 0llu;
    workgroup_uint64_reduce_max(thread_idx.x, max_current_addr4, workgroup_max_current_addr);

    if (thread_idx.x == 0)
    {
        workgroup_counters_out[256u + group_idx.x] = uint64_t_to_uint2(workgroup_max_current_addr);
    }
}
