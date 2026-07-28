// voxelbvh_nanovdb_allocate_metadata.slang

#define PNANOVDB_HLSL
#define PNANOVDB_ADDRESS_64
#define PNANOVDB_BUF_HLSL_64
#define PNANOVDB_BUF_HLSL_RW
#include "PNanoVDB.h"

struct metadata_t
{
    pnanovdb_uint64_t element_size;
    pnanovdb_uint64_t metadata_count;
};

StructuredBuffer<metadata_t> metadatas;

RWStructuredBuffer<uint2> buf;

StructuredBuffer<uint64_t> total_count_in;

#include <workgroup_scan_uint64.slang>

[shader("compute")][numthreads(256, 1, 1)]
void main(uint3 group_idx : SV_GroupID, uint3 thread_idx : SV_GroupThreadID)
{
    if (group_idx.x != 0u)
    {
        return;
    }

    pnanovdb_grid_handle_t grid = {};
    pnanovdb_tree_handle_t tree = pnanovdb_grid_get_tree(buf, grid);
    pnanovdb_root_handle_t root = pnanovdb_tree_get_root(buf, tree);
    pnanovdb_grid_type_t grid_type = pnanovdb_grid_get_grid_type(buf, grid);

    uint64_t grid_size = pnanovdb_grid_get_grid_size(buf, grid);
    uint64_t metadata_count = metadatas[0u].metadata_count;
    if (thread_idx.x == 0u)
    {
        pnanovdb_grid_set_blind_metadata_offset(buf, grid, grid_size - grid.address.byte_offset);
        pnanovdb_grid_set_blind_metadata_count(buf, grid, metadata_count);
    }

    GroupMemoryBarrierWithGroupSync();

    uint64_t header_size = PNANOVDB_GRIDBLINDMETADATA_SIZE * metadata_count;

    uint64_t element_count = total_count_in[0u];

    uint64_t data_size = 0u;
    if (thread_idx.x < metadata_count)
    {
        data_size = metadatas[thread_idx.x].element_size * element_count;
    }
    data_size = 32u * ((data_size + 31u) / 32u);

    vector<uint64_t, 4> total_count4 = vector<uint64_t, 4>(data_size, 0llu, 0llu, 0llu);
    vector<uint64_t, 4> scan_total_count4 = vector<uint64_t, 4>(0llu, 0llu, 0llu, 0llu);
    uint64_t workgroup_total_count = 0llu;
    workgroup_scan_uint64(thread_idx.x, total_count4, scan_total_count4, workgroup_total_count);

    uint64_t data_offset = scan_total_count4.x - total_count4.x + grid_size + header_size;

    if (thread_idx.x < metadata_count)
    {
        pnanovdb_gridblindmetadata_handle_t meta = pnanovdb_grid_get_gridblindmetadata(buf, grid, thread_idx.x);

        pnanovdb_address_t meta_addr = pnanovdb_address_offset64(pnanovdb_address_null(), data_offset);

        pnanovdb_gridblindmetadata_set_data_offset(buf, meta, pnanovdb_address_diff(meta_addr, meta.address));
        pnanovdb_gridblindmetadata_set_value_count(buf, meta, element_count);
        pnanovdb_gridblindmetadata_set_value_size(buf, meta, metadatas[thread_idx.x].element_size);
        pnanovdb_gridblindmetadata_set_data_type(buf, meta, PNANOVDB_GRID_TYPE_RGBA8);
    }
    grid_size = grid_size + header_size + workgroup_total_count;
    if (thread_idx.x == 0u)
    {
        pnanovdb_grid_set_grid_size(buf, grid, grid_size);
    }
}
