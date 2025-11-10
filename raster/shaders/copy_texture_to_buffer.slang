// copy_texture_to_buffer.slang

struct constants_t
{
    uint grid_dim_x;
    uint y_width;
    uint uv_width;
    uint plane0_end;
    uint plane1_end;
    uint plane2_end;
    uint pad1;
    uint pad2;
};

ConstantBuffer<constants_t> constants;

Texture2D<float> plane_0_in;
Texture2D<float2> plane_1_in;

RWStructuredBuffer<uint> buffer_out;

uint gather_value(uint buffer_idx)
{
    uint value = 0u;
    if (buffer_idx < constants.plane0_end)
    {
        uint pixel_idx_1d = buffer_idx;
        uint2 pixel_idx = uint2(
            pixel_idx_1d % constants.y_width,
            pixel_idx_1d / constants.y_width
        );

        value = uint(255.f * max(0.f, min(1.f, plane_0_in[pixel_idx])));
    }
    else if (buffer_idx < constants.plane1_end)
    {
        uint pixel_idx_1d = buffer_idx - constants.plane0_end;
        uint2 pixel_idx = uint2(
            pixel_idx_1d % constants.uv_width,
            pixel_idx_1d / constants.uv_width
        );

        value = uint(255.f * max(0.f, min(1.f, plane_1_in[pixel_idx].x)));
    }
    else if (buffer_idx < constants.plane2_end)
    {
        uint pixel_idx_1d = buffer_idx - constants.plane1_end;
        uint2 pixel_idx = uint2(
            pixel_idx_1d % constants.uv_width,
            pixel_idx_1d / constants.uv_width
        );

        value = uint(255.f * max(0.f, min(1.f, plane_1_in[pixel_idx].y)));
    }
    return value;
}

[shader("compute")][numthreads(256, 1, 1)]
void main(uint3 group_idx : SV_GroupID, uint3 thread_idx : SV_GroupThreadID)
{
    uint group_idx_1d = (group_idx.y * constants.grid_dim_x + group_idx.x);
    uint buffer_word_idx = group_idx_1d * 256u + thread_idx.x;

    if (!(4u * buffer_word_idx < constants.plane2_end))
    {
        return;
    }

    uint4 value4 = uint4(
        gather_value(4u * buffer_word_idx + 0u),
        gather_value(4u * buffer_word_idx + 1u),
        gather_value(4u * buffer_word_idx + 2u),
        gather_value(4u * buffer_word_idx + 3u)
    );

    uint value = (value4.x) | (value4.y << 8u) |
        (value4.z << 16u) | (value4.w << 24u);

    buffer_out[buffer_word_idx] = value;
}
