// voxelbvh_nanovdb_add_link.slang
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
RWStructuredBuffer<uint2> workgroup_counters_in;

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

                    pnanovdb_uint32_t count = pnanovdb_root_get_tile_count(buf, root);
                    for (uint tile_idx = thread_idx.x; tile_idx < count; tile_idx += 256u)
                    {
                        pnanovdb_root_tile_handle_t tile = pnanovdb_root_get_tile(grid_type, root, tile_idx);
                        int64_t child_off = pnanovdb_root_tile_get_child(buf, tile);
                        if (child_off == 1)
                        {
                            total_new_bytes += PNANOVDB_GRID_TYPE_GET(grid_type, upper_size);
                        }
                    }
                }
                else if (type == 2u)
                {
                    pnanovdb_upper_handle_t upper = {uint64_t(chunk_idx) << 5u};

                    for (uint n = thread_idx.x; n < PNANOVDB_UPPER_TABLE_COUNT; n+= 256u)
                    {
                        if (pnanovdb_upper_get_child_mask(buf, upper, n))
                        {
                            pnanovdb_lower_handle_t child = pnanovdb_upper_get_child(grid_type, buf, upper, n);
                            if (child.address.byte_offset == upper.address.byte_offset)
                            {
                                total_new_bytes += PNANOVDB_GRID_TYPE_GET(grid_type, lower_size);
                            }
                        }
                    }
                }
                else if (type == 3u)
                {
                    pnanovdb_lower_handle_t lower = {uint64_t(chunk_idx) << 5u};

                    for (uint n = thread_idx.x; n < PNANOVDB_LOWER_TABLE_COUNT; n+= 256u)
                    {
                        if (pnanovdb_lower_get_child_mask(buf, lower, n))
                        {
                            pnanovdb_leaf_handle_t child = pnanovdb_lower_get_child(grid_type, buf, lower, n);
                            if (child.address.byte_offset == lower.address.byte_offset)
                            {
                                total_new_bytes += PNANOVDB_GRID_TYPE_GET(grid_type, leaf_size);
                            }
                        }
                    }
                }
            }
        }

        GroupMemoryBarrierWithGroupSync();
    }

    // scan all the memory requests for this workgroup
    vector<uint64_t, 4> total_new_bytes4 = vector<uint64_t, 4>(total_new_bytes, 0llu, 0llu, 0llu);
    vector<uint64_t, 4> scan_total_new_bytes4 = vector<uint64_t, 4>(0llu, 0llu, 0llu, 0llu);
    uint64_t workgroup_total_new_bytes = 0llu;
    workgroup_scan_uint64(thread_idx.x, total_new_bytes4, scan_total_new_bytes4, workgroup_total_new_bytes);

    uint64_t global_total_new_bytes = uint2_to_uint64_t(workgroup_counters_in[group_idx.x]);
    uint64_t global_scan_total_new_bytes = uint2_to_uint64_t(workgroup_counters_in[512u + group_idx.x]);
    uint64_t global_begin_offset = global_scan_total_new_bytes - global_total_new_bytes;

    uint64_t allocation_offset = global_begin_offset + scan_total_new_bytes4.x - total_new_bytes4.x;

    // place allocation after existing
    allocation_offset += uint2_to_uint64_t(workgroup_counters_in[768u]);

    // now we can sweep again know final allocation offsets
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

                    pnanovdb_uint32_t count = pnanovdb_root_get_tile_count(buf, root);
                    for (uint tile_idx = thread_idx.x; tile_idx < count; tile_idx += 256u)
                    {
                        pnanovdb_root_tile_handle_t tile = pnanovdb_root_get_tile(grid_type, root, tile_idx);
                        int64_t child_off = pnanovdb_root_tile_get_child(buf, tile);
                        if (child_off == 1)
                        {
                            pnanovdb_upper_handle_t child = {allocation_offset};
                            pnanovdb_root_set_child(grid_type, buf, root, tile, child);
                            allocation_offset += PNANOVDB_GRID_TYPE_GET(grid_type, upper_size);
                        }
                    }
                }
                else if (type == 2u)
                {
                    pnanovdb_upper_handle_t upper = {uint64_t(chunk_idx) << 5u};

                    for (uint n = thread_idx.x; n < PNANOVDB_UPPER_TABLE_COUNT; n+= 256u)
                    {
                        if (pnanovdb_upper_get_child_mask(buf, upper, n))
                        {
                            pnanovdb_lower_handle_t child = pnanovdb_upper_get_child(grid_type, buf, upper, n);
                            if (child.address.byte_offset == upper.address.byte_offset)
                            {
                                child.address.byte_offset = allocation_offset;
                                pnanovdb_upper_set_child(grid_type, buf, upper, n, child);
                                allocation_offset += PNANOVDB_GRID_TYPE_GET(grid_type, lower_size);
                            }
                        }
                    }
                }
                else if (type == 3u)
                {
                    pnanovdb_lower_handle_t lower = {uint64_t(chunk_idx) << 5u};

                    for (uint n = thread_idx.x; n < PNANOVDB_LOWER_TABLE_COUNT; n+= 256u)
                    {
                        if (pnanovdb_lower_get_child_mask(buf, lower, n))
                        {
                            pnanovdb_leaf_handle_t child = pnanovdb_lower_get_child(grid_type, buf, lower, n);
                            if (child.address.byte_offset == lower.address.byte_offset)
                            {
                                child.address.byte_offset = allocation_offset;
                                pnanovdb_lower_set_child(grid_type, buf, lower, n, child);
                                allocation_offset += PNANOVDB_GRID_TYPE_GET(grid_type, leaf_size);
                            }
                        }
                    }
                }
            }
        }

        GroupMemoryBarrierWithGroupSync();
    }
}
