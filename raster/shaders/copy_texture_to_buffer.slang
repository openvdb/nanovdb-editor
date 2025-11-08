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

    {
        uint raw = uint(255.f * max(0.f, min(1.f, plane_0_in[pixel_idx])));

        uint buffer_idx = pixel_idx.y * constants.width + pixel_idx.x;
        uint shift = (buffer_idx & 3) << 3u;

        InterlockedAnd(buffer_out[buffer_idx / 4], ~(0xFF << shift));
        InterlockedOr(buffer_out[buffer_idx / 4], raw << shift);
    }

    if (pixel_idx.x >= (constants.width / 2u) || pixel_idx.y >= (constants.height / 2u))
    {
        return;
    }

    {
        uint raw = uint(255.f * max(0.f, min(1.f, plane_1_in[pixel_idx].x)));

        uint offset = constants.width * constants.height;
        uint buffer_idx = offset + pixel_idx.y * (constants.width / 2u) + pixel_idx.x;
        uint shift = (buffer_idx & 3) << 3u;

        InterlockedAnd(buffer_out[buffer_idx / 4], ~(0xFF << shift));
        InterlockedOr(buffer_out[buffer_idx / 4], raw << shift);
    }

    {
        uint raw = uint(255.f * max(0.f, min(1.f, plane_1_in[pixel_idx].y)));

        uint offset = constants.width * constants.height +
            (constants.width / 2u) * (constants.height / 2u);
        uint buffer_idx = offset + pixel_idx.y * (constants.width / 2u) + pixel_idx.x;
        uint shift = (buffer_idx & 3) << 3u;

        InterlockedAnd(buffer_out[buffer_idx / 4], ~(0xFF << shift));
        InterlockedOr(buffer_out[buffer_idx / 4], raw << shift);
    }
}
