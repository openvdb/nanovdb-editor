// gaussian_count_tiles.slang

#include "raster2d_common.slang"

ConstantBuffer<constants_t> constants;
ConstantBuffer<shader_params_t> shader_params;

StructuredBuffer<float2> means2d_in;
StructuredBuffer<int> radii_in;

RWStructuredBuffer<uint> num_tiles_per_gaussian_out;

[shader("compute")][numthreads(256, 1)]
void main(uint3 group_idx : SV_GroupID, uint3 thread_idx : SV_GroupThreadID)
{
    uint idx = group_idx.x * 256u + thread_idx.x;

    if (idx >= constants.prim_count)
    {
        return;
    }

    int radius = radii_in[idx];
    if (radius <= 0)
    {
        num_tiles_per_gaussian_out[idx] = 0u;
        return;
    }
    float radiusf = float(radius);

    float2 mean2d = means2d_in[idx];
    float tile_radius = radiusf / float(shader_params.tile_size);
    float tile_mean_u = mean2d.x / float(shader_params.tile_size);
    float tile_mean_v = mean2d.y / float(shader_params.tile_size);

    // tile_min is inclusive, tile_max is exclusive
    uint2 tile_min, tile_max;
    tile_min.x = min(max(0, (uint32_t)floor(tile_mean_u - tile_radius)), constants.num_tiles_w);
    tile_min.y = min(max(0, (uint32_t)floor(tile_mean_v - tile_radius)), constants.num_tiles_h);
    tile_max.x = min(max(0, (uint32_t)ceil(tile_mean_u + tile_radius)), constants.num_tiles_w);
    tile_max.y = min(max(0, (uint32_t)ceil(tile_mean_v + tile_radius)), constants.num_tiles_h);

    uint num_tiles = (tile_max.y - tile_min.y) * (tile_max.x - tile_min.x);

    num_tiles_per_gaussian_out[idx] = num_tiles;
}
