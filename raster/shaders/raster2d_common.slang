// raster2d_common.slang

struct constants_t
{
    float4x4 view;
    float4 view_rot0;
    float4 view_rot1;
    float4 view_rot2;

    float near_plane;
    float far_plane;
    float fx;
    float fy;

    float cx;
    float cy;
    uint image_width;
    uint image_height;

    uint prim_count;
    uint n_isects;
    uint pad1;
    uint pad2;

    uint image_origin_w;
    uint image_origin_h;
    uint tile_origin_w;
    uint tile_origin_h;

    uint tile_width;
    uint tile_height;
    uint num_tiles_w;
    uint num_tiles_h;

    float3 view_dir;
    uint num_tiles;

    uint is_orthographic;
    uint pad3;
    uint pad4;
    uint pad5;
};

struct shader_params_t
{
    float near_plane;
    float far_plane;
    float eps2d;
    float radius_clip;

    uint tile_size;
    uint sh_degree;
    uint _pad1;
    uint _pad2;
};
