// gaussian_rasterize_2d.slang

#include "raster2d_common.slang"

ConstantBuffer<constants_t> constants;
ConstantBuffer<shader_params_t> shader_params;

StructuredBuffer<float2> means2d_in;
StructuredBuffer<float> depth_in;
StructuredBuffer<float> conics_in;
StructuredBuffer<float> colors_in;
StructuredBuffer<float> opacities_in;
StructuredBuffer<int> tile_offsets_in;
StructuredBuffer<int> tile_gaussians_ids_in;

RWTexture2D<float4> color_2d_out;
RWTexture2D<float> depth_out;

// RWStructuredBuffer<float3> render_colors_out;
// RWStructuredBuffer<float> render_alphas_out;
// RWStructuredBuffer<int> last_ids_out;

groupshared uint smem[8u * 256u];
groupshared uint smem_depth[256u];
groupshared uint smem_should_run;

void volume_render_tile(uint3 group_idx,
                        uint3 thread_idx,
                        uint pix_id,
                        uint tile_start,
                        uint tile_end,
                        uint block_size,
                        uint tile_size,
                        bool write_pixel,
                        uint i,
                        uint j)
{
    uint num_batches = (tile_end - tile_start + block_size - 1u) / block_size;

    uint tidx = thread_idx.x * 16 + thread_idx.y;

    bool done = !write_pixel;

    float px = float(j) + 0.5f;
    float py = float(i) + 0.5f;

    float accum_transmittance = 1.f;
    float accum_depth = 0.f;
    uint cur_idx = 0u;

    float3 pix_out = float3(0.f, 0.f, 0.f);
    for (uint b = 0u; b < num_batches; b++)
    {
        if (thread_idx.x == 0 && thread_idx.y == 0)
        {
            smem_should_run = 0u;
        }
        GroupMemoryBarrierWithGroupSync();
        if (!done)
        {
            smem_should_run = 1u;
        }
        GroupMemoryBarrierWithGroupSync();
        if (smem_should_run == 0u)
        {
            break;
        }

        uint batch_start = tile_start + block_size * b;
        uint idx = batch_start + tidx;
        if (idx < tile_end)
        {
            int g = tile_gaussians_ids_in[idx];
            float2 xy = means2d_in[g];
            float opac = opacities_in[g];
            float3 conic = float3(conics_in[3u * g + 0u], conics_in[3u * g + 1u], conics_in[3u * g + 2u]);
            float depth = depth_in[g];

            smem[8u * tidx + 0] = uint(g);
            smem[8u * tidx + 1] = asuint(xy.x);
            smem[8u * tidx + 2] = asuint(xy.y);
            smem[8u * tidx + 3] = asuint(opac);
            smem[8u * tidx + 4] = asuint(conic.x);
            smem[8u * tidx + 5] = asuint(conic.y);
            smem[8u * tidx + 6] = asuint(conic.z);

            smem_depth[tidx] = asuint(depth);
        }

        GroupMemoryBarrierWithGroupSync();

        uint batch_size = min(block_size, tile_end - batch_start);
        for (uint t = 0u; (t < batch_size) && !done; t++)
        {
            int g = int(smem[8u * t + 0]);

            if (g >= constants.prim_count)
            {
                continue;
            }

            float2 xy = float2(asfloat(smem[8u * t + 1]), asfloat(smem[8u * t + 2]));
            float opac = asfloat(smem[8u * t + 3]);
            float3 conic = float3(asfloat(smem[8u * t + 4]), asfloat(smem[8u * t + 5]), asfloat(smem[8u * t + 6]));

            float2 delta = float2(xy.x - px, xy.y - py);
            float sigma =
                0.5f * (conic.x * delta.x * delta.x + conic.z * delta.y * delta.y) + conic.y * delta.x * delta.y;
            float alpha = min(0.999f, opac * exp(-sigma));

            if (sigma < 0.f || alpha < 1.f / 255.f)
            {
                continue;
            }

            float next_transmittance = accum_transmittance * (1.f - alpha);
            if (next_transmittance <= 1e-4f) // this pixel is done exclusive
            {
                done = true;
                break;
            }

            float depth = asfloat(smem_depth[t]);

            float vis = alpha * accum_transmittance;
            float3 color = float3(colors_in[3u * g + 0u], colors_in[3u * g + 1u], colors_in[3u * g + 2u]);
            pix_out += color * vis;
            accum_depth += depth * vis;

            cur_idx = batch_start + t;
            accum_transmittance = next_transmittance;
        }
    }

    if (write_pixel)
    {
        float3 background = float3(0.f, 0.f, 0.f);
        float final_depth = 0.f;
        if (accum_transmittance < 1.f)
        {
            final_depth = accum_depth / (1.f - accum_transmittance);
            // go to standard depth buffer space
            float4 ndc = mul(float4(0.f, 0.f, final_depth, 1.f), constants.projection);
            if (ndc.w != 0.f)
            {
                ndc.z *= (1.f / ndc.w);
            }
            final_depth = ndc.z;
        }

        color_2d_out[int2(j, constants.image_height - 1u - i)] =
            float4(pix_out + accum_transmittance * background, 1.f - accum_transmittance);
        depth_out[int2(j, constants.image_height - 1u - i)] = final_depth;
        // render_alphas_out[pix_id] = 1.f - accum_transmittance;
        // render_colors_out[pix_id] = pix_out + accum_transmittance * background;
        // last_ids_out[pix_id] = int(cur_idx);
    }
}

[shader("compute")][numthreads(16, 16, 1)]
void main(uint3 group_idx : SV_GroupID, uint3 thread_idx : SV_GroupThreadID)
{
    int tile_id =
        (group_idx.x + constants.tile_origin_h) * constants.tile_width + (group_idx.y + constants.tile_origin_w);

    uint i = group_idx.x * shader_params.tile_size + thread_idx.x;
    uint j = group_idx.y * shader_params.tile_size + thread_idx.y;
    int pix_id = i * constants.image_width + j;

    bool pixel_in_image = (i < constants.image_height && j < constants.image_width);

    int range_start = tile_offsets_in[tile_id];
    int range_end = (tile_id == constants.tile_width * constants.tile_height - 1u) ? constants.n_isects :
                                                                                     tile_offsets_in[tile_id + 1];

    const uint block_size = 16 * 16;

    uint global_i = i + constants.image_origin_h;
    uint global_j = j + constants.image_origin_w;

    volume_render_tile(group_idx, thread_idx, pix_id, range_start, range_end, block_size, shader_params.tile_size,
                       pixel_in_image, global_i, global_j);
}
