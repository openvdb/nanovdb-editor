// copy_large_buffer.slang

struct constants_t
{
    uint64_t copy_element_count;
    uint64_t* ptr_src;
    uint64_t* ptr_dst;
    uint grid_dim_x;
    uint pad;
};

ConstantBuffer<constants_t> constants;

[shader("compute")][numthreads(256, 1, 1)]
void main(uint3 group_idx : SV_GroupID, uint3 thread_idx : SV_GroupThreadID)
{
    uint64_t begin_idx = uint64_t(group_idx.x) * 256u + thread_idx.x;
    uint64_t stride = uint64_t(constants.grid_dim_x) * 256u;

    uint64_t* ptr_src = constants.ptr_src;
    uint64_t* ptr_dst = constants.ptr_dst;
    for(uint64_t idx = begin_idx; idx < constants.copy_element_count; idx+=stride)
    {
        ptr_dst[begin_idx] = ptr_src[begin_idx];
        ptr_src = ptr_src + stride;
        ptr_dst = ptr_dst + stride;
    }
}
