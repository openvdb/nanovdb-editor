// radix_sort_dual2.slang

struct constants_t
{
    uint workgroup_count;
    uint key_count;
    uint grid_dim_x;
};

StructuredBuffer<uint> key_high_in;
StructuredBuffer<uint> val_tmp_in;

ConstantBuffer<constants_t> constants;

RWStructuredBuffer<uint> key_tmp_out;

[shader("compute")][numthreads(1024u, 1, 1)]
void main(uint3 group_idx : SV_GroupID, uint3 thread_idx : SV_GroupThreadID)
{
    uint group_idx_1d = group_idx.y * constants.grid_dim_x + group_idx.x;
    uint idx = 1024u * group_idx_1d + thread_idx.x;

    if (idx >= constants.key_count)
    {
        return;
    }

    uint src_idx = val_tmp_in[idx];
    key_tmp_out[idx] = key_high_in[src_idx];
}
