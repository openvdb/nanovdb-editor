// bloom_downsample.slang

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
SamplerState sampler_in;

[shader("compute")][numthreads(16, 16, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
	int2 idx = int2(dispatchThreadID.xy);

	float4 color = 0.f.xxxx;

	color += 0.25f * color_in.SampleLevel(sampler_in, (float2(idx) + float2(0.f, 0.f)) * constants.texDimInv.xy, 0.0);
	color += 0.25f * color_in.SampleLevel(sampler_in, (float2(idx) + float2(1.f, 0.f)) * constants.texDimInv.xy, 0.0);
	color += 0.25f * color_in.SampleLevel(sampler_in, (float2(idx) + float2(0.f, 1.f)) * constants.texDimInv.xy, 0.0);
	color += 0.25f * color_in.SampleLevel(sampler_in, (float2(idx) + float2(1.f, 1.f)) * constants.texDimInv.xy, 0.0);

	color.rgb -= constants.threshold;

	color.rgb = max(0.f.xxx, color.rgb);

	color_out[idx] = color;
}
