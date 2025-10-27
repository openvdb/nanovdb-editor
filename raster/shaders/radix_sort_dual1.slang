// radix_sort_dual1.slang

struct constants_t
{
    uint workgroup_count;
    uint key_count;
    uint grid_dim_x;
};

StructuredBuffer<uint> key_low_in;

ConstantBuffer<constants_t> constants;

RWStructuredBuffer<uint> key_tmp_out;
RWStructuredBuffer<uint> val_tmp_out;

[shader("compute")][numthreads(1024u, 1, 1)]
void main(uint3 group_idx : SV_GroupID, uint3 thread_idx : SV_GroupThreadID)
{
    uint group_idx_1d = group_idx.y * constants.grid_dim_x + group_idx.x;
    uint idx = 1024u * group_idx_1d + thread_idx.x;

    if (idx >= constants.key_count)
    {
        return;
    }

    key_tmp_out[idx] = key_low_in[idx];
    val_tmp_out[idx] = idx;
}
