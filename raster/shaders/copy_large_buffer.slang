// copy_large_buffer.slang

struct constants_t
{
    uint64_t copy_element_count;
    uint64_t ptr_src;
    uint64_t ptr_dst;
    uint grid_dim_x;
    uint pad;
};

struct ptr_t
{
    uint64_t* ptr;
};
uint64_t load_uint64(uint64_t ptr_raw, uint64_t idx)
{
    ptr_raw = ptr_raw + 8u * idx;
    ptr_t ptr = reinterpret<ptr_t>(ptr_raw);
    return *ptr.ptr;
}
void store_uint64(uint64_t ptr_raw, uint64_t idx, uint64_t val)
{
    ptr_raw = ptr_raw + 8u * idx;
    ptr_t ptr = reinterpret<ptr_t>(ptr_raw);
    *ptr.ptr = val;
}

ConstantBuffer<constants_t> constants;

[shader("compute")][numthreads(256, 1, 1)]
void main(uint3 group_idx : SV_GroupID, uint3 thread_idx : SV_GroupThreadID)
{
    uint64_t begin_idx = uint64_t(group_idx.x) * 256u + thread_idx.x;
    uint64_t stride = uint64_t(constants.grid_dim_x) * 256u;

    for(uint64_t idx = begin_idx; idx < constants.copy_element_count; idx+=stride)
    {
        store_uint64(constants.ptr_dst, idx, load_uint64(constants.ptr_src, idx));
    }
}
