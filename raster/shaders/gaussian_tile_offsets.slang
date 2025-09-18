// gaussian_tile_offsets.slang

#include "raster2d_common.slang"

ConstantBuffer<constants_t> constants;
ConstantBuffer<shader_params_t> shader_params;

StructuredBuffer<uint> sorted_intersection_keys_high_in;

RWStructuredBuffer<uint> offsets_out;

[shader("compute")][numthreads(256, 1)] void main(uint3 group_idx
                                                  : SV_GroupID, uint3 thread_idx
                                                  : SV_GroupThreadID)
{
    uint idx = group_idx.x * 256u + thread_idx.x;

    if (idx >= constants.n_isects)
    {
        return;
    }

    uint tile_start_idx = sorted_intersection_keys_high_in[idx];

    if (idx == 0)
    {
        // The first tile in the first camera writes out 0 as the offset
        // until the first valid tile (inclusive). i.e. tiles before this one
        // have no intersections, so their offset range is [0, 0]
        for (uint i = 0u; i < tile_start_idx + 1u; i++)
        {
            offsets_out[i] = 0;
        }
    }
    if (idx == constants.n_isects - 1u)
    {
        for (uint i = tile_start_idx + 1; i < constants.num_tiles; i++)
        {
            offsets_out[i] = constants.n_isects;
        }
    }

    if (idx > 0)
    {
        uint prev_tile_start_idx = sorted_intersection_keys_high_in[idx - 1];

        if (prev_tile_start_idx == tile_start_idx)
        {
            return;
        }

        for (uint i = prev_tile_start_idx + 1; i < tile_start_idx + 1u; i++)
        {
            offsets_out[i] = idx;
        }
    }
}
