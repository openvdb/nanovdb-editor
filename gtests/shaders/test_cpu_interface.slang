// test_cpu_interface.slang

// note: GUID from Slang sample
[COM("007E2DAF-3A1F-4BC3-B08B-A9DB233D38CE")]
interface print_iface
{
    void print();
};

struct constants_t
{
    int magic_number;
    int pad1;
    print_iface print;
};

StructuredBuffer<int> data_in;
ConstantBuffer<constants_t> constants;
RWStructuredBuffer<int> data_out;
RWStructuredBuffer<uint> scratch;

[shader("compute")][numthreads(8, 1, 1)]
void computeMain(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint idx = dispatchThreadID.x;

    data_out[idx] = data_in[idx] + constants.magic_number;
    constants.print.print();
}
