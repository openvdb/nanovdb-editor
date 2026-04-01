// voxelbvh_nanovdb_level_list_splat.slang
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
RWStructuredBuffer<uint2> range_flat_out;

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

    pnanovdb_uint64_t val = pnanovdb_read_uint64(buf, address);

    uint active_lmask = uint(val & 0xFFFF);
    uint list_begin_idx = uint(val >> 32u);
    uint list_idx = list_begin_idx + countbits(active_lmask & ~lmask);

    range_flat_out[list_idx] = range;
}
