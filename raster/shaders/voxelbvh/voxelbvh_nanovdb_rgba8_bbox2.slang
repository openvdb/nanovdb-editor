// voxelbvh_nanovdb_level_list_alloc2.slang

struct user_state_t
{
    int unused;
};

#include "voxelbvh_nanovdb_iterate.slang"

RWStructuredBuffer<int4> workgroup_bboxes;

#include <workgroup_scan.slang>

void iterate_main(pnanovdb_address_t node_arr, pnanovdb_address_t val_addr, uint level, uint n,
                  pnanovdb_coord_t ijk, inout user_state_t user_state)
{
    // not used
}

[shader("compute")][numthreads(256, 1, 1)]
void main(uint3 group_idx : SV_GroupID, uint3 thread_idx : SV_GroupThreadID)
{
    if (group_idx.x == 0)
    {
        int3 bbox_min = workgroup_bboxes[2u * thread_idx.x + 0u].xyz;
        int3 bbox_max = workgroup_bboxes[2u * thread_idx.x + 1u].xyz;

        workgroup_reduce_min_int(thread_idx.x, bbox_min.x, bbox_min.x);
        workgroup_reduce_min_int(thread_idx.x, bbox_min.y, bbox_min.y);
        workgroup_reduce_min_int(thread_idx.x, bbox_min.z, bbox_min.z);
        workgroup_reduce_max_int(thread_idx.x, bbox_max.x, bbox_max.x);
        workgroup_reduce_max_int(thread_idx.x, bbox_max.y, bbox_max.y);
        workgroup_reduce_max_int(thread_idx.x, bbox_max.z, bbox_max.z);

        if (thread_idx.x == 0)
        {
            // Note: single grid assumption
            pnanovdb_grid_handle_t grid = { pnanovdb_address_null() };
            pnanovdb_uint32_t grid_type = pnanovdb_grid_get_grid_type(buf, grid);
            pnanovdb_tree_handle_t tree = pnanovdb_grid_get_tree(buf, grid);
            pnanovdb_root_handle_t root = pnanovdb_tree_get_root(buf, tree);

            pnanovdb_root_set_bbox_min(buf, root, bbox_min);
            pnanovdb_root_set_bbox_max(buf, root, bbox_max);
        }
    }
}
