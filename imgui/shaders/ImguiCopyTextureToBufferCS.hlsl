// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   ImguiCopyTextureToBufferCS.hlsl

    \author Andrew Reidmeyer

    \brief  This file is part of the PNanoVDB Compute Vulkan reference implementation.
*/
// Copyright (c) 2014-2022 NVIDIA Corporation. All rights reserved.

struct constants_t
{
    uint width;
    uint height;
};

ConstantBuffer<constants_t> constants;

Texture2D<float4> colorIn;

RWStructuredBuffer<uint> colorOut;

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    if (dispatchThreadID.x >= constants.width || dispatchThreadID.y >= constants.height)
    {
        return;
    }
    int2 tidx = int2(dispatchThreadID.xy);

    float4 color = colorIn[tidx];

    color = max(0.f, min(1.f, color));
    color *= 255.f;

    uint color_rgba = int(color.r) | (int(color.g) << 8u) | (int(color.b) << 16u) | (int(color.a) << 24u);

    colorOut[tidx.y * constants.width + tidx.x] = color_rgba;
}
