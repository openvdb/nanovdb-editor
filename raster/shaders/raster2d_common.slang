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
    uint sh_stride;
    uint points_grid_dim_x;
    uint isects_grid_dim_x;

    float* ptr_means;
    float* ptr_quats;
    float* ptr_scales;
    int* ptr_radii;
    float2* ptr_means2d;
    float* ptr_depths;
    float* ptr_conics;
    float* ptr_compensations;
    float* ptr_colors;
};

struct shader_params_t
{
    float eps2d;
    float radius_clip;

    uint tile_size;
    int sh_degree;
    uint sh_stride_rgbrgbrgb;
    uint _pad2;
};
