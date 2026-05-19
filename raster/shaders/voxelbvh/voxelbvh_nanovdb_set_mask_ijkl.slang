// voxelbvh_nanovdb_set_mask_ijkl.slang
#define PNANOVDB_HLSL
#define PNANOVDB_ADDRESS_64
#define PNANOVDB_BUF_HLSL_64
#define PNANOVDB_BUF_HLSL_RW
#include "PNanoVDB.h"

struct constants_t
{
    uint nanovdb_word_count;
    uint ijkl_count;
    uint nanovdb_chunk_count;
    uint node_mask_uint64_count;
    uint range_count;
};

ConstantBuffer<constants_t> constants;

StructuredBuffer<uint2> ijkl_in;
StructuredBuffer<uint2> range_in;

RWStructuredBuffer<uint2> buf;
RWStructuredBuffer<uint2> range_scratch;

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
uint2 uint64_t_to_uint2(uint64_t v)
{
    return uint2(uint(v), uint(v >> 32u));
}

[shader("compute")][numthreads(256, 1, 1)]
void main(uint3 group_idx : SV_GroupID, uint3 thread_idx : SV_GroupThreadID)
{
    uint idx = group_idx.x * 256u + thread_idx.x;

    if (idx >= constants.range_count)
    {
        return;
    }
    uint2 range = range_in[idx];
    if (range.x >= range.y || range.x >= constants.ijkl_count)
    {
        return;
    }
    uint2 ijkl_raw = ijkl_in[range.x];
    if (ijkl_raw.x == ~0u && ijkl_raw.y == ~0u)
    {
        return;
    }
    int4 ijkl = int4(
        int(ijkl_raw.y) >> 16,
        int(ijkl_raw.y << 16) >> 16,
        int(ijkl_raw.x) >> 16,
        int(ijkl_raw.x << 16) >> 16
    );
    if (ijkl.w == 65535)
    {
        return;
    }
    // squash lsbs to zero for higher levels
    uint lmask = ~((1u << ijkl.w) - 1u);
    ijkl.xyz = ijkl.xyz & lmask;

    pnanovdb_grid_handle_t grid = { pnanovdb_address_null() };
    pnanovdb_tree_handle_t tree = pnanovdb_grid_get_tree(buf, grid);
    pnanovdb_root_handle_t root = pnanovdb_tree_get_root(buf, tree);
    pnanovdb_grid_type_t grid_type = pnanovdb_grid_get_grid_type(buf, grid);

    pnanovdb_readaccessor_t acc;
    pnanovdb_readaccessor_init(PNANOVDB_REF(acc), root);

    pnanovdb_uint32_t level = 0u;
    pnanovdb_address_t address =
        pnanovdb_readaccessor_get_value_address_and_level(grid_type, buf, PNANOVDB_REF(acc), ijkl.xyz, level);

    uint64_t range_scratch_val = uint64_t(0u);
    if (level == 1u && ijkl.w < 3) // lower
    {
        pnanovdb_lower_handle_t lower = acc.lower;
        pnanovdb_address_t addr = pnanovdb_address_offset(lower.address, PNANOVDB_LOWER_OFF_CHILD_MASK);
        pnanovdb_uint32_t n = pnanovdb_lower_coord_to_offset(ijkl.xyz);
        range_scratch_val = addr.byte_offset | (uint64_t(n) << 48u);
        //nanovdb_set_bit(addr, n);
    }
    else if (level == 2u && ijkl.w < 7) // upper
    {
        pnanovdb_upper_handle_t upper = acc.upper;
        pnanovdb_address_t addr = pnanovdb_address_offset(upper.address, PNANOVDB_UPPER_OFF_CHILD_MASK);
        pnanovdb_uint32_t n = pnanovdb_upper_coord_to_offset(ijkl.xyz);
        range_scratch_val = addr.byte_offset | (uint64_t(n) << 48u);
        //nanovdb_set_bit(addr, n);
    }
    else if (level == 3u && ijkl.w < 12) // root tile
    {
        // root tile has no direct mask, so we mark it with illegal offset 1u
        // this illegal offset will be overridden with a valid offset after allocation
        pnanovdb_root_tile_handle_t root_tile = pnanovdb_root_find_tile(grid_type, buf, acc.root, ijkl.xyz);
        if (!pnanovdb_address_is_null(root_tile.address))
        {
            //pnanovdb_root_tile_set_child(buf, root_tile, int64_t(1));
            pnanovdb_address_t addr = pnanovdb_address_offset(root_tile.address, PNANOVDB_ROOT_TILE_OFF_CHILD);
            uint n = 0u;
            range_scratch_val = addr.byte_offset | (uint64_t(n) << 48u);
        }
    }
    else if (level == 4u) // root background
    {
        // this means missing root tile, will quietly fail
    }
    range_scratch[idx] = uint64_t_to_uint2(range_scratch_val);
}
