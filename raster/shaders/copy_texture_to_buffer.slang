// copy_texture_to_buffer.slang

struct constants_t
{
    uint width;
    uint height;
    uint pad1;
    uint pad2;
};

ConstantBuffer<constants_t> constants;

Texture2D<float> plane_0_in;
Texture2D<float2> plane_1_in;

RWStructuredBuffer<uint> buffer_out;

[shader("compute")][numthreads(16, 16, 1)]
void main(uint3 group_idx : SV_GroupID, uint3 thread_idx : SV_GroupThreadID)
{
    uint2 pixel_idx = group_idx.xy * 16u + thread_idx.xy;

    if (pixel_idx.x >= constants.width || pixel_idx.y >= constants.height)
    {
        return;
    }

    if ((pixel_idx.x & 3) == 0)
    {
        uint raw = 0u;
        raw |= uint(255.f * max(0.f, min(1.f, plane_0_in[pixel_idx])));
        raw |= (uint(255.f * max(0.f, min(1.f, plane_0_in[pixel_idx + int2(1, 0)]))) << 8u);
        raw |= (uint(255.f * max(0.f, min(1.f, plane_0_in[pixel_idx + int2(2, 0)]))) << 16u);
        raw |= (uint(255.f * max(0.f, min(1.f, plane_0_in[pixel_idx + int2(3, 0)]))) << 24u);

        uint buffer_idx = pixel_idx.y * constants.width + pixel_idx.x;

        buffer_out[buffer_idx / 4] = raw;
    }

    if (pixel_idx.x >= (constants.width / 2u) || pixel_idx.y >= (constants.height / 2u))
    {
        return;
    }

    if ((pixel_idx.x & 3) == 0)
    {
        uint raw = 0u;
        raw |= uint(255.f * max(0.f, min(1.f, plane_1_in[pixel_idx].x)));
        raw |= (uint(255.f * max(0.f, min(1.f, plane_1_in[pixel_idx + int2(1, 0)].x))) << 8u);
        raw |= (uint(255.f * max(0.f, min(1.f, plane_1_in[pixel_idx + int2(2, 0)].x))) << 16u);
        raw |= (uint(255.f * max(0.f, min(1.f, plane_1_in[pixel_idx + int2(3, 0)].x))) << 24u);

        uint offset = constants.width * constants.height;
        uint buffer_idx = pixel_idx.y * (constants.width / 2u) + pixel_idx.x;

        buffer_out[(offset + buffer_idx) / 4] = raw;
    }

    if ((pixel_idx.x & 3) == 0)
    {
        uint raw = 0u;
        raw |= uint(255.f * max(0.f, min(1.f, plane_1_in[pixel_idx].y)));
        raw |= (uint(255.f * max(0.f, min(1.f, plane_1_in[pixel_idx + int2(1, 0)].y))) << 8u);
        raw |= (uint(255.f * max(0.f, min(1.f, plane_1_in[pixel_idx + int2(2, 0)].y))) << 16u);
        raw |= (uint(255.f * max(0.f, min(1.f, plane_1_in[pixel_idx + int2(3, 0)].y))) << 24u);

        uint offset = constants.width * constants.height +
            (constants.width / 2u) * (constants.height / 2u);
        uint buffer_idx = pixel_idx.y * (constants.width / 2u) + pixel_idx.x;

        buffer_out[(offset + buffer_idx) / 4] = raw;
    }
}
