// voxelbvh_nanovdb_to_bbox.slang

#define PNANOVDB_HLSL
#define PNANOVDB_ADDRESS_64
#define PNANOVDB_BUF_HLSL_64
//#define PNANOVDB_BUF_HLSL_RW
#include "PNanoVDB.h"

StructuredBuffer<uint2> buf;

RWStructuredBuffer<float> world_bbox_out;

[shader("compute")][numthreads(32, 1, 1)]
void main(uint3 group_idx : SV_GroupID, uint3 thread_idx : SV_GroupThreadID)
{
    pnanovdb_grid_handle_t grid = {};
    pnanovdb_tree_handle_t tree = pnanovdb_grid_get_tree(buf, grid);
    pnanovdb_root_handle_t root = pnanovdb_tree_get_root(buf, tree);
    pnanovdb_grid_type_t grid_type = pnanovdb_grid_get_grid_type(buf, grid);

    if (thread_idx.x < 6u)
    {
        world_bbox_out[thread_idx.x] = float(pnanovdb_grid_get_world_bbox(buf, grid, thread_idx.x));
    }
}
