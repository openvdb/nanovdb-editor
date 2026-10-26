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
    struct Instance
    {
        pnanovdb_shader_context_t* shader_ctx = nullptr;
        const char* shader_path = nullptr;
        std::vector<uint8_t> shader_params_data;
        std::vector<uint8_t> shader_params_metadata;
    };
    PNANOVDB_CAST_PAIR(Instance, pnanovdb_pipeline_instance_t)

    struct pnanovdb_pipeline_nanovdb_render_params_t
    {
        pnanovdb_pipeline_shader_t shader;
    };
    static const pnanovdb_pipeline_nanovdb_render_params_t pnanovdb_pipeline_nanovdb_render_params_t_default = {
        {"editor/flow_raymarch.slang", nullptr, nullptr, nullptr, nullptr} // shader
    };
    #define PNANOVDB_REFLECT_TYPE pnanovdb_pipeline_nanovdb_render_params_t
    PNANOVDB_REFLECT_BEGIN()
    PNANOVDB_REFLECT_VALUE(pnanovdb_pipeline_shader_t, shader, 0, 0)
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

        return cast(ptr);
    }

    void destroy(const pnanovdb_compute_t* compute,
                 pnanovdb_compute_queue_t* queue,
                 pnanovdb_pipeline_instance_t* instance)
    {
        auto ptr = cast(instance);

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

        pnanovdb_compute_resource_t resources[6u] = {};
        resources[0u].buffer_transient = compute_interface->register_buffer_as_transient(context, input->buffer_buffers[smoke_idx]);
        resources[1u].buffer_transient = compute_interface->register_buffer_as_transient(context, input->buffer_buffers[temperature_idx]);
        resources[2u].buffer_transient = image_buffer;
        resources[3u].texture_transient = compute_interface->register_texture_as_transient(context, target->color);
        resources[4u].buffer_transient = constant_transient;
        resources[5u].buffer_transient = shader_params_transient;

        compute->dispatch_shader(compute_interface, context, ptr->shader_ctx, resources, (target->width + 15u) / 16u,
                                (target->height + 7u) / 8u, 1u, "flow_render");

        compute_interface->destroy_buffer(context, constant_buffer);
        compute_interface->destroy_buffer(context, shader_params_buffer);
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
}

void pnanovdb_pipeline_register_flow(const pnanovdb_compute_t* compute,
                                     const pnanovdb_compiler_t* compiler,
                                     pnanovdb_pipeline_registry_t* registry)
{
    pnanovdb_get_pipeline()->register_pipeline(registry, pnanovdb_pipeline_flow_render::get_desc());
}
