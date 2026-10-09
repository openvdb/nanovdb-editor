// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

#include "editor_params.slang"

struct shader_params_t
{
    float4 cold;
    float4 hot;
    float4 third;
    float4 fourth;
    float4 positions;
    uint count;
};

StructuredBuffer<uint2> buf;
RWStructuredBuffer<uint> image_out_deprecated;
RWTexture2D<float4> texture_out;
ConstantBuffer<EditorParams> editor_params;
ConstantBuffer<shader_params_t> shader_params;

float4 sample_ramp(float value)
{
    float4 colors[4] = { shader_params.cold, shader_params.hot, shader_params.third, shader_params.fourth };
    uint count = clamp(shader_params.count, 1u, 4u);
    uint low = 0u;
    uint high = 0u;
    bool has_low = false;
    bool has_high = false;
    for (uint i = 0u; i < count; ++i)
    {
        float position = shader_params.positions[i];
        // Later slots win at equal positions, as in the widget preview.
        if (position <= value && (!has_low || position >= shader_params.positions[low]))
        {
            low = i;
            has_low = true;
        }
        if (position >= value && (!has_high || position <= shader_params.positions[high]))
        {
            high = i;
            has_high = true;
        }
    }
    if (!has_low)
    {
        return colors[high];
    }
    if (!has_high || shader_params.positions[low] == shader_params.positions[high])
    {
        return colors[low];
    }
    float weight = (value - shader_params.positions[low]) /
                   (shader_params.positions[high] - shader_params.positions[low]);
    return lerp(colors[low], colors[high], weight);
}

[shader("compute")][numthreads(16, 8, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint2 pixel = dispatchThreadID.xy;
    if (pixel.x >= editor_params.width || pixel.y >= editor_params.height)
    {
        return;
    }
    float2 size = float2(editor_params.width, editor_params.height);
    float2 uv = (float2(pixel) + 0.5f) / size;
    float2 radial = (uv - float2(0.5f, 0.75f)) * size / (0.22f * size.y);
    float value = uv.y < 0.5f ? uv.x : saturate(1.f - length(radial));
    float4 color = sample_ramp(value);
    uint2 tile = pixel / 24u;
    float checker = ((tile.x + tile.y) & 1u) != 0u ? 0.18f : 0.08f;
    float alpha = saturate(color.a);
    float3 preview = color.rgb * alpha + checker * (1.f - alpha);
    if (abs(uv.y - 0.5f) < 2.f / size.y)
    {
        preview = float3(0.03f);
    }
    // The editor stores remaining transmittance in alpha; this preview is opaque.
    texture_out[pixel] = float4(preview, 0.f);
}
