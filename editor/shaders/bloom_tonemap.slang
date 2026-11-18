// bloom_tonemap.slang

struct constants_t
{
	uint mode;
	float weight;
	float threshold;
	float size;
	float4 texDimInv;
	float4 centerWeight;
	float4 weights[4u];
};

ConstantBuffer<constants_t> constants;

Texture2D<float4> color_in;
RWTexture2D<float4> color_out;

Texture2D<float4> downsample_in;
SamplerState sampler_in;

[shader("compute")][numthreads(16, 16, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    int2 idx = int2(dispatchThreadID.xy);

	float4 color = color_in[idx];

	float4 bloom = downsample_in.SampleLevel(sampler_in, (float2(idx) + float2(0.5f, 0.5f)) * constants.texDimInv.zw, 0.0);

	color.rgb += bloom.rgb;

	//color.rgb *= constants.scale.rgb;

	color.r = color.r / (1.f + color.r);
	color.g = color.g / (1.f + color.g);
	color.b = color.b / (1.f + color.b);

	//color.rgb = saturate(color.rgb);

	color.r = pow(abs(color.r), 1.f / 2.2f);
	color.g = pow(abs(color.g), 1.f / 2.2f);
	color.b = pow(abs(color.b), 1.f / 2.2f);

	color_out[idx] = color;
}
