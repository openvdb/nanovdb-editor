// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   ImguiCopyTextureCS.hlsl

    \author Andrew Reidmeyer

    \brief  This file is part of the PNanoVDB Compute Vulkan reference implementation.
*/
// Copyright (c) 2014-2022 NVIDIA Corporation. All rights reserved.

Texture2D<float4> colorIn;

RWTexture2D<float4> plane0Out;
RWTexture2D<float4> plane1Out;
RWTexture2D<float4> plane2Out;

float3 rgb_to_yuv(float3 rgb)
{
    float4 color = float4(rgb, 1.f);

    float y = dot(float4(0.299f, 0.587f, 0.114f, 0.f), color);
    float u = dot(float4(-0.169f, -0.331f, 0.5f, 0.5f), color);
    float v = dot(float4(0.5f, -0.419f, -0.081f, 0.5f), color);

    return min(max(float3(0.f, 0.f, 0.f), float3(y, u, v)), float3(1.f, 1.f, 1.f));
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    int2 tidx = int2(dispatchThreadID.xy);

    float3 yuv = rgb_to_yuv(colorIn[tidx].xyz);

    plane0Out[tidx] = float4(yuv.x, yuv.x, yuv.x, yuv.x);
    if ((tidx.x & 1) == 0 &&
        (tidx.y & 1) == 0)
    {
        float3 yuv10 = rgb_to_yuv(colorIn[tidx + int2(1, 0)].xyz);
        float3 yuv01 = rgb_to_yuv(colorIn[tidx + int2(0, 1)].xyz);
        float3 yuv11 = rgb_to_yuv(colorIn[tidx + int2(1, 1)].xyz);

        yuv = 0.25f * (yuv + yuv10 + yuv01 + yuv11);
        plane1Out[tidx / 2] = float4(yuv.y, yuv.y, yuv.y, yuv.y);
        plane2Out[tidx / 2] = float4(yuv.z, yuv.z, yuv.z, yuv.z);
    }
}
