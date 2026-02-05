// voxelbvh_nanovdb_find_lowers.slang
#define PNANOVDB_HLSL
#define PNANOVDB_ADDRESS_64
#define PNANOVDB_BUF_HLSL_64
// #define PNANOVDB_BUF_HLSL_RW
#include "PNanoVDB.h"

struct constants_t
{
    uint nanovdb_word_count;
    uint node_mask_element_count;
    uint nanovdb_chunk_count;
    uint node_mask_uint64_count;
};

ConstantBuffer<constants_t> constants;
StructuredBuffer<uint2> buf;

RWStructuredBuffer<uint2> node_mask_inout;

void set_mask(pnanovdb_address_t addr, uint type)
{
    // 4-bit type per 256-bit chunk
    uint64_t chunk_idx = addr.byte_offset >> 5u;
    // 16 chunks per 64-bit mask word
    uint64_t word_idx = chunk_idx >> 4u;
    uint shift = uint(chunk_idx & 15) << 2;
    uint64_t val = uint64_t(type) << shift;
    uint2 val2 = uint2(uint(val), uint(val >> 32u));
    if (val2.x != 0u) { InterlockedOr(node_mask_inout[word_idx].x, val2.x); };
    if (val2.y != 0u) { InterlockedOr(node_mask_inout[word_idx].y, val2.y); };
}

uint64_t uint2_to_uint64_t(uint2 v)
{
    return (uint64_t(v.y) << 32u) | uint64_t(v.x);
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
            node_mask = uint2_to_uint64_t(node_mask_inout[word_idx]);
        }
        uint type = uint(node_mask >> shift) & 15;

        s_types[thread_idx.x] = type;

        GroupMemoryBarrierWithGroupSync();

        for (uint s_idx = 0u; s_idx < 256u; s_idx++)
        {
            type = s_types[s_idx];
            if (type == 2u)
            {
                chunk_idx = pass_idx * 65536u + group_idx.x + 256u * s_idx;

                // Should this be avoided?
                pnanovdb_grid_handle_t grid = { pnanovdb_address_null() };
                pnanovdb_uint32_t grid_type = pnanovdb_grid_get_grid_type(buf, grid);

                pnanovdb_upper_handle_t upper = {chunk_idx << 5u};

                for (uint n = thread_idx.x; n < PNANOVDB_UPPER_TABLE_COUNT; n+= 256u)
                {
                    if (pnanovdb_upper_get_child_mask(buf, upper, n))
                    {
                        pnanovdb_lower_handle_t child = pnanovdb_upper_get_child(grid_type, buf, upper, n);
                        if (!pnanovdb_address_is_null(child.address))
                        {
                            set_mask(child.address, 3u);
                        }
                    }
                }
            }
        }

        GroupMemoryBarrierWithGroupSync();
    }
}
