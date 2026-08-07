// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/compute/ShaderParamsTypes.h

    \author Andrew Reidmeyer

    \brief
*/

#include "nanovdb_editor/putil/Reflect.h"

struct shader_params_vec2_t
{
    float x, y;
};
#define PNANOVDB_REFLECT_TYPE shader_params_vec2_t
PNANOVDB_REFLECT_BEGIN()
PNANOVDB_REFLECT_VALUE(float, x, 0, 0)
PNANOVDB_REFLECT_VALUE(float, y, 0, 0)
PNANOVDB_REFLECT_END(0)
#undef PNANOVDB_REFLECT_TYPE

struct shader_params_vec3_t
{
    float x, y, z;
};
#define PNANOVDB_REFLECT_TYPE shader_params_vec3_t
PNANOVDB_REFLECT_BEGIN()
PNANOVDB_REFLECT_VALUE(float, x, 0, 0)
PNANOVDB_REFLECT_VALUE(float, y, 0, 0)
PNANOVDB_REFLECT_VALUE(float, z, 0, 0)
PNANOVDB_REFLECT_END(0)
#undef PNANOVDB_REFLECT_TYPE

struct shader_params_vec4_t
{
    float x, y, z, w;
};
#define PNANOVDB_REFLECT_TYPE shader_params_vec4_t
PNANOVDB_REFLECT_BEGIN()
PNANOVDB_REFLECT_VALUE(float, x, 0, 0)
PNANOVDB_REFLECT_VALUE(float, y, 0, 0)
PNANOVDB_REFLECT_VALUE(float, z, 0, 0)
PNANOVDB_REFLECT_VALUE(float, w, 0, 0)
PNANOVDB_REFLECT_END(0)
#undef PNANOVDB_REFLECT_TYPE

struct shader_params_ivec2_t
{
    pnanovdb_int32_t x, y;
};
#define PNANOVDB_REFLECT_TYPE shader_params_ivec2_t
PNANOVDB_REFLECT_BEGIN()
PNANOVDB_REFLECT_VALUE(pnanovdb_int32_t, x, 0, 0)
PNANOVDB_REFLECT_VALUE(pnanovdb_int32_t, y, 0, 0)
PNANOVDB_REFLECT_END(0)
#undef PNANOVDB_REFLECT_TYPE

struct shader_params_ivec3_t
{
    pnanovdb_int32_t x, y, z;
};
#define PNANOVDB_REFLECT_TYPE shader_params_ivec3_t
PNANOVDB_REFLECT_BEGIN()
PNANOVDB_REFLECT_VALUE(pnanovdb_int32_t, x, 0, 0)
PNANOVDB_REFLECT_VALUE(pnanovdb_int32_t, y, 0, 0)
PNANOVDB_REFLECT_VALUE(pnanovdb_int32_t, z, 0, 0)
PNANOVDB_REFLECT_END(0)
#undef PNANOVDB_REFLECT_TYPE

struct shader_params_ivec4_t
{
    pnanovdb_int32_t x, y, z, w;
};
#define PNANOVDB_REFLECT_TYPE shader_params_ivec4_t
PNANOVDB_REFLECT_BEGIN()
PNANOVDB_REFLECT_VALUE(pnanovdb_int32_t, x, 0, 0)
PNANOVDB_REFLECT_VALUE(pnanovdb_int32_t, y, 0, 0)
PNANOVDB_REFLECT_VALUE(pnanovdb_int32_t, z, 0, 0)
PNANOVDB_REFLECT_VALUE(pnanovdb_int32_t, w, 0, 0)
PNANOVDB_REFLECT_END(0)
#undef PNANOVDB_REFLECT_TYPE

struct shader_params_uvec2_t
{
    pnanovdb_uint32_t x, y;
};
#define PNANOVDB_REFLECT_TYPE shader_params_uvec2_t
PNANOVDB_REFLECT_BEGIN()
PNANOVDB_REFLECT_VALUE(pnanovdb_uint32_t, x, 0, 0)
PNANOVDB_REFLECT_VALUE(pnanovdb_uint32_t, y, 0, 0)
PNANOVDB_REFLECT_END(0)
#undef PNANOVDB_REFLECT_TYPE

