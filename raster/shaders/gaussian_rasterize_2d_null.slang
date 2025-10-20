// gaussian_rasterize_2d.slang

#include "raster2d_common.slang"

ConstantBuffer<constants_t> constants;
ConstantBuffer<shader_params_t> shader_params;

RWTexture2D<float4> color_2d_out;
RWTexture2D<float> depth_out;

[shader("compute")][numthreads(16, 16, 1)]
void main(uint3 group_idx : SV_GroupID, uint3 thread_idx : SV_GroupThreadID)
{
    int tile_id =
        (group_idx.x + constants.tile_origin_h) * constants.tile_width + (group_idx.y + constants.tile_origin_w);

    uint i = group_idx.x * shader_params.tile_size + thread_idx.x;
    uint j = group_idx.y * shader_params.tile_size + thread_idx.y;
    int pix_id = i * constants.image_width + j;

    bool pixel_in_image = (i < constants.image_height && j < constants.image_width);

    const uint block_size = 16 * 16;

    uint global_i = i + constants.image_origin_h;
    uint global_j = j + constants.image_origin_w;

    if (pixel_in_image)
    {
        float3 background = float3(0.f, 0.f, 0.f);
        color_2d_out[int2(global_j, constants.image_height - 1u - global_i)] = float4(background, 0.f);
        depth_out[int2(global_j, constants.image_height - 1u - global_i)] = 0.f;
    }
}
