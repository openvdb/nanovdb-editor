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

    uint64_t ptr_means;
    uint64_t ptr_quats;
    uint64_t ptr_scales;
    uint64_t ptr_radii;
    uint64_t ptr_means2d;
    uint64_t ptr_depths;
    uint64_t ptr_conics;
    uint64_t ptr_compensations;
    uint64_t ptr_sh_0;
    uint64_t ptr_sh_n;
    uint64_t ptr_opacities;
    uint64_t ptr_colors;
    uint64_t ptr_num_tiles_per_gaussian;
    uint64_t ptr_scan_tiles_per_gaussian;
    uint64_t ptr_intersection_keys_low;
    uint64_t ptr_intersection_keys_high;
    uint64_t ptr_intersection_vals;
    uint64_t ptr_tile_offsets;
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

struct ptr_uint_t
{
    uint* ptr;
};
uint load_uint(uint64_t ptr_raw, uint64_t idx)
{
    ptr_raw = ptr_raw + 4u * idx;
    ptr_uint_t ptr = reinterpret<ptr_uint_t>(ptr_raw);
    return *ptr.ptr;
}
void store_uint(uint64_t ptr_raw, uint64_t idx, uint val)
{
    ptr_raw = ptr_raw + 4u * idx;
    ptr_uint_t ptr = reinterpret<ptr_uint_t>(ptr_raw);
    *ptr.ptr = val;
}

struct ptr_int_t
{
    int* ptr;
};
int load_int(uint64_t ptr_raw, uint64_t idx)
{
    ptr_raw = ptr_raw + 4u * idx;
    ptr_int_t ptr = reinterpret<ptr_int_t>(ptr_raw);
    return *ptr.ptr;
}
void store_int(uint64_t ptr_raw, uint64_t idx, int val)
{
    ptr_raw = ptr_raw + 4u * idx;
    ptr_int_t ptr = reinterpret<ptr_int_t>(ptr_raw);
    *ptr.ptr = val;
}

struct ptr_float_t
{
    float* ptr;
};
float load_float(uint64_t ptr_raw, uint64_t idx)
{
    ptr_raw = ptr_raw + 4u * idx;
    ptr_float_t ptr = reinterpret<ptr_float_t>(ptr_raw);
    return *ptr.ptr;
}
void store_float(uint64_t ptr_raw, uint64_t idx, float val)
{
    ptr_raw = ptr_raw + 4u * idx;
    ptr_float_t ptr = reinterpret<ptr_float_t>(ptr_raw);
    *ptr.ptr = val;
}

struct ptr_float2_t
{
    float2* ptr;
};
float2 load_float2(uint64_t ptr_raw, uint64_t idx)
{
    ptr_raw = ptr_raw + 8u * idx;
    ptr_float2_t ptr = reinterpret<ptr_float2_t>(ptr_raw);
    return *ptr.ptr;
}
void store_float2(uint64_t ptr_raw, uint64_t idx, float2 val)
{
    ptr_raw = ptr_raw + 8u * idx;
    ptr_float2_t ptr = reinterpret<ptr_float2_t>(ptr_raw);
    *ptr.ptr = val;
}