// voxelbvh_nanovdb_find_root.slang
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

[shader("compute")][numthreads(256, 1, 1)]
void main(uint3 group_idx : SV_GroupID, uint3 thread_idx : SV_GroupThreadID)
{
    if (group_idx.x != 0u)
    {
        return;
    }
    if (thread_idx.x != 0u)
    {
        return;
    }

    pnanovdb_grid_handle_t grid = { pnanovdb_address_null() };
    pnanovdb_tree_handle_t tree = pnanovdb_grid_get_tree(buf, grid);
    pnanovdb_root_handle_t root = pnanovdb_tree_get_root(buf, tree);

    if (!pnanovdb_address_is_null(root.address))
    {
        set_mask(root.address, 1u);
    }
}
