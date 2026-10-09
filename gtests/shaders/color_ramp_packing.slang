// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

struct shader_params_t
{
    float4 cold;
    float4 hot;
    float2 positions;
    uint count;
};

StructuredBuffer<uint> data_in;
ConstantBuffer<shader_params_t> constants;
RWStructuredBuffer<uint> data_out;

[shader("compute")][numthreads(1, 1, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    data_out[0] = constants.count;
    data_out[1] = asuint(constants.positions.x);
    data_out[2] = asuint(constants.positions.y);
    for (uint i = 0; i < 4; ++i)
    {
        data_out[3 + i] = asuint(constants.cold[i]);
        data_out[7 + i] = asuint(constants.hot[i]);
    }
}
