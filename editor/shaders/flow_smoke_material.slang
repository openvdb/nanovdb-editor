// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

#ifndef FLOW_SMOKE_MATERIAL_SLANG
#define FLOW_SMOKE_MATERIAL_SLANG

struct shader_params_t
{
    float attenuation;
    float step_size_scale;
    float color_scale;
    float fallback_temperature;

    float colormap_min;
    float colormap_max;
    uint colormap_resolution;
    uint point_count;

    float4 point_positions0;
    float4 point_positions1;
    float4 point_color0;
    float4 point_color1;
    float4 point_color2;
    float4 point_color3;
    float4 point_color4;
    float4 point_color5;
    float4 point_color6;
    float4 point_color7;

    float3 shadow_direction;
    float shadow_factor;
    float shadow_attenuation;
    float shadow_step_size_scale;
    float shadow_step_offset_scale;
    float shadow_min_intensity;
    uint shadow_num_steps;
    float shadow_cell_size_scale;
    float2 shadow_colormap_range;
};

static const uint FLOW_MAX_RAY_STEPS = 4096u;
static const uint FLOW_MAX_SHADOW_STEPS = 128u;

struct flow_ray_steps_t
{
    float first_offset;
    float step_size;
    uint count;
};

flow_ray_steps_t flow_ray_steps(float near_t, float far_t, float requested_step, float noise)
{
    flow_ray_steps_t result = { 0.f, 0.f, 0u };
    float distance = far_t - near_t;
    if (!all(isfinite(float4(near_t, far_t, requested_step, noise))) ||
        !isfinite(distance) || !(distance > 0.f) || !(requested_step > 0.f)) return result;

    // Coarser samples cover the full ray when its requested sample count exceeds the limit.
    result.step_size = max(requested_step, distance / float(FLOW_MAX_RAY_STEPS));
    float phase = frac(fmod(near_t, result.step_size) / result.step_size + noise);
    result.first_offset = phase == 0.f ? 0.f : (1.f - phase) * result.step_size;
    result.count = uint(clamp(ceil((distance - result.first_offset) / result.step_size),
        0.f, float(FLOW_MAX_RAY_STEPS)));
    return result;
}

uint flow_shadow_step_count(uint requested)
{
    return min(requested, FLOW_MAX_SHADOW_STEPS);
}

float4 flow_colormap_point(shader_params_t params, uint index)
{
    switch (index)
    {
    case 0: return params.point_color0;
    case 1: return params.point_color1;
    case 2: return params.point_color2;
    case 3: return params.point_color3;
    case 4: return params.point_color4;
    case 5: return params.point_color5;
    case 6: return params.point_color6;
    default: return params.point_color7;
    }
}

float4 flow_colormap_texel(shader_params_t params, float u)
{
    float low_position = -1e30f;
    float high_position = 1e30f;
    float4 low_color = float4(0.f);
    float4 high_color = float4(0.f);
    for (uint index = 0; index < min(params.point_count, 8u); index++)
    {
        float position = index < 4u ? params.point_positions0[index] : params.point_positions1[index - 4u];
        float4 color = flow_colormap_point(params, index);
        if (position <= u && position >= low_position)
        {
            low_position = position;
            low_color = color;
        }
        if (position >= u && position <= high_position)
        {
            high_position = position;
            high_color = color;
        }
    }
    if (low_position == -1e30f) return high_color;
    if (high_position == 1e30f || low_position == high_position) return low_color;
    return lerp(low_color, high_color, (u - low_position) / (high_position - low_position));
}

float4 flow_sample_colormap(shader_params_t params, float temperature)
{
    float width = params.colormap_max - params.colormap_min;
    float u = width > 0.f ? saturate((temperature - params.colormap_min) / width) : 0.f;
    float resolution = float(max(params.colormap_resolution, 1u));
    float texel = clamp(u * resolution - 0.5f, 0.f, resolution - 1.f);
    float lower = floor(texel);
    float upper = min(lower + 1.f, resolution - 1.f);
    float4 a = flow_colormap_texel(params, (lower + 0.5f) / resolution);
    float4 b = flow_colormap_texel(params, (upper + 0.5f) / resolution);
    a = f16tof32(f32tof16(a));
    b = f16tof32(f32tof16(b));
    return lerp(a, b, texel - lower);
}

float flow_opacity(float colormap_alpha, float smoke, float attenuation, float world_step)
{
    // Flow clamps density before applying the step-dependent opacity.
    return saturate(colormap_alpha * smoke) * (1.f - exp(-max(attenuation, 0.f) * world_step));
}

float flow_shadow_light(shader_params_t params, float transmittance)
{
    float intensity = lerp(transmittance, 1.f, saturate(params.shadow_min_intensity));
    return max(0.f, 1.f + params.shadow_factor * (intensity - 1.f));
}

float4 flow_smoke_sample(shader_params_t params, float temperature, float smoke, float world_step)
{
    float4 color = flow_sample_colormap(params, temperature);
    color.rgb *= params.color_scale;
    color.rgb = max(color.rgb, float3(0.f));
    color.a = flow_opacity(color.a, smoke, params.attenuation, world_step);
    return color;
}

void flow_accumulate(float4 color, inout float4 sum)
{
    sum.rgb += sum.a * color.a * color.rgb;
    sum.a *= 1.f - color.a;
}

#endif
