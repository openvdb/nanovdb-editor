// voxelbvh_nanovdb_level_list_alloc1.slang

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
        pnanovdb_uint32_t val_raw = pnanovdb_read_uint32(buf, val_addr);
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
