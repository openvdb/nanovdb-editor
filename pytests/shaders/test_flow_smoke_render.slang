// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

#define PNANOVDB_HLSL
#define PNANOVDB_ADDRESS_64
#define PNANOVDB_BUF_HLSL_64
#include "PNanoVDB.h"
#include "flow_smoke_material.slang"

StructuredBuffer<uint2> buf;
ConstantBuffer<shader_params_t> shader_params;
RWStructuredBuffer<float4> data_out;

#include "flow_smoke_ray_march.slang"

[shader("compute")][numthreads(1, 1, 1)]
void main(uint3 thread_id : SV_DispatchThreadID)
{
    float3 origin = float3(1.f + 0.5f * float(thread_id.x), 1.f + 0.5f * float(thread_id.y), -1.f);
    data_out[thread_id.y * 4u + thread_id.x] = flow_ray_march(origin, float3(0.f, 0.f, 1.f));
}
