// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

#include "flow_smoke_material.slang"

StructuredBuffer<float4> data_in;
ConstantBuffer<shader_params_t> constants;
RWStructuredBuffer<float4> data_out;

void computeMain(uint3 thread_id : SV_DispatchThreadID)
{
    float4 input_value = data_in[thread_id.x];
    float4 color = flow_smoke_sample(constants, input_value.x, input_value.y, input_value.z);
    float4 sum = float4(0.f, 0.f, 0.f, 1.f);
    for (uint index = 0; index < uint(input_value.w); index++) flow_accumulate(color, sum);
    data_out[thread_id.x] = sum;
}

void computeShadowMain(uint3 thread_id : SV_DispatchThreadID)
{
    float4 input_value = data_in[thread_id.x];
    float opacity = flow_opacity(input_value.x, input_value.y, constants.shadow_attenuation, input_value.z);
    float transmittance = 1.f;
    for (uint index = 0; index < uint(input_value.w); index++) transmittance *= 1.f - opacity;
    float intensity = flow_shadow_light(constants, transmittance);
    data_out[thread_id.x] = float4(intensity, intensity, intensity, transmittance);
}

void computeRayStepsMain(uint3 thread_id : SV_DispatchThreadID)
{
    float4 input_value = data_in[thread_id.x];
    flow_ray_steps_t steps = flow_ray_steps(input_value.x, input_value.y, input_value.z, input_value.w);
    float last_offset = -1.f;
    uint advanced = 0u;
    for (uint index = 0u; index < steps.count; index++)
    {
        float offset = steps.first_offset + float(index) * steps.step_size;
        if (offset > last_offset) advanced++;
        last_offset = offset;
    }
    data_out[thread_id.x] = float4(float(steps.count), float(advanced), steps.step_size, last_offset);
}

void computeShadowCountMain(uint3 thread_id : SV_DispatchThreadID)
{
    uint count = flow_shadow_step_count(constants.shadow_num_steps);
    float transmittance = 1.f;
    for (uint index = 0u; index < count; index++) transmittance *= 0.99f;
    data_out[thread_id.x] = float4(float(count), transmittance, 0.f, 0.f);
}

void computeColormapTexelMain(uint3 thread_id : SV_DispatchThreadID)
{
    data_out[thread_id.x] = flow_quantize_colormap_texel(data_in[thread_id.x]);
}
