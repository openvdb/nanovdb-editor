// voxelbvh_lines_to_ijkl.slang

struct constants_t
{
    uint line_count;
    uint workgroup_count;
    uint voxel_count;
    uint voxel_workgroup_count;
    uint integer_space_max;
};

ConstantBuffer<constants_t> constants;

StructuredBuffer<uint2> indices_in;
StructuredBuffer<float> positions_in;

StructuredBuffer<float> bbox_in;

RWStructuredBuffer<uint64_t> keys_out;
RWStructuredBuffer<uint> bbox_ids_out;

[shader("compute")][numthreads(256, 1, 1)]
void main(uint3 group_idx : SV_GroupID, uint3 thread_idx : SV_GroupThreadID)
{
    uint idx = group_idx.x * 256u + thread_idx.x;

    if (idx >= constants.line_count)
    {
        return;
    }

    float3 g_aabb_min = float3(bbox_in[0], bbox_in[1], bbox_in[2]);
    float3 g_aabb_max = float3(bbox_in[3], bbox_in[4], bbox_in[5]);
    float3 g_aabb_delta = g_aabb_max - g_aabb_min;
    float g_aabb_delta_max = max(g_aabb_delta.x, max(g_aabb_delta.y, g_aabb_delta.z));
    float scale = float(constants.integer_space_max) / (g_aabb_delta_max);
    float3 offset = -g_aabb_min * scale;

    // line
    uint2 index = indices_in[idx];

    float3 pos_a = float3(
        positions_in[3u * index.x + 0u],
        positions_in[3u * index.x + 1u],
        positions_in[3u * index.x + 2u]);
    float3 pos_b = float3(
        positions_in[3u * index.y + 0u],
        positions_in[3u * index.y + 1u],
        positions_in[3u * index.y + 2u]);

    float3 aabb_min = float3(1.f / 0.f, 1.f / 0.f, 1.f / 0.f);
    float3 aabb_max = float3(-1.f / 0.f, -1.f / 0.f, -1.f / 0.f);
    aabb_min = min(aabb_min, pos_a);
    aabb_max = max(aabb_max, pos_a);
    aabb_min = min(aabb_min, pos_b);
    aabb_max = max(aabb_max, pos_b);

    // transform world space to voxel space
    int3 ijk_min = int3(floor(scale * aabb_min + offset));
    int3 ijk_max = int3(floor(scale * aabb_max + offset));

    const uint level_step = 1u;

    uint level = 11u;
    uint level_last_good = level;
    while (level < 12u)
    {
        int3 ijk_min_level = ijk_min & ~((1 << level) - 1);
        int3 ijk_max_level = ijk_max & ~((1 << level) - 1);
        uint overlap_count = 0u;
        for (int i = ijk_min_level.x; i <= ijk_max_level.x; i += (1u << level))
        {
            for (int j = ijk_min_level.y; j <= ijk_max_level.y; j += (1u << level))
            {
                for (int k = ijk_min_level.z; k <= ijk_max_level.z; k += (1u << level))
                {
                    int3 ijk = int3(i,j,k);
                    bool is_overlap = true;
                    if (is_overlap)
                    {
                        overlap_count++;
                    }
                }
            }
        }
        if (overlap_count > 8u)
        {
            level = level_last_good;
            break;
        }
        if (level == 0u)
        {
            break;
        }
        level_last_good = level;
        level -= level_step;
        if (level >= 12u)
        {
            level = 0u;
        }
    }

    uint write_subidx = 0u;
    if (level < 12u)
    {
        int3 ijk_min_level = ijk_min & ~((1 << level) - 1);
        int3 ijk_max_level = ijk_max & ~((1 << level) - 1);
        for (int i = ijk_min_level.x; i <= ijk_max_level.x; i += (1u << level))
        {
            for (int j = ijk_min_level.y; j <= ijk_max_level.y; j += (1u << level))
            {
                for (int k = ijk_min_level.z; k <= ijk_max_level.z; k += (1u << level))
                {
                    int3 ijk = int3(i,j,k);
                    bool is_overlap = true;
                    if (is_overlap && write_subidx < 8u)
                    {
                        uint64_t key = (uint64_t(i) << 48u) | (uint64_t(j) << 32u) |
                            (uint64_t(k) << 16u) | uint64_t(level);
                        keys_out[8u * idx + write_subidx] = key;
                        bbox_ids_out[8u * idx + write_subidx] = idx;
                        write_subidx++;
                    }
                }
            }
        }
    }

    // set unused to last key and invalid id
    for (; write_subidx < 8u; write_subidx++)
    {
        keys_out[8u * idx + write_subidx] = ~uint64_t(0u);
        bbox_ids_out[8u * idx + write_subidx] = ~0u;
    }
}
