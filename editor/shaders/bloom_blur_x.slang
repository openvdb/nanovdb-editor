// bloom_blur_x.slang

struct constants_t
{
    uint mode;
    float weight;
    float threshold;
    float size;
    float4 texDimInv;
    float4 centerWeight;
    float4 weights[4u];
    float4 colorScale;
};

ConstantBuffer<constants_t> constants;

Texture2D<float4> tex_in;
RWTexture2D<float4> tex_out;

groupshared float4 sdata[16][48];

[shader("compute")][numthreads(16, 16, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID, uint3 thread_idx : SV_GroupThreadID)
{
    int2 idx = int2(dispatchThreadID.xy);

    sdata[thread_idx.y][thread_idx.x +  0] = tex_in[idx + int2(-16, 0)];
    sdata[thread_idx.y][thread_idx.x + 16] = tex_in[idx + int2(  0, 0)];
    sdata[thread_idx.y][thread_idx.x + 32] = tex_in[idx + int2(+16, 0)];

    GroupMemoryBarrierWithGroupSync();

    float4 accum = 0.f.xxxx;

    accum += constants.weights[3u].w * sdata[thread_idx.y][thread_idx.x + 16 - 16];
    accum += constants.weights[3u].z * sdata[thread_idx.y][thread_idx.x + 16 - 15];
    accum += constants.weights[3u].y * sdata[thread_idx.y][thread_idx.x + 16 - 14];
    accum += constants.weights[3u].x * sdata[thread_idx.y][thread_idx.x + 16 - 13];

    accum += constants.weights[2u].w * sdata[thread_idx.y][thread_idx.x + 16 - 12];
    accum += constants.weights[2u].z * sdata[thread_idx.y][thread_idx.x + 16 - 11];
    accum += constants.weights[2u].y * sdata[thread_idx.y][thread_idx.x + 16 - 10];
    accum += constants.weights[2u].x * sdata[thread_idx.y][thread_idx.x + 16 - 9];

    accum += constants.weights[1u].w * sdata[thread_idx.y][thread_idx.x + 16 - 8];
    accum += constants.weights[1u].z * sdata[thread_idx.y][thread_idx.x + 16 - 7];
    accum += constants.weights[1u].y * sdata[thread_idx.y][thread_idx.x + 16 - 6];
    accum += constants.weights[1u].x * sdata[thread_idx.y][thread_idx.x + 16 - 5];

    accum += constants.weights[0u].w * sdata[thread_idx.y][thread_idx.x + 16 - 4];
    accum += constants.weights[0u].z * sdata[thread_idx.y][thread_idx.x + 16 - 3];
    accum += constants.weights[0u].y * sdata[thread_idx.y][thread_idx.x + 16 - 2];
    accum += constants.weights[0u].x * sdata[thread_idx.y][thread_idx.x + 16 - 1];

    accum += constants.centerWeight * sdata[thread_idx.y][thread_idx.x + 16];

    accum += constants.weights[0u].x * sdata[thread_idx.y][thread_idx.x + 16 + 1];
    accum += constants.weights[0u].y * sdata[thread_idx.y][thread_idx.x + 16 + 2];
    accum += constants.weights[0u].z * sdata[thread_idx.y][thread_idx.x + 16 + 3];
    accum += constants.weights[0u].w * sdata[thread_idx.y][thread_idx.x + 16 + 4];

    accum += constants.weights[1u].x * sdata[thread_idx.y][thread_idx.x + 16 + 5];
    accum += constants.weights[1u].y * sdata[thread_idx.y][thread_idx.x + 16 + 6];
    accum += constants.weights[1u].z * sdata[thread_idx.y][thread_idx.x + 16 + 7];
    accum += constants.weights[1u].w * sdata[thread_idx.y][thread_idx.x + 16 + 8];

    accum += constants.weights[2u].x * sdata[thread_idx.y][thread_idx.x + 16 + 9];
    accum += constants.weights[2u].y * sdata[thread_idx.y][thread_idx.x + 16 + 10];
    accum += constants.weights[2u].z * sdata[thread_idx.y][thread_idx.x + 16 + 11];
    accum += constants.weights[2u].w * sdata[thread_idx.y][thread_idx.x + 16 + 12];

    accum += constants.weights[3u].x * sdata[thread_idx.y][thread_idx.x + 16 + 13];
    accum += constants.weights[3u].y * sdata[thread_idx.y][thread_idx.x + 16 + 14];
    accum += constants.weights[3u].z * sdata[thread_idx.y][thread_idx.x + 16 + 15];
    accum += constants.weights[3u].w * sdata[thread_idx.y][thread_idx.x + 16 + 16];

    tex_out[idx] = accum;
}
