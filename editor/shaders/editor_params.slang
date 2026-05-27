struct EditorParams
{
    float4x4 view_inv;
    float4x4 projection_inv;

    float4x4 view;
    float4x4 projection;

    uint width;
    uint height;
    uint composite;
    uint pad2;
};
