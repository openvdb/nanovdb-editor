// voxelbvh_nanovdb_rgba8_bbox1.slang

struct user_state_t
{
   int3 bbox_min;
   int3 bbox_max;
};

#include "voxelbvh_nanovdb_iterate.slang"
#include "voxelbvh_common.h"

RWStructuredBuffer<int4> workgroup_bboxes;

#include <workgroup_scan.slang>

void iterate_main(pnanovdb_address_t node_arr, pnanovdb_address_t val_addr, uint level, uint n,
                  pnanovdb_coord_t ijk, inout user_state_t user_state)
{
    if (!pnanovdb_address_is_null(val_addr))
    {
        // assuming single grid for now
        pnanovdb_grid_handle_t grid = {};
        pnanovdb_uint32_t grid_type = pnanovdb_grid_get_grid_type(buf, grid);
        pnanovdb_uint32_t val_raw = 0u;
        if (grid_type == PNANOVDB_GRID_TYPE_ONINDEX)
        {
            pnanovdb_gridblindmetadata_handle_t metadata = pnanovdb_grid_get_gridblindmetadata(buf, grid, 0u);
            pnanovdb_uint64_t val_index64;
            if (level == 0)
            {
                val_index64 = pnanovdb_leaf_onindex_get_value_index(buf, val_addr, ijk);
            }
            else
            {
                val_index64 = pnanovdb_read_uint64(buf, val_addr);
            }
            // write to metadata
            if (val_index64 != 0u)
            {
                pnanovdb_int64_t byte_offset = pnanovdb_gridblindmetadata_get_data_offset(buf, metadata);
                pnanovdb_address_t meta_val_addr =
                    pnanovdb_address_offset64(metadata.address, pnanovdb_int64_as_uint64(byte_offset));
                meta_val_addr = pnanovdb_address_offset64(meta_val_addr, val_index64 * 4u);
                val_raw = pnanovdb_read_uint32(buf, meta_val_addr);
            }
        }
        else
        {
            val_raw = pnanovdb_read_uint32(buf, val_addr);
        }
        if (val_raw != 0u)
        {
            uint dim = 1;
            if (level == 4) { dim = 4096; }
            if (level == 3) { dim = 4096; }
            if (level == 2) { dim = 128; }
            if (level == 1) { dim = 8; }
            user_state.bbox_min = min(user_state.bbox_min, ijk);
            user_state.bbox_max = max(user_state.bbox_max, ijk + int(dim - 1u));
        }
    }
}

[shader("compute")][numthreads(256, 1, 1)]
void main(uint3 group_idx : SV_GroupID, uint3 thread_idx : SV_GroupThreadID)
{
    user_state_t user_state = {
        int3(WORKGROUP_INT_MAX, WORKGROUP_INT_MAX, WORKGROUP_INT_MAX),
        int3(WORKGROUP_INT_MIN, WORKGROUP_INT_MIN, WORKGROUP_INT_MIN)
    };
    do_iteration(group_idx.x, thread_idx.x, user_state);

    workgroup_reduce_min_int(thread_idx.x, user_state.bbox_min.x, user_state.bbox_min.x);
    workgroup_reduce_min_int(thread_idx.x, user_state.bbox_min.y, user_state.bbox_min.y);
    workgroup_reduce_min_int(thread_idx.x, user_state.bbox_min.z, user_state.bbox_min.z);
    workgroup_reduce_max_int(thread_idx.x, user_state.bbox_max.x, user_state.bbox_max.x);
    workgroup_reduce_max_int(thread_idx.x, user_state.bbox_max.y, user_state.bbox_max.y);
    workgroup_reduce_max_int(thread_idx.x, user_state.bbox_max.z, user_state.bbox_max.z);

    if (thread_idx.x == 0)
    {
        workgroup_bboxes[2u * group_idx.x + 0u] = int4(user_state.bbox_min, 0);
        workgroup_bboxes[2u * group_idx.x + 1u] = int4(user_state.bbox_max, 0);
    }
}
