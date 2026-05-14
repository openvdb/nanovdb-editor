// workgroup_radix_sort.slang

groupshared uint smem[256u];
uint read_smem_idx(uint addr, uint idx)
{
    return smem[addr + idx];
}
void write_smem_idx(uint addr, uint idx, uint val)
{
    smem[addr + idx] = val;
}
void workgroup_scan(uint thread_idx, uint val, inout uint result, inout uint total_count)
{
    uint sdata0_addr = 0;
    uint sdata1_addr = 128u;

    uint local_val = val;
    write_smem_idx(sdata0_addr, thread_idx, local_val);

    GroupMemoryBarrierWithGroupSync();

    if (thread_idx >= 1)
        local_val += read_smem_idx(sdata0_addr, thread_idx - 1);
    if (thread_idx >= 2)
        local_val += read_smem_idx(sdata0_addr, thread_idx - 2);
    if (thread_idx >= 3)
        local_val += read_smem_idx(sdata0_addr, thread_idx - 3);
    write_smem_idx(sdata1_addr, thread_idx, local_val);

    GroupMemoryBarrierWithGroupSync();

    if (thread_idx >= 4)
        local_val += read_smem_idx(sdata1_addr, thread_idx - 4);
    if (thread_idx >= 8)
        local_val += read_smem_idx(sdata1_addr, thread_idx - 8);
    if (thread_idx >= 12)
        local_val += read_smem_idx(sdata1_addr, thread_idx - 12);
    write_smem_idx(sdata0_addr, thread_idx, local_val);

    GroupMemoryBarrierWithGroupSync();

    if (thread_idx >= 16)
        local_val += read_smem_idx(sdata0_addr, thread_idx - 16);
    if (thread_idx >= 32)
        local_val += read_smem_idx(sdata0_addr, thread_idx - 32);
    if (thread_idx >= 48)
        local_val += read_smem_idx(sdata0_addr, thread_idx - 48);
    write_smem_idx(sdata1_addr, thread_idx, local_val);

    GroupMemoryBarrierWithGroupSync();

    if (thread_idx >= 64)
        local_val += read_smem_idx(sdata1_addr, thread_idx - 64);

    result = local_val;

    // compute total_count
    total_count = read_smem_idx(sdata1_addr, 63) + read_smem_idx(sdata1_addr, 127);
}
void workgroup_scan_uint4(uint thread_idx, uint4 val, inout uint4 result, inout uint total_count)
{
    uint val1 = val.x + val.y + val.z + val.w;
    uint result1 = 0u;
    workgroup_scan(thread_idx, val1, result1, total_count);
    result.w = result1;
    result.z = result.w - val.w;
    result.y = result.z - val.z;
    result.x = result.y - val.y;
}
void split4(uint thread_idx, uint4 pred, out uint4 rank)
{
    uint count;
    uint4 scan;
    workgroup_scan_uint4(thread_idx, pred, scan, count);

    rank.x = pred.x != 0u ? scan.x - 1u : 4u * thread_idx + 0u - scan.x + count;
    rank.y = pred.y != 0u ? scan.y - 1u : 4u * thread_idx + 1u - scan.y + count;
    rank.z = pred.z != 0u ? scan.z - 1u : 4u * thread_idx + 2u - scan.z + count;
    rank.w = pred.w != 0u ? scan.w - 1u : 4u * thread_idx + 3u - scan.w + count;
}
void radix_sort(uint thread_idx)
{
    uint4 key = uint4(
        s_prim_keys[4u * thread_idx + 0u],
        s_prim_keys[4u * thread_idx + 1u],
        s_prim_keys[4u * thread_idx + 2u],
        s_prim_keys[4u * thread_idx + 3u]);

    GroupMemoryBarrierWithGroupSync();

    for (uint pass_idx = 0u; pass_idx < 4u; pass_idx++)
    {
        uint4 pred = uint4(
            ((key.x >> pass_idx) & 1u) ^ 1u,
            ((key.y >> pass_idx) & 1u) ^ 1u,
            ((key.z >> pass_idx) & 1u) ^ 1u,
            ((key.w >> pass_idx) & 1u) ^ 1u
        );

        uint4 rank;
        split4(thread_idx, pred, rank);

        s_prim_keys[rank.x] = key.x;
        s_prim_keys[rank.y] = key.y;
        s_prim_keys[rank.z] = key.z;
        s_prim_keys[rank.w] = key.w;

        GroupMemoryBarrierWithGroupSync();

        key = uint4(
            s_prim_keys[4u * thread_idx + 0u],
            s_prim_keys[4u * thread_idx + 1u],
            s_prim_keys[4u * thread_idx + 2u],
            s_prim_keys[4u * thread_idx + 3u]);
    }
}
