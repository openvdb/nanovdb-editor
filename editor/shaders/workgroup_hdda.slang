// workgroup_hdda.slang

struct hdda_t
{
    int3 ijk;
    float tmin;
    float3 ijk_sub;
    float tmax;
};

uint hdda_sort_asuint(float f)
{
    uint f_uint = asuint(f);
    uint mask = -int(f_uint >> 31) | 0x80000000;
    return f_uint ^ mask;
}

void hdda_init(inout hdda_t hdda, float3 ray_origin, float3 ray_dir, float3 ray_dir_inv, float tmin)
{
    float3 ray_pos = ray_dir * tmin + ray_origin;

    hdda.ijk = int3(floor(ray_pos));
    hdda.ijk_sub = ray_pos - float3(hdda.ijk);
    hdda.tmin = tmin;
    hdda.tmax = tmin;
}

void hdda_update(inout hdda_t hdda, float3 ray_origin, float3 ray_dir, float3 ray_dir_inv, int dim)
{
    // based on dim, put everything in a local space
    int3 offset_to_local = hdda.ijk & ~(dim - 1);

    float3 box_min = float3(offset_to_local) - ray_origin;
    float3 box_max = float3(offset_to_local + int3(dim, dim, dim)) - ray_origin;
    intersect_box(ray_dir, ray_dir_inv, box_min, box_max, hdda.tmin, hdda.tmax);
}

void hdda_step(inout hdda_t hdda, float3 ray_origin, float3 ray_dir, float3 ray_dir_inv, int dim)
{
    // based on dim, put everything in a local space
    int3 offset_to_local = hdda.ijk & ~(dim - 1);

    float3 local_pos = float3(hdda.ijk - offset_to_local) + hdda.ijk_sub;
    float3 local_max = float3(
        (ray_dir.x > 0.f ? float(dim) : 0.f),
        (ray_dir.y > 0.f ? float(dim) : 0.f),
        (ray_dir.z > 0.f ? float(dim) : 0.f)
    );

    float3 hit_ts = float3(local_max - local_pos) * ray_dir_inv;
    float hit_t;
    int axis;
    if (ray_dir.x != 0.f && (hit_ts.x <= hit_ts.y || ray_dir.y == 0.f) && (hit_ts.x <= hit_ts.z || ray_dir.z == 0.f))
    {
        hit_t = hit_ts.x;
        axis = 0;
    }
    else if (ray_dir.y != 0.f && (hit_ts.y <= hit_ts.x || ray_dir.x == 0.f) && (hit_ts.y <= hit_ts.z || ray_dir.z == 0.f))
    {
        hit_t = hit_ts.y;
        axis = 1;
    }
    else
    {
        hit_t = hit_ts.z;
        axis = 2;
    }

    float3 local_pos_new = local_pos + hit_t * ray_dir;
    hdda.ijk = max(0, min(dim - 1, int3(floor(local_pos_new)))) + offset_to_local;
    hdda.ijk_sub = local_pos_new - float3(hdda.ijk - offset_to_local);
    if (axis == 0)
    {
        hdda.ijk.x = offset_to_local.x + (ray_dir.x > 0.f ? dim : -1);
        hdda.ijk_sub.x = ray_dir.x > 0.f ? 0.f : 1.f;
    }
    if (axis == 1)
    {
        hdda.ijk.y = offset_to_local.y + (ray_dir.y > 0.f ? dim : -1);
        hdda.ijk_sub.y = ray_dir.y > 0.f ? 0.f : 1.f;
    }
    if (axis == 2)
    {
        hdda.ijk.z = offset_to_local.z + (ray_dir.z > 0.f ? dim : -1);
        hdda.ijk_sub.z = ray_dir.z > 0.f ? 0.f : 1.f;
    }

    hdda.tmin = hdda.tmax;
}

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

            uint skip_level = (uint(s_voxel_val[s_hdda_min_idx]) >> 16u) & 0xFFFF;
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

#if 0
groupshared uint64_t s_voxel_val[128u];

void hdda_walk(uint thread_idx, float3 rayOrigin, float3 rayDir, float3 rayDirInv, bool did_hit, float min_t, float max_t)
{
    workgroup_hdda_t hdda = {};
    workgroup_hdda_init(hdda, thread_idx, rayOrigin, rayDir, rayDirInv, did_hit, min_t, max_t);
    bool should_run = true;
    while(should_run)
    {
        GroupMemoryBarrierWithGroupSync();

        // update HDDA state as needed
        if (workgroup_hdda_should_update(hdda, thread_idx))
        {
            int dim = 1;
            if (hdda.is_active)
            {
                pnanovdb_address_t addr = pnanovdb_readaccessor_get_value_address(grid_type, buf, acc, hdda.ijk);
                uint64_t val_raw = pnanovdb_read_uint64(buf, addr);
                uint skip_level = (uint(val_raw) >> 16u) & 0xFFFF;

                s_voxel_val[thread_idx] = val_raw;

                // align HDDA tmin/tmax to current voxel size
                dim = 1 << skip_level;
            }
            else
            {
                s_voxel_val[thread_idx] = ~uint64_t(0u);
            }
            workgroup_hdda_update(hdda, thread_idx, rayOrigin, rayDir, rayDirInv, dim);
        }

        uint current_hdda_idx = workgroup_hdda_vote(hdda, thread_idx);
        // cooperatively process min_voxel_val
        if (current_hdda_idx != ~0u)
        {
            if (s_voxel_val[current_hdda_idx] != ~uint64_t(0u))
            {

            }
        }
        if (current_hdda_idx == ~0u|| sum.a <= 0.0001f)
        {
            workgroup_mark_done(hdda, thread_idx);
        }
        should_run = workgroup_hdda_step(hdda, thread_idx, rayOrigin, rayDir, rayDirInv);
    };
}
#endif
