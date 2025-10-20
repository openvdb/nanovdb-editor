// clear_2d.slang

struct constants_t
{
    uint image_width;
    uint image_height;
    float depth_value;
    float pad1;
    float red;
    float green;
    float blue;
    float alpha;
};

ConstantBuffer<constants_t> constants;

RWTexture2D<float4> color_2d_out;
RWTexture2D<float> depth_out;

[shader("compute")][numthreads(16, 16, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    int2 tidx = int2(dispatchThreadID.xy);

    if (tidx.x < constants.image_width && tidx.y < constants.image_height)
    {
        color_2d_out[tidx] = float4(constants.red, constants.green, constants.blue, constants.alpha);
        depth_out[tidx] = constants.depth_value;
    }
}
