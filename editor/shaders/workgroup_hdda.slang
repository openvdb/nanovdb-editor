// workgroup_hdda.slang

#include "hdda.slang"

groupshared uint s_hdda_active[4u];
groupshared uint s_hdda_tmin[128u];
groupshared uint64_t s_hdda_ijkl[128u];
groupshared uint s_hdda_min_tmin;
groupshared uint s_hdda_min_idx;

struct workgroup_hdda_t
{
    hdda_t hdda;
    bool is_active;
    bool needs_update;
    bool has_set_inactive;
    float max_t;
};

void workgroup_hdda_init(inout workgroup_hdda_t hdda, uint thread_idx, float3 rayOrigin, float3 rayDir, float3 rayDirInv, bool did_hit, float min_t, float max_t)
{
    hdda.is_active = did_hit;
    hdda.needs_update = true;
    hdda.has_set_inactive = false;
    hdda.max_t = max_t;
    if (hdda.is_active)
    {
        hdda_init(hdda.hdda, rayOrigin, rayDir, rayDirInv, min_t);
        hdda_update(hdda.hdda, rayOrigin, rayDir, rayDirInv, 1);
    }

    // initialize active mask
    if (thread_idx < 4u)
    {
        s_hdda_active[thread_idx] = ~0u;
    }
    GroupMemoryBarrierWithGroupSync();
}

bool workgroup_hdda_should_update(inout workgroup_hdda_t hdda, uint thread_idx)
{
    return hdda.needs_update;
}

void workgroup_hdda_update(inout workgroup_hdda_t hdda, uint thread_idx, float3 rayOrigin, float3 rayDir, float3 rayDirInv, int dim)
{
    hdda.needs_update = false;
    if (hdda.is_active)
    {
        hdda_update(hdda.hdda, rayOrigin, rayDir, rayDirInv, dim);

        int4 ijkl = int4(
            hdda.hdda.ijk.x & ~(dim - 1),
            hdda.hdda.ijk.y & ~(dim - 1),
            hdda.hdda.ijk.z & ~(dim - 1),
            dim
        );
        uint64_t ijkl_64 = (uint64_t(ijkl.x) << 48u) | (uint64_t(ijkl.y) << 32u) |
                            (uint64_t(ijkl.z) << 16u) | uint64_t(ijkl.w);


        s_hdda_ijkl[thread_idx] = ijkl_64;
        s_hdda_tmin[thread_idx] = hdda_sort_asuint(hdda.hdda.tmin);
    }
    else
    {
        s_hdda_tmin[thread_idx] = hdda_sort_asuint(1.f / 0.f);
    }
}

uint workgroup_hdda_vote(inout workgroup_hdda_t hdda, uint thread_idx)
{
    // vote for voxel val to update cooperatively
    if (thread_idx == 0u)
    {
        s_hdda_min_tmin = hdda_sort_asuint(1.f / 0.f);
        s_hdda_min_idx = ~0u;
    }
    GroupMemoryBarrierWithGroupSync();
    if (hdda.is_active)
    {
        InterlockedMin(s_hdda_min_tmin, s_hdda_tmin[thread_idx]);
    }
    GroupMemoryBarrierWithGroupSync();
    if (hdda.is_active && s_hdda_min_tmin == s_hdda_tmin[thread_idx])
    {
        InterlockedMin(s_hdda_min_idx, thread_idx);
    }
    GroupMemoryBarrierWithGroupSync();

    // if no result, mark done
    if (s_hdda_min_idx == ~0u)
    {
        hdda.is_active = false;
    }
    return s_hdda_min_idx;
}

bool workgroup_hdda_step(inout workgroup_hdda_t hdda, uint thread_idx, float3 rayOrigin, float3 rayDir, float3 rayDirInv)
{
    if (hdda.is_active)
    {
        // only step if target voxel was processed
        if (s_hdda_ijkl[s_hdda_min_idx] == s_hdda_ijkl[thread_idx])
        {
            hdda.needs_update = true;

            uint skip_level = voxelbvh_get_skip_level(s_voxel_val[s_hdda_min_idx]);
            // align HDDA tmin/tmax to current voxel size
            int dim = 1 << skip_level;
            hdda_step(hdda.hdda, rayOrigin, rayDir, rayDirInv, dim);
        }
    }

    if (hdda.hdda.tmin >= hdda.max_t)
    {
         hdda.is_active = false;
    }
    if (!hdda.is_active && !hdda.has_set_inactive)
    {
        hdda.has_set_inactive = true;
        InterlockedAnd(s_hdda_active[thread_idx >> 5u], ~(1u << (thread_idx & 31)));
    }
    GroupMemoryBarrierWithGroupSync();

    return s_hdda_active[0] != 0u || s_hdda_active[1] != 0u || s_hdda_active[2] != 0u || s_hdda_active[3] != 0u;
}

void workgroup_mark_done(inout workgroup_hdda_t hdda, uint thread_idx)
{
    hdda.is_active = false;
}

bool workgroup_this_thread_active(inout workgroup_hdda_t hdda, uint thread_idx)
{
    return s_hdda_ijkl[s_hdda_min_idx] == s_hdda_ijkl[thread_idx];
}
