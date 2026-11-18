// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   compute/FlowPipelines.cpp

    \author Andrew Reidmeyer

    \brief  This file provides Flow + NanoVDB related pipelines
*/

#include "nanovdb_editor/putil/Compute.h"
#include "nanovdb_editor/putil/Pipeline.h"
#include "nanovdb_editor/putil/Camera.h"
#include "nanovdb_editor/putil/Reflect.h"

#include <vector>

namespace pnanovdb_pipeline_flow_render
{
    struct bloom_params_t
    {
        pnanovdb_vec3_t colorScale;
        float weightScale;
        float sizeScale;
        float threshold;
        float weight1;
        float size1;
        float weight2;
        float size2;
        float weight3;
        float size3;
        float weight4;
        float size4;
        float weight5;
        float size5;
    };
    #define bloom_params_default_init { \
        {1.f, 1.f, 1.f}, 0.03f, 0.15f, 1.f, \
        0.35f, 0.003f, \
        0.14f, 0.001f, \
        0.12f, 0.02f, \
        0.06f, 0.10f, \
        0.06f, 0.30f \
    }
    static const bloom_params_t bloom_params_t_default = bloom_params_default_init;
    #define PNANOVDB_REFLECT_TYPE bloom_params_t
    PNANOVDB_REFLECT_BEGIN()
    PNANOVDB_REFLECT_VALUE(pnanovdb_vec3_t, colorScale, 0, 0)
    PNANOVDB_REFLECT_VALUE(float, weightScale, 0, 0)
    PNANOVDB_REFLECT_VALUE(float, sizeScale, 0, 0)
    PNANOVDB_REFLECT_VALUE(float, threshold, 0, 0)
    PNANOVDB_REFLECT_VALUE(float, weight1, 0, 0)
    PNANOVDB_REFLECT_VALUE(float, size1, 0, 0)
    PNANOVDB_REFLECT_VALUE(float, weight2, 0, 0)
    PNANOVDB_REFLECT_VALUE(float, size2, 0, 0)
    PNANOVDB_REFLECT_VALUE(float, weight3, 0, 0)
    PNANOVDB_REFLECT_VALUE(float, size3, 0, 0)
    PNANOVDB_REFLECT_VALUE(float, weight4, 0, 0)
    PNANOVDB_REFLECT_VALUE(float, size4, 0, 0)
    PNANOVDB_REFLECT_VALUE(float, weight5, 0, 0)
    PNANOVDB_REFLECT_VALUE(float, size5, 0, 0)
    PNANOVDB_REFLECT_END(&bloom_params_t_default)
    #undef PNANOVDB_REFLECT_TYPE

    struct bloom_ctx_t;
    bloom_ctx_t* bloom_create(const pnanovdb_compute_t* compute, pnanovdb_compute_queue_t* queue);
    void bloom_destroy(const pnanovdb_compute_t* compute, pnanovdb_compute_queue_t* queue, bloom_ctx_t* ctx);
    void bloom_update(const pnanovdb_compute_t* compute, pnanovdb_compute_queue_t* queue, bloom_ctx_t* ctx,
                      int width, int height, const bloom_params_t* params,
                      pnanovdb_compute_texture_transient_t* image_in, pnanovdb_compute_texture_transient_t* image_out);

    struct Instance
    {
        pnanovdb_shader_context_t* shader_ctx = nullptr;
        const char* shader_path = nullptr;
        std::vector<uint8_t> shader_params_data;
        std::vector<uint8_t> shader_params_metadata;

        bloom_ctx_t* bloom_ctx = nullptr;
    };
    PNANOVDB_CAST_PAIR(Instance, pnanovdb_pipeline_instance_t)

    static const pnanovdb_uint64_t default_point_count = 6u;
    static const float x_points_default[default_point_count] = {
        0.000000f,
        0.050000f,
        0.15000f,
        0.600000f,
        0.850000f,
        1.000000f
    };
    static const pnanovdb_vec4_t rgba_points_default[default_point_count] = {
        { 0.015400f,    0.017700f,    0.015400f,    0.004902f },
        { 0.035750f,    0.035750f,    0.035750f,    0.504902f },
        { 0.035750f,    0.035750f,    0.035750f,    0.504902f },
        { 1.f,            0.1594125f, 0.0135315f, 0.800000f },
        { 13.534992f,    2.986956f,  0.125991f,  0.800000f },
        { 78.08f,       39.04f,     6.1f,       0.700000f },
    };
    struct pnanovdb_pipeline_nanovdb_render_params_t
    {
        pnanovdb_pipeline_shader_t shader;
        pnanovdb_uint32_t colormap_resolution;
        const float* x_points;
        pnanovdb_uint64_t x_point_count;
        const pnanovdb_vec4_t* rgba_points;
        pnanovdb_uint64_t rgba_point_count;
        bloom_params_t bloom;
    };
    static const pnanovdb_pipeline_nanovdb_render_params_t pnanovdb_pipeline_nanovdb_render_params_t_default = {
        {"editor/flow_raymarch.slang", nullptr, nullptr, nullptr, nullptr}, // shader
        32u,
        x_points_default, default_point_count,
        rgba_points_default, default_point_count,
        bloom_params_default_init
    };
    #define PNANOVDB_REFLECT_TYPE pnanovdb_pipeline_nanovdb_render_params_t
    PNANOVDB_REFLECT_BEGIN()
    PNANOVDB_REFLECT_VALUE(pnanovdb_pipeline_shader_t, shader, 0, 0)
    PNANOVDB_REFLECT_VALUE(pnanovdb_uint32_t, colormap_resolution, 0, 0)
    PNANOVDB_REFLECT_ARRAY(float, x_points, x_point_count, 0, 0)
    PNANOVDB_REFLECT_ARRAY(pnanovdb_vec4_t, rgba_points, rgba_point_count, 0, 0)
    PNANOVDB_REFLECT_VALUE(bloom_params_t, bloom, 0, 0)
    PNANOVDB_REFLECT_END(&pnanovdb_pipeline_nanovdb_render_params_t_default)
    #undef PNANOVDB_REFLECT_TYPE

    struct pnanovdb_pipeline_nanovdb_render_metadata_item_t
    {
        const char** names;
        pnanovdb_uint64_t name_count;
    };
    #define PNANOVDB_REFLECT_TYPE pnanovdb_pipeline_nanovdb_render_metadata_item_t
    PNANOVDB_REFLECT_BEGIN()
    PNANOVDB_REFLECT_POINTER_ARRAY(char, names, name_count, 0, 0)
    PNANOVDB_REFLECT_VALUE(pnanovdb_uint64_t, name_count, 0, 0)
    PNANOVDB_REFLECT_END()
    #undef PNANOVDB_REFLECT_TYPE
    struct pnanovdb_pipeline_nanovdb_render_metadata_t
    {
        pnanovdb_pipeline_nanovdb_render_metadata_item_t shader;
    };
    static const char* shader_names[] = {
        "editor/flow_raymarch.slang"
    };
    static const pnanovdb_pipeline_nanovdb_render_metadata_t pnanovdb_pipeline_nanovdb_render_metadata_t_default = {
        {shader_names, 1u} // shader
    };
    #define PNANOVDB_REFLECT_TYPE pnanovdb_pipeline_nanovdb_render_metadata_t
    PNANOVDB_REFLECT_BEGIN()
    PNANOVDB_REFLECT_VALUE(pnanovdb_pipeline_nanovdb_render_metadata_item_t, shader, 0, 0)
    PNANOVDB_REFLECT_END(&pnanovdb_pipeline_nanovdb_render_metadata_t_default)
    #undef PNANOVDB_REFLECT_TYPE

    void get_pipeline_params_type(const pnanovdb_compute_t* compute,
                                  const pnanovdb_compiler_t* compiler,
                                  const pnanovdb_reflect_data_type_t** out_data_type,
                                  const pnanovdb_reflect_data_type_t** out_metadata_type)
    {
        *out_data_type = PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_pipeline_nanovdb_render_params_t);
        *out_metadata_type = PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_pipeline_nanovdb_render_metadata_t);
    }

    pnanovdb_pipeline_instance_t* create(const pnanovdb_compute_t* compute,
                                         pnanovdb_compute_queue_t* queue)
    {
        auto ptr = new Instance();

        ptr->bloom_ctx = bloom_create(compute, queue);

        return cast(ptr);
    }

    void destroy(const pnanovdb_compute_t* compute,
                 pnanovdb_compute_queue_t* queue,
                 pnanovdb_pipeline_instance_t* instance)
    {
        auto ptr = cast(instance);

        bloom_destroy(compute, queue, ptr->bloom_ctx);

        pnanovdb_compute_interface_t* compute_interface = compute->device_interface.get_compute_interface(queue);
        pnanovdb_compute_context_t* context = compute->device_interface.get_compute_context(queue);
        if (ptr->shader_ctx)
        {
            compute->destroy_shader_context(compute, queue, ptr->shader_ctx);
            ptr->shader_ctx = nullptr;
        }

        delete ptr;
    }

    pnanovdb_bool_t get_requirements(pnanovdb_pipeline_requirements_params_t* params,
                                     pnanovdb_pipeline_requirements_output_t* output)
    {
        if (!params->params_data || !params->params_data_type ||
            !pnanovdb_reflect_layout_compare(params->params_data_type, PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_pipeline_nanovdb_render_params_t)))
        {
            *output->p_name_count = 0u;
            return PNANOVDB_TRUE;
        }
        if (!output->names)
        {
            *output->p_name_count = 2u;
            return PNANOVDB_FALSE;
        }
        auto ptr = (pnanovdb_pipeline_nanovdb_render_params_t*)params->params_data;
        if (*output->p_name_count >= 1u) { output->names[0u] = "smoke"; }
        if (*output->p_name_count >= 2u) { output->names[1u] = "temperature"; }
        if (*output->p_name_count >= 2u)
        {
            *output->p_name_count = 2u;
            return PNANOVDB_TRUE;
        }
        return PNANOVDB_FALSE;
    }

    static pnanovdb_vec4_t compute_texel(pnanovdb_uint32_t idx,
                                         pnanovdb_uint32_t count,
                                         pnanovdb_pipeline_nanovdb_render_params_t* params)
    {
        float u = (float(idx) + 0.5f) * (1.f / float(count));

        pnanovdb_uint32_t beginPtIdx = 0u;
        pnanovdb_uint32_t endPtIdx = params->x_point_count < params->rgba_point_count ?
                                     params->x_point_count : params->rgba_point_count;

        pnanovdb_uint32_t xlowIdx = ~0u;
        float xlowVal = 0.f;
        pnanovdb_uint32_t xhighIdx = ~0u;
        float xhighVal = 0.f;
        for (pnanovdb_uint32_t ptIdx = beginPtIdx; ptIdx < endPtIdx; ptIdx++)
        {
            float x = params->x_points[ptIdx];
            if (x <= u && (x >= xlowVal || xlowIdx == ~0u))
            {
                xlowIdx = ptIdx;
                xlowVal = x;
            }
            if (x >= u && (x <= xhighVal || xhighIdx == ~0u))
            {
                xhighIdx = ptIdx;
                xhighVal = x;
            }
        }

        pnanovdb_vec4_t a = xlowIdx != ~0u ? params->rgba_points[xlowIdx] : pnanovdb_vec4_t{0.f, 0.f, 0.f, 0.f};
        pnanovdb_vec4_t b = xhighIdx != ~0u ? params->rgba_points[xhighIdx] : pnanovdb_vec4_t{1.f, 1.f, 1.f, 1.f};
        float t = (u - xlowVal) / (xhighVal - xlowVal);
        pnanovdb_vec4_t c = {
            (1.f - t) * a.x + t * b.x,
            (1.f - t) * a.y + t * b.y,
            (1.f - t) * a.z + t * b.z,
            (1.f - t) * a.w + t * b.w,
        };

        //c.xyz *= layerParamsIn[layerParamIdx].colorScale;
        return c;
    }

    void render(pnanovdb_pipeline_instance_t* instance,
                const pnanovdb_compute_t* compute,
                pnanovdb_compute_queue_t* queue,
                pnanovdb_pipeline_input_t* input,
                pnanovdb_pipeline_render_target_t* target)
    {
        auto ptr = cast(instance);

        pnanovdb_compute_interface_t* compute_interface = compute->device_interface.get_compute_interface(queue);
        pnanovdb_compute_context_t* context = compute->device_interface.get_compute_context(queue);

        if (!input->params_data || !input->params_data_type ||
            !pnanovdb_reflect_layout_compare(
                PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_pipeline_nanovdb_render_params_t),
                input->params_data_type))
        {
            return;
        }
        pnanovdb_pipeline_nanovdb_render_params_t* params = (pnanovdb_pipeline_nanovdb_render_params_t*)input->params_data;

        pnanovdb_uint64_t smoke_idx = ~0llu;
        pnanovdb_uint64_t temperature_idx = ~0llu;
        for (pnanovdb_uint64_t name_idx = 0u;name_idx < input->buffer_count;name_idx++)
        {
            if (pnanovdb_reflect_string_compare(input->buffer_names[name_idx]->str, "smoke")==0)
            {
                smoke_idx = name_idx;
            }
            if (pnanovdb_reflect_string_compare(input->buffer_names[name_idx]->str, "temperature")==0)
            {
                temperature_idx = name_idx;
            }
        }
        if (smoke_idx >= input->buffer_count || temperature_idx >= input->buffer_count)
        {
            printf("Missing nanovdb buffer\n");
            return;
        }
        if (!input->buffer_buffers[smoke_idx] || !input->buffer_buffers[temperature_idx])
        {
            return;
        }

        if (ptr->shader_ctx)
        {
            if (pnanovdb_reflect_string_compare(ptr->shader_path, params->shader.shader_path) != 0)
            {
                compute->destroy_shader_context(compute, queue, ptr->shader_ctx);
                ptr->shader_ctx = nullptr;
                ptr->shader_path = nullptr;
            }
        }

        if (!ptr->shader_ctx && params)
        {
            pnanovdb_compiler_settings_t compile_settings = {};
            pnanovdb_compiler_settings_init(&compile_settings);

            ptr->shader_path = params->shader.shader_path;
            ptr->shader_ctx = compute->create_shader_context(params->shader.shader_path);
            if (compute->init_shader(compute, queue, ptr->shader_ctx, &compile_settings) == PNANOVDB_FALSE)
            {
                compute->destroy_shader_context(compute, queue, ptr->shader_ctx);
                ptr->shader_ctx = nullptr;
            }

            if (ptr->shader_ctx)
            {
                params->shader.params_data_type = nullptr;
                params->shader.params_metadata_type = nullptr;
                compute->shader_context_map_shader_params_type(ptr->shader_ctx, &params->shader.params_data_type, &params->shader.params_metadata_type);
                if (params->shader.params_data_type)
                {
                    ptr->shader_params_data.resize(params->shader.params_data_type->element_size);
                    params->shader.params_data = ptr->shader_params_data.data();
                    if (params->shader.params_data_type->default_value)
                    {
                        memcpy(params->shader.params_data,
                            params->shader.params_data_type->default_value,
                            params->shader.params_data_type->element_size);
                    }
                }
                if (params->shader.params_metadata_type)
                {
                    ptr->shader_params_metadata.resize(params->shader.params_metadata_type->element_size);
                    params->shader.params_metadata = ptr->shader_params_metadata.data();
                    if (params->shader.params_metadata_type->default_value)
                    {
                        memcpy(params->shader.params_metadata,
                            params->shader.params_metadata_type->default_value,
                            params->shader.params_metadata_type->element_size);
                    }
                }
            }
        }
        // need shader to proceed
        if (!ptr->shader_ctx)
        {
            return;
        }

        struct EditorParams
        {
            pnanovdb_camera_mat_t view_inv;
            pnanovdb_camera_mat_t projection_inv;

            pnanovdb_camera_mat_t view;
            pnanovdb_camera_mat_t projection;

            pnanovdb_uint32_t width;
            pnanovdb_uint32_t height;
            pnanovdb_uint32_t composite;
            pnanovdb_uint32_t pad2;
        };

        pnanovdb_camera_mat_t view_inv = pnanovdb_camera_mat_inverse(target->view);
        pnanovdb_camera_mat_t projection_inv = pnanovdb_camera_mat_inverse(target->projection);

        EditorParams editor_params = {};
        editor_params.view_inv = pnanovdb_camera_mat_transpose(view_inv);
        editor_params.projection_inv = pnanovdb_camera_mat_transpose(projection_inv);
        editor_params.view = pnanovdb_camera_mat_transpose(target->view);
        editor_params.projection = pnanovdb_camera_mat_transpose(target->projection);
        editor_params.width = target->width;
        editor_params.height = target->height;
        editor_params.composite = PNANOVDB_FALSE;

        pnanovdb_compute_buffer_desc_t buf_desc = {};
        buf_desc.usage = PNANOVDB_COMPUTE_BUFFER_USAGE_CONSTANT;
        buf_desc.format = PNANOVDB_COMPUTE_FORMAT_UNKNOWN;
        buf_desc.structure_stride = 0u;
        buf_desc.size_in_bytes = sizeof(editor_params);
        pnanovdb_compute_buffer_t* constant_buffer =
            compute_interface->create_buffer(context, PNANOVDB_COMPUTE_MEMORY_TYPE_UPLOAD, &buf_desc);
        // copy constants
        void* mapped_constants = compute_interface->map_buffer(context, constant_buffer);
        memcpy(mapped_constants, &editor_params, sizeof(editor_params));
        compute_interface->unmap_buffer(context, constant_buffer);
        pnanovdb_compute_buffer_transient_t* constant_transient =
            compute_interface->register_buffer_as_transient(context, constant_buffer);

        buf_desc.usage = PNANOVDB_COMPUTE_BUFFER_USAGE_CONSTANT;
        buf_desc.format = PNANOVDB_COMPUTE_FORMAT_UNKNOWN;
        buf_desc.structure_stride = 0u;
        buf_desc.size_in_bytes = ptr->shader_params_data.size();
        pnanovdb_compute_buffer_t* shader_params_buffer =
            compute_interface->create_buffer(context, PNANOVDB_COMPUTE_MEMORY_TYPE_UPLOAD, &buf_desc);
        // copy shader params
        void* mapped_shader_params = compute_interface->map_buffer(context, shader_params_buffer);
        memcpy(mapped_shader_params, ptr->shader_params_data.data(), ptr->shader_params_data.size());
        compute_interface->unmap_buffer(context, shader_params_buffer);
        pnanovdb_compute_buffer_transient_t* shader_params_transient =
            compute_interface->register_buffer_as_transient(context, shader_params_buffer);

        // legacy, keep small to save memory
        pnanovdb_compute_buffer_desc_t image_buf_desc = {};
        image_buf_desc.size_in_bytes = pnanovdb_uint64_t(65536u);
        image_buf_desc.usage = PNANOVDB_COMPUTE_BUFFER_USAGE_RW_STRUCTURED | PNANOVDB_COMPUTE_BUFFER_USAGE_COPY_SRC;
        image_buf_desc.structure_stride = 4u;
        auto* image_buffer = compute_interface->get_buffer_transient(context, &image_buf_desc);

        struct colormap_t
        {
            pnanovdb_uint32_t resolution;
            pnanovdb_uint32_t pad0;
            pnanovdb_uint32_t pad1;
            pnanovdb_uint32_t pad2;
            pnanovdb_vec4_t rgba[1024u];
        };
        colormap_t colormap = {};
        colormap.resolution = params->colormap_resolution;
        if (colormap.resolution == 0) {colormap.resolution = 1u;}
        if (colormap.resolution > 1024u) {colormap.resolution = 1024u;}
        for (pnanovdb_uint32_t idx = 0u; idx < colormap.resolution; idx++)
        {
            colormap.rgba[idx] = compute_texel(idx, colormap.resolution, params);
        }

        pnanovdb_compute_buffer_desc_t colormap_buf_desc = {};
        colormap_buf_desc.size_in_bytes = sizeof(colormap_t);
        colormap_buf_desc.usage = PNANOVDB_COMPUTE_BUFFER_USAGE_CONSTANT;
        colormap_buf_desc.structure_stride = 0u;
        auto* colormap_buffer = compute_interface->create_buffer(context, PNANOVDB_COMPUTE_MEMORY_TYPE_UPLOAD, &colormap_buf_desc);
        void* mapped_colormap = (void*)compute_interface->map_buffer(context, colormap_buffer);
        memcpy(mapped_colormap, &colormap, sizeof(colormap_t));
        compute_interface->unmap_buffer(context, colormap_buffer);
        pnanovdb_compute_buffer_transient_t* colormap_transient =
            compute_interface->register_buffer_as_transient(context, colormap_buffer);

        pnanovdb_compute_texture_desc_t tex_desc = {};
        tex_desc.texture_type = PNANOVDB_COMPUTE_TEXTURE_TYPE_2D;
        tex_desc.usage = PNANOVDB_COMPUTE_TEXTURE_USAGE_TEXTURE | PNANOVDB_COMPUTE_TEXTURE_USAGE_RW_TEXTURE;
        tex_desc.format = PNANOVDB_COMPUTE_FORMAT_R16G16B16A16_FLOAT;
        tex_desc.width = target->width;
        tex_desc.height = target->height;
        tex_desc.depth = 1u;
        tex_desc.mip_levels = 1u;
        pnanovdb_compute_texture_transient_t* color_hdr = compute_interface->get_texture_transient(context, &tex_desc);

        pnanovdb_compute_resource_t resources[7u] = {};
        resources[0u].buffer_transient = compute_interface->register_buffer_as_transient(context, input->buffer_buffers[smoke_idx]);
        resources[1u].buffer_transient = compute_interface->register_buffer_as_transient(context, input->buffer_buffers[temperature_idx]);
        resources[2u].buffer_transient = image_buffer;
        resources[3u].texture_transient = color_hdr;
        resources[4u].buffer_transient = constant_transient;
        resources[5u].buffer_transient = shader_params_transient;
        resources[6u].buffer_transient = colormap_transient;

        pnanovdb_compute_texture_transient_t* color_out = compute_interface->register_texture_as_transient(context, target->color);

        compute->dispatch_shader(compute_interface, context, ptr->shader_ctx, resources, (target->width + 15u) / 16u,
                                (target->height + 7u) / 8u, 1u, "flow_render");

        bloom_update(compute, queue, ptr->bloom_ctx, target->width, target->height, &params->bloom,
                     color_hdr, color_out);

        compute_interface->destroy_buffer(context, constant_buffer);
        compute_interface->destroy_buffer(context, shader_params_buffer);
        compute_interface->destroy_buffer(context, colormap_buffer);
    }

    static pnanovdb_pipeline_desc_t* get_desc()
    {
        static pnanovdb_pipeline_desc_t desc = {};
        desc.name = pnanovdb_get_compute()->get_token("flow_render");
        desc.get_pipeline_params_type = get_pipeline_params_type;
        desc.create = create;
        desc.destroy = destroy;
        desc.get_requirements = get_requirements;
        desc.render = render;
        return &desc;
    }

    enum bloom_shader
    {
        bloom_blur_x_slang,
        bloom_blur_y_slang,
        bloom_downsample_slang,
        bloom_tonemap_slang,

        bloom_shader_count
    };
    static const char* bloom_shader_names[bloom_shader_count] = {
        "editor/bloom_blur_x.slang",
        "editor/bloom_blur_y.slang",
        "editor/bloom_downsample.slang",
        "editor/bloom_tonemap.slang"
    };
    struct bloom_ctx_t
    {
        pnanovdb_shader_context_t* shader_ctx[bloom_shader_count] = {};
        pnanovdb_compute_sampler_t* sampler_linear_clamp = nullptr;
        pnanovdb_compute_sampler_t* sampler_linear_border = nullptr;
    };
    bloom_ctx_t* bloom_create(const pnanovdb_compute_t* compute, pnanovdb_compute_queue_t* queue)
    {
        pnanovdb_compute_interface_t* compute_interface = compute->device_interface.get_compute_interface(queue);
        pnanovdb_compute_context_t* context = compute->device_interface.get_compute_context(queue);

        auto ptr = new bloom_ctx_t();

        pnanovdb_compiler_settings_t compile_settings = {};
        pnanovdb_compiler_settings_init(&compile_settings);
        for (pnanovdb_uint32_t idx = 0u; idx < bloom_shader_count; idx++)
        {
            ptr->shader_ctx[idx] = compute->create_shader_context(bloom_shader_names[idx]);
            if (compute->init_shader(compute, queue, ptr->shader_ctx[idx], &compile_settings) == PNANOVDB_FALSE)
            {
                compute->destroy_shader_context(compute, queue, ptr->shader_ctx[idx]);
                ptr->shader_ctx[idx] = nullptr;
            }
        }
        pnanovdb_compute_sampler_desc_t samplerDesc = {};
        samplerDesc.filter_mode = PNANOVDB_COMPUTE_SAMPLER_FILTER_MODE_LINEAR;
        samplerDesc.address_mode_u = PNANOVDB_COMPUTE_SAMPLER_ADDRESS_MODE_CLAMP;
        samplerDesc.address_mode_v = PNANOVDB_COMPUTE_SAMPLER_ADDRESS_MODE_CLAMP;
        samplerDesc.address_mode_w = PNANOVDB_COMPUTE_SAMPLER_ADDRESS_MODE_CLAMP;
        ptr->sampler_linear_clamp = compute_interface->create_sampler(context, &samplerDesc);
        samplerDesc.address_mode_u = PNANOVDB_COMPUTE_SAMPLER_ADDRESS_MODE_BORDER;
        samplerDesc.address_mode_v = PNANOVDB_COMPUTE_SAMPLER_ADDRESS_MODE_BORDER;
        samplerDesc.address_mode_w = PNANOVDB_COMPUTE_SAMPLER_ADDRESS_MODE_BORDER;
        ptr->sampler_linear_border = compute_interface->create_sampler(context, &samplerDesc);
        return ptr;
    }
    void bloom_destroy(const pnanovdb_compute_t* compute, pnanovdb_compute_queue_t* queue, bloom_ctx_t* ptr)
    {
        pnanovdb_compute_interface_t* compute_interface = compute->device_interface.get_compute_interface(queue);
        pnanovdb_compute_context_t* context = compute->device_interface.get_compute_context(queue);
        for (pnanovdb_uint32_t idx = 0u; idx < bloom_shader_count; idx++)
        {
            if (ptr->shader_ctx[idx])
            {
                compute->destroy_shader_context(compute, queue, ptr->shader_ctx[idx]);
                ptr->shader_ctx[idx] = nullptr;
            }
        }
        compute_interface->destroy_sampler(context, ptr->sampler_linear_clamp);
        compute_interface->destroy_sampler(context, ptr->sampler_linear_border);
        delete ptr;
    }
    pnanovdb_compute_buffer_t* bloom_update_constants(
        pnanovdb_compute_interface_t* compute_interface,
        pnanovdb_compute_context_t* context,
        float weightScale, float sizeScale,
        pnanovdb_uint32_t width1, pnanovdb_uint32_t height1,
        pnanovdb_uint32_t width, pnanovdb_uint32_t height,
        pnanovdb_uint32_t mode, float threshold,
        float weight, float size, pnanovdb_vec3_t colorScale)
    {
        float width1Inv = 1.f / float(width1);
        float weights[17u];
        float sum = 0.f;
        float k = 1.f / (2.f * sizeScale * sizeScale * size * size);
        for (int weightIdx = 0; weightIdx < 17u; weightIdx++)
        {
            float normOffset = float(weightIdx) * width1Inv;

            weights[weightIdx] = expf(-k * normOffset * normOffset);

            sum += weights[weightIdx];
        }
        float sumInv = 1.f / sum;
        for (int weightIdx = 0; weightIdx < 17u; weightIdx++)
        {
            weights[weightIdx] *= sumInv;
        }
        struct constants_t
        {
            pnanovdb_uint32_t mode;
            float weight;
            float threshold;
            float size;
            pnanovdb_vec4_t texDimInv;
            pnanovdb_vec4_t centerWeight;
            pnanovdb_vec4_t weights[4u];
            pnanovdb_vec4_t colorScale;
        };
        constants_t constants = {};
        constants.mode = mode;
        constants.weight = weightScale * weight;
        constants.threshold = threshold;
        constants.size = sizeScale * size;
        constants.texDimInv = pnanovdb_vec4_t{ 1.f / float(width1), 1.f / float(height1), 1.f / float(width), 1.f / float(height) };
        constants.centerWeight = pnanovdb_vec4_t{ weights[0u], weights[0u], weights[0u], weights[0u] };
        constants.weights[0u] = pnanovdb_vec4_t{ weights[1u], weights[2u], weights[3u], weights[4u] };
        constants.weights[1u] = pnanovdb_vec4_t{ weights[5u], weights[6u], weights[7u], weights[8u] };
        constants.weights[2u] = pnanovdb_vec4_t{ weights[9u], weights[10u], weights[11u], weights[12u] };
        constants.weights[3u] = pnanovdb_vec4_t{ weights[13u], weights[14u], weights[15u], weights[16u] };
        constants.colorScale = pnanovdb_vec4_t{ colorScale.x, colorScale.y, colorScale.z, 0.f };

        // constants
        pnanovdb_compute_buffer_desc_t buf_desc = {};
        buf_desc.usage = PNANOVDB_COMPUTE_BUFFER_USAGE_CONSTANT;
        buf_desc.format = PNANOVDB_COMPUTE_FORMAT_UNKNOWN;
        buf_desc.structure_stride = 0u;
        buf_desc.size_in_bytes = sizeof(constants_t);
        pnanovdb_compute_buffer_t* constant_buffer =
            compute_interface->create_buffer(context, PNANOVDB_COMPUTE_MEMORY_TYPE_UPLOAD, &buf_desc);

        // copy constants
        void* mapped_constants = compute_interface->map_buffer(context, constant_buffer);
        memcpy(mapped_constants, &constants, sizeof(constants_t));
        compute_interface->unmap_buffer(context, constant_buffer);

        return constant_buffer;
    }
    void bloom_update(const pnanovdb_compute_t* compute, pnanovdb_compute_queue_t* queue, bloom_ctx_t* ptr,
                      int width, int height, const bloom_params_t* params,
                      pnanovdb_compute_texture_transient_t* image_in,
                      pnanovdb_compute_texture_transient_t* image_out)
    {
        pnanovdb_compute_interface_t* compute_interface = compute->device_interface.get_compute_interface(queue);
        pnanovdb_compute_context_t* context = compute->device_interface.get_compute_context(queue);

        pnanovdb_vec3_t colorScale = params->colorScale;
        float weightScale = params->weightScale;
        float sizeScale = params->sizeScale;
        float threshold = params->threshold;
        static const pnanovdb_uint32_t level_count = 5;
        float weights[level_count] = {params->weight1, params->weight2, params->weight3, params->weight4, params->weight5};
        float sizes[level_count] = {params->size1, params->size2, params->size3, params->size4, params->size5};

        pnanovdb_compute_buffer_t* constant_buffer[level_count] = {};
        pnanovdb_compute_buffer_transient_t* constant_buffer_transient[level_count] = {};
        pnanovdb_int32_t widths[level_count] = {};
        pnanovdb_int32_t heights[level_count] = {};
        for (pnanovdb_uint32_t level_idx = 0u; level_idx < level_count; level_idx++)
        {
            widths[level_idx] = ((level_idx == 0u ? width : widths[level_idx - 1u]) + 1u) / 2u;
            heights[level_idx] = ((level_idx == 0u ? height : heights[level_idx - 1u]) + 1u) / 2u;

            float threshold_level = level_idx == 0u ? threshold : 0.f;
            int width_fine = level_idx == 0u ? width : widths[level_idx - 1u];
            int height_fine = level_idx == 0u ? height : heights[level_idx - 1u];
            int width_coarse = widths[level_idx];
            int height_coarse = heights[level_idx];

	        constant_buffer[level_idx] = bloom_update_constants(
                compute_interface, context,
                weightScale, sizeScale,
                widths[level_idx], heights[level_idx],
                width_fine, height_fine, level_idx, threshold_level,
                weights[level_idx], sizes[level_idx], colorScale);

            constant_buffer_transient[level_idx] =
                compute_interface->register_buffer_as_transient(context, constant_buffer[level_idx]);
        }

        // downsample pass
        pnanovdb_compute_texture_transient_t* downsample_tex[level_count] = {};
        pnanovdb_compute_texture_transient_t* temp_tex[level_count] = {};
        for (pnanovdb_uint32_t level_idx = 0u; level_idx < level_count; level_idx++)
        {
            pnanovdb_compute_texture_transient_t* src = level_idx == 0u ? image_in : downsample_tex[level_idx - 1u];
            pnanovdb_compute_texture_desc_t tex_desc = {};
            tex_desc.texture_type = PNANOVDB_COMPUTE_TEXTURE_TYPE_2D;
            tex_desc.usage = PNANOVDB_COMPUTE_TEXTURE_USAGE_TEXTURE | PNANOVDB_COMPUTE_TEXTURE_USAGE_RW_TEXTURE;
            tex_desc.format = PNANOVDB_COMPUTE_FORMAT_R16G16B16A16_FLOAT;
            tex_desc.width = widths[level_idx];
            tex_desc.height = heights[level_idx];
            tex_desc.depth = 1u;
            tex_desc.mip_levels = 1u;
            pnanovdb_compute_texture_transient_t* dst = compute_interface->get_texture_transient(context, &tex_desc);
            temp_tex[level_idx] = compute_interface->get_texture_transient(context, &tex_desc);

            pnanovdb_compute_resource_t resources[4u] = {};
            resources[0u].buffer_transient = constant_buffer_transient[level_idx];
            resources[1u].texture_transient = src;
            resources[2u].texture_transient = dst;
            resources[3u].sampler = ptr->sampler_linear_clamp;

            pnanovdb_uint32_t grid_dim_x = (widths[level_idx] + 15u) / 16u;
            pnanovdb_uint32_t grid_dim_y = (heights[level_idx] + 15u) / 16u;
            compute->dispatch_shader(compute_interface, context, ptr->shader_ctx[bloom_downsample_slang],
                                     resources, grid_dim_x, grid_dim_y, 1u, "bloom_downsample");

            downsample_tex[level_idx] = dst;
        }

        // blur pass
        for (pnanovdb_uint32_t level_idx = level_count - 1u; level_idx < level_count; level_idx--)
        {
            pnanovdb_compute_texture_transient_t* src = downsample_tex[level_idx];
            pnanovdb_compute_texture_transient_t* dst = temp_tex[level_idx];
            {
                pnanovdb_compute_resource_t resources[3u] = {};
                resources[0u].buffer_transient = constant_buffer_transient[level_idx];
                resources[1u].texture_transient = src;
                resources[2u].texture_transient = dst;

                pnanovdb_uint32_t grid_dim_x = (widths[level_idx] + 15u) / 16u;
                pnanovdb_uint32_t grid_dim_y = (heights[level_idx] + 15u) / 16u;
                compute->dispatch_shader(compute_interface, context, ptr->shader_ctx[bloom_blur_x_slang],
                                        resources, grid_dim_x, grid_dim_y, 1u, "bloom_blur_x");
            }
            {
                pnanovdb_compute_resource_t resources[5u] = {};
                resources[0u].buffer_transient = constant_buffer_transient[level_idx];
                resources[1u].texture_transient = dst;
                resources[2u].texture_transient = src;
                resources[3u].texture_transient = level_idx == level_count - 1u ? dst : downsample_tex[level_idx + 1u];
                resources[4u].sampler = ptr->sampler_linear_border;

                pnanovdb_uint32_t grid_dim_x = (widths[level_idx] + 15u) / 16u;
                pnanovdb_uint32_t grid_dim_y = (heights[level_idx] + 15u) / 16u;
                compute->dispatch_shader(compute_interface, context, ptr->shader_ctx[bloom_blur_y_slang],
                                        resources, grid_dim_x, grid_dim_y, 1u, "bloom_blur_y");
            }
        }

        // tonemap and composite to target
        {
            pnanovdb_compute_resource_t resources[5u] = {};
            resources[0u].buffer_transient = constant_buffer_transient[0u];
            resources[1u].texture_transient = image_in;
            resources[2u].texture_transient = image_out;
            resources[3u].texture_transient = downsample_tex[0u];
            resources[4u].sampler = ptr->sampler_linear_clamp;

            pnanovdb_uint32_t grid_dim_x = (width + 15u) / 16u;
            pnanovdb_uint32_t grid_dim_y = (height + 15u) / 16u;
            compute->dispatch_shader(compute_interface, context, ptr->shader_ctx[bloom_tonemap_slang],
                                     resources, grid_dim_x, grid_dim_y, 1u, "bloom_tonemap");
        }

        for (pnanovdb_uint32_t level_idx = 0u; level_idx < level_count; level_idx++)
        {
            compute_interface->destroy_buffer(context, constant_buffer[level_idx]);
        }
    }
}

void pnanovdb_pipeline_register_flow(const pnanovdb_compute_t* compute,
                                     const pnanovdb_compiler_t* compiler,
                                     pnanovdb_pipeline_registry_t* registry)
{
    pnanovdb_get_pipeline()->register_pipeline(registry, pnanovdb_pipeline_flow_render::get_desc());
}
