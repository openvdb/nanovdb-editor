// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0
// Portions adapted from NVIDIA NvFlow, Copyright (c) 2014-2026 NVIDIA CORPORATION & AFFILIATES.

#define PNANOVDB_HLSL
#define PNANOVDB_ADDRESS_64
#define PNANOVDB_BUF_HLSL_64
#include "PNanoVDB.h"
#include "editor_params.slang"
#include "flow_smoke_material.slang"

StructuredBuffer<uint2> buf;
RWStructuredBuffer<uint> image_out_deprecated;
RWTexture2D<float4> texture_out;
ConstantBuffer<EditorParams> editor_params;
ConstantBuffer<shader_params_t> shader_params;

#include "flow_smoke_ray_march.slang"

[shader("compute")][numthreads(16, 8, 1)]
void main(uint3 dispatch_thread_id : SV_DispatchThreadID)
{
    uint2 pixel = dispatch_thread_id.xy;
    if (pixel.x >= editor_params.width || pixel.y >= editor_params.height) return;

    float2 ndc = float2(2.f, -2.f) * ((float2(pixel) + 0.5f) /
        float2(editor_params.width, editor_params.height)) + float2(-1.f, 1.f);
    float4 point0 = mul(float4(ndc, 0.f, 1.f), editor_params.projection_inv);
    float4 point1 = mul(float4(ndc, 1.f, 1.f), editor_params.projection_inv);
    bool reverse_z = abs(point0.z / point0.w) > abs(point1.z / point1.w);
    float4 near_point = reverse_z ? point1 : point0;
    float4 far_point = near_point + mul(float4(0.f, 0.f, 1.f, 0.f), editor_params.projection_inv);
    float3 direction = normalize(far_point.xyz / far_point.w - near_point.xyz / near_point.w);
    if (reverse_z) direction = -direction;
    direction = normalize(mul(float4(direction, 0.f), editor_params.view_inv).xyz);
    float4 origin = mul(near_point, editor_params.view_inv);

    // The editor composites RGB with alpha interpreted as transmittance.
    texture_out[pixel] = flow_ray_march(origin.xyz / origin.w, direction);
}