struct shader_params_uvec3_t
{
    pnanovdb_uint32_t x, y, z;
};
#define PNANOVDB_REFLECT_TYPE shader_params_uvec3_t
PNANOVDB_REFLECT_BEGIN()
PNANOVDB_REFLECT_VALUE(pnanovdb_uint32_t, x, 0, 0)
PNANOVDB_REFLECT_VALUE(pnanovdb_uint32_t, y, 0, 0)
PNANOVDB_REFLECT_VALUE(pnanovdb_uint32_t, z, 0, 0)
PNANOVDB_REFLECT_END(0)
#undef PNANOVDB_REFLECT_TYPE

struct shader_params_uvec4_t
{
    pnanovdb_uint32_t x, y, z, w;
};
#define PNANOVDB_REFLECT_TYPE shader_params_uvec4_t
PNANOVDB_REFLECT_BEGIN()
PNANOVDB_REFLECT_VALUE(pnanovdb_uint32_t, x, 0, 0)
PNANOVDB_REFLECT_VALUE(pnanovdb_uint32_t, y, 0, 0)
PNANOVDB_REFLECT_VALUE(pnanovdb_uint32_t, z, 0, 0)
PNANOVDB_REFLECT_VALUE(pnanovdb_uint32_t, w, 0, 0)
PNANOVDB_REFLECT_END(0)
#undef PNANOVDB_REFLECT_TYPE

struct shader_params_mat4x4_t
{
    shader_params_vec4_t x, y, z, w;
};
#define PNANOVDB_REFLECT_TYPE shader_params_mat4x4_t
PNANOVDB_REFLECT_BEGIN()
PNANOVDB_REFLECT_VALUE(shader_params_vec4_t, x, 0, 0)
PNANOVDB_REFLECT_VALUE(shader_params_vec4_t, y, 0, 0)
PNANOVDB_REFLECT_VALUE(shader_params_vec4_t, z, 0, 0)
PNANOVDB_REFLECT_VALUE(shader_params_vec4_t, w, 0, 0)
PNANOVDB_REFLECT_END(0)
#undef PNANOVDB_REFLECT_TYPE

static const pnanovdb_reflect_data_type_t* get_vector_type(pnanovdb_uint32_t num_elements, pnanovdb_reflect_type_t data_type)
{
    const pnanovdb_reflect_data_type_t* reflect_type = nullptr;
    if (num_elements == 2u && data_type == PNANOVDB_REFLECT_TYPE_FLOAT)
    {
        reflect_type = PNANOVDB_REFLECT_DATA_TYPE(shader_params_vec2_t);
    }
    else if (num_elements == 3u && data_type == PNANOVDB_REFLECT_TYPE_FLOAT)
    {
        reflect_type = PNANOVDB_REFLECT_DATA_TYPE(shader_params_vec3_t);
    }
    else if (num_elements == 4u && data_type == PNANOVDB_REFLECT_TYPE_FLOAT)
    {
        reflect_type = PNANOVDB_REFLECT_DATA_TYPE(shader_params_vec4_t);
    }
    else if (num_elements == 2u && data_type == PNANOVDB_REFLECT_TYPE_INT32)
    {
        reflect_type = PNANOVDB_REFLECT_DATA_TYPE(shader_params_ivec2_t);
    }
    else if (num_elements == 3u && data_type == PNANOVDB_REFLECT_TYPE_INT32)
    {
        reflect_type = PNANOVDB_REFLECT_DATA_TYPE(shader_params_ivec3_t);
    }
    else if (num_elements == 4u && data_type == PNANOVDB_REFLECT_TYPE_INT32)
    {
        reflect_type = PNANOVDB_REFLECT_DATA_TYPE(shader_params_ivec4_t);
    }
    else if (num_elements == 2u && data_type == PNANOVDB_REFLECT_TYPE_UINT32)
    {
        reflect_type = PNANOVDB_REFLECT_DATA_TYPE(shader_params_uvec2_t);
    }
    else if (num_elements == 3u && data_type == PNANOVDB_REFLECT_TYPE_UINT32)
    {
        reflect_type = PNANOVDB_REFLECT_DATA_TYPE(shader_params_uvec3_t);
    }
    else if (num_elements == 4u && data_type == PNANOVDB_REFLECT_TYPE_UINT32)
    {
        reflect_type = PNANOVDB_REFLECT_DATA_TYPE(shader_params_uvec4_t);
    }
    else if (num_elements == 16u && data_type == PNANOVDB_REFLECT_TYPE_FLOAT)
    {
        reflect_type = PNANOVDB_REFLECT_DATA_TYPE(shader_params_mat4x4_t);
    }
    return reflect_type;
}
