// voxelbvh_nanovdb_find_leafs.slang
#define PNANOVDB_HLSL
#define PNANOVDB_ADDRESS_64
#define PNANOVDB_BUF_HLSL_64
#define PNANOVDB_BUF_HLSL_RW
#include "PNanoVDB.h"

struct constants_t
{
    uint nanovdb_word_count;
    uint ijkl_count;
    uint pad1;
    uint pad2;
};

ConstantBuffer<constants_t> constants;

StructuredBuffer<uint2> ijkl_in;

RWStructuredBuffer<uint2> buf;

void nanovdb_set_bit(pnanovdb_address_t addr, uint bit_idx)
{
    uint64_t word_idx = (addr.byte_offset >> 3u) + (bit_idx >> 6u);
    uint64_t val = uint64_t(1u) << (bit_idx & 63u);
    uint2 val2 = uint2(uint(val), uint(val >> 32u));
    if (val2.x != 0u) { InterlockedOr(buf[word_idx].x, val2.x); };
    if (val2.y != 0u) { InterlockedOr(buf[word_idx].y, val2.y); };
}

uint64_t uint2_to_uint64_t(uint2 v)
{
    return (uint64_t(v.y) << 32u) | uint64_t(v.x);
}

[shader("compute")][numthreads(256, 1, 1)]
void main(uint3 group_idx : SV_GroupID, uint3 thread_idx : SV_GroupThreadID)
{
    uint idx = group_idx.x * 256u + thread_idx.x;

    if (idx >= constants.ijkl_count)
    {
        return;
    }

    uint2 ijkl_raw = ijkl_in[idx];
    int4 ijkl = int4(
        int(ijkl_raw.y) >> 16,
        int(ijkl_raw.y << 16) >> 16,
        int(ijkl_raw.x) >> 16,
        int(ijkl_raw.x << 16) >> 16
    );

    pnanovdb_grid_handle_t grid = { pnanovdb_address_null() };
    pnanovdb_tree_handle_t tree = pnanovdb_grid_get_tree(buf, grid);
    pnanovdb_root_handle_t root = pnanovdb_tree_get_root(buf, tree);
    pnanovdb_grid_type_t grid_type = pnanovdb_grid_get_grid_type(buf, grid);

    pnanovdb_readaccessor_t acc;
    pnanovdb_readaccessor_init(PNANOVDB_REF(acc), root);

    pnanovdb_uint32_t level = 0u;
    pnanovdb_address_t address =
        pnanovdb_readaccessor_get_value_address_and_level(grid_type, buf, PNANOVDB_REF(acc), ijkl.xyz, level);

    if (level == 0u && ijkl.w < 3) // leaf
    {
        pnanovdb_leaf_handle_t leaf = acc.leaf;
        pnanovdb_address_t addr = pnanovdb_address_offset(leaf.address, PNANOVDB_LEAF_OFF_VALUE_MASK);
        pnanovdb_uint32_t n = pnanovdb_leaf_coord_to_offset(ijkl.xyz);
        nanovdb_set_bit(addr, n);
    }
    else if (level == 1u && ijkl.w < 7) // lower
    {
        pnanovdb_lower_handle_t lower = acc.lower;
        pnanovdb_address_t addr = pnanovdb_address_offset(lower.address, PNANOVDB_LOWER_OFF_CHILD_MASK);
        pnanovdb_uint32_t n = pnanovdb_lower_coord_to_offset(ijkl.xyz);
        nanovdb_set_bit(addr, n);
    }
    else if (level == 2u && ijkl.w < 12) // upper
    {
        pnanovdb_upper_handle_t upper = acc.upper;
        pnanovdb_address_t addr = pnanovdb_address_offset(upper.address, PNANOVDB_UPPER_OFF_CHILD_MASK);
        pnanovdb_uint32_t n = pnanovdb_upper_coord_to_offset(ijkl.xyz);
        nanovdb_set_bit(addr, n);
    }
    else if (level == 3u) // root tile
    {
        // root tile has no direct mask, so we mark it with illegal offset 1u
        // this illegal offset will be overridden with a valid offset after allocation
        pnanovdb_root_tile_handle_t root_tile = pnanovdb_root_find_tile(grid_type, buf, acc.root, ijkl.xyz);
        if (!pnanovdb_address_is_null(root_tile.address))
        {
            pnanovdb_root_tile_set_child(buf, root_tile, int64_t(1));
        }
    }
    else if (level == 4u) // root background
    {
        // this means missing root tile, will quietly fail
    }
}
