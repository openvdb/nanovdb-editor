// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   compute/DefaultPipelines.cpp

    \author Andrew Reidmeyer

    \brief  This file provides NanoVDB related pipelines
*/

#include "nanovdb_editor/putil/Compute.h"
#include "nanovdb_editor/putil/Pipeline.h"
#include "nanovdb_editor/putil/Camera.h"
#include "nanovdb_editor/putil/Reflect.h"

#include <vector>

#include <nanovdb/GridHandle.h>
#include <nanovdb/HostBuffer.h>
#include <nanovdb/tools/CreateNanoGrid.h>

namespace pnanovdb_pipeline_nanovdb_load
{
    struct Instance
    {
        pnanovdb_uint64_t key_size = 0llu;
        pnanovdb_uint64_t key_version = 0llu;
        pnanovdb_compute_token_t* key_path = nullptr;
        pnanovdb_bool_t key_convert_to_half = PNANOVDB_FALSE;
        pnanovdb_compute_buffer_t* nanovdb_buffer = 0llu;
    };
    PNANOVDB_CAST_PAIR(Instance, pnanovdb_pipeline_instance_t)

    struct pnanovdb_pipeline_nanovdb_load_params_t
    {
        const char* buffer_name;
        pnanovdb_bool_t convert_to_half;
    };
    static const pnanovdb_pipeline_nanovdb_load_params_t pnanovdb_pipeline_nanovdb_load_params_t_default = {
        "nanovdb",
        PNANOVDB_FALSE // enabled
    };
    #define PNANOVDB_REFLECT_TYPE pnanovdb_pipeline_nanovdb_load_params_t
    PNANOVDB_REFLECT_BEGIN()
    PNANOVDB_REFLECT_POINTER(char, buffer_name, 0, 0)
    PNANOVDB_REFLECT_VALUE(pnanovdb_bool_t, convert_to_half, 0, 0)
    PNANOVDB_REFLECT_END(&pnanovdb_pipeline_nanovdb_load_params_t_default)
    #undef PNANOVDB_REFLECT_TYPE

    void get_pipeline_params_type(const pnanovdb_compute_t* compute,
                                  const pnanovdb_compiler_t* compiler,
                                  const pnanovdb_reflect_data_type_t** out_data_type,
                                  const pnanovdb_reflect_data_type_t** out_metadata_type)
    {
        *out_data_type = PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_pipeline_nanovdb_load_params_t);
        *out_metadata_type = nullptr;
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
        if (ptr->nanovdb_buffer)
        {
            compute_interface->destroy_buffer(context, ptr->nanovdb_buffer);
            ptr->nanovdb_buffer = nullptr;
        }
        delete ptr;
    }

    pnanovdb_bool_t get_requirements(pnanovdb_pipeline_requirements_params_t* params,
                                     pnanovdb_pipeline_requirements_output_t* output)
    {
        if (!params->params_data || !params->params_data_type ||
            !pnanovdb_reflect_layout_compare(params->params_data_type, PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_pipeline_nanovdb_load_params_t)))
        {
            *output->p_name_count = 0u;
            return PNANOVDB_TRUE;
        }
        if (!output->names)
        {
            *output->p_name_count = 1u;
            return PNANOVDB_FALSE;
        }
        auto ptr = (pnanovdb_pipeline_nanovdb_load_params_t*)params->params_data;
        if (*output->p_name_count >= 1u) { output->names[0u] = ptr->buffer_name; }
        if (*output->p_name_count >= 1u)
        {
            *output->p_name_count = 1u;
            return PNANOVDB_TRUE;
        }
        return PNANOVDB_FALSE;
    }

    void load(pnanovdb_pipeline_instance_t* instance,
              const pnanovdb_compute_t* compute,
              pnanovdb_compute_queue_t* queue,
              pnanovdb_pipeline_load_input_t* input,
              pnanovdb_pipeline_output_t* output)
    {
        auto ptr = cast(instance);

        pnanovdb_compute_interface_t* compute_interface = compute->device_interface.get_compute_interface(queue);
        pnanovdb_compute_context_t* context = compute->device_interface.get_compute_context(queue);

        if (!input->params_data || !input->params_data_type ||
            !pnanovdb_reflect_layout_compare(input->params_data_type, PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_pipeline_nanovdb_load_params_t)))
        {
            return;
        }
        auto params = (pnanovdb_pipeline_nanovdb_load_params_t*)input->params_data;

        const char* buffer_name = "nanovdb";
        if (params->buffer_name)
        {
            buffer_name = params->buffer_name;
        }

        pnanovdb_uint64_t name_idx = 0u;
        for (;name_idx < input->buffer_count;name_idx++)
        {
            if (pnanovdb_reflect_string_compare(input->buffer_names[name_idx]->str, buffer_name)==0)
            {
                break;
            }
        }
        if (name_idx >= input->buffer_count)
        {
            printf("Missing nanovdb buffer\n");
            return;
        }

        if (input->buffer_array_versions[name_idx] != ptr->key_version ||
            input->buffer_paths[name_idx] != ptr->key_path ||
            params->convert_to_half != ptr->key_convert_to_half)
        {
            ptr->key_version = input->buffer_array_versions[name_idx];
            ptr->key_path = input->buffer_paths[name_idx];
            ptr->key_convert_to_half = params->convert_to_half;
            ptr->key_size = 0llu;
            if (input->buffer_arrays[name_idx])
            {
                ptr->key_size = input->buffer_arrays[name_idx]->element_size * input->buffer_arrays[name_idx]->element_count;
            }

            if (ptr->nanovdb_buffer)
            {
                compute_interface->destroy_buffer(context, ptr->nanovdb_buffer);
                ptr->nanovdb_buffer = nullptr;
            }

            pnanovdb_uint64_t res_element_size = input->buffer_arrays[name_idx] ? input->buffer_arrays[name_idx]->element_size : 0llu;
            pnanovdb_uint64_t res_size = ptr->key_size;
            void* res_data = input->buffer_arrays[name_idx] ? input->buffer_arrays[name_idx]->data : nullptr;

            pnanovdb_compute_array_t* array_from_path = nullptr;
            if (ptr->key_path)
            {
                array_from_path = compute->load_nanovdb(ptr->key_path->str);
                if (array_from_path)
                {
                    res_element_size = array_from_path->element_size;
                    res_size = res_element_size * array_from_path->element_count;
                    res_data = array_from_path->data;
                }
            }

            std::vector<uint8_t> half_data;
            if (params->convert_to_half)
            {
                auto buffer = nanovdb::HostBuffer::createFull(res_size, res_data);
                auto handleSrc = nanovdb::GridHandle<nanovdb::HostBuffer>(std::move(buffer));
                auto *nanoGridSrc = handleSrc.grid<float>();

                auto handle = nanovdb::tools::createNanoGrid<nanovdb::FloatGrid, nanovdb::Half>(*nanoGridSrc);
                auto *nanoGrid = handle.grid<nanovdb::Half>();

                half_data.assign((uint8_t*)handle.data(), (uint8_t*)handle.data() + handle.bufferSize());

                printf("Did conversion!!!!\n");

                res_size = half_data.size();
                res_data = half_data.data();
            }

            static const pnanovdb_uint64_t min_size = 65536u;
            pnanovdb_uint64_t buf_size = res_size < min_size ? min_size : res_size;

            pnanovdb_compute_buffer_desc_t buf_desc = {};
            buf_desc.usage = PNANOVDB_COMPUTE_BUFFER_USAGE_COPY_SRC;
            buf_desc.format = PNANOVDB_COMPUTE_FORMAT_UNKNOWN;
            buf_desc.structure_stride = 0;
            buf_desc.size_in_bytes = buf_size;
            pnanovdb_compute_buffer_t* upload_buffer =
                compute_interface->create_buffer(context, PNANOVDB_COMPUTE_MEMORY_TYPE_UPLOAD, &buf_desc);

            buf_desc.usage = PNANOVDB_COMPUTE_BUFFER_USAGE_STRUCTURED | PNANOVDB_COMPUTE_BUFFER_USAGE_RW_STRUCTURED |
                PNANOVDB_COMPUTE_BUFFER_USAGE_COPY_SRC | PNANOVDB_COMPUTE_BUFFER_USAGE_COPY_DST;
            buf_desc.format = PNANOVDB_COMPUTE_FORMAT_UNKNOWN;
            buf_desc.structure_stride = res_element_size;
            buf_desc.size_in_bytes = buf_size;
            ptr->nanovdb_buffer =
                compute_interface->create_buffer(context, PNANOVDB_COMPUTE_MEMORY_TYPE_DEVICE, &buf_desc);

            void* mapped_upload = compute_interface->map_buffer(context, upload_buffer);
            if (res_size < min_size)
            {
                memset(mapped_upload, 0, min_size);
            }
            memcpy(mapped_upload, res_data, res_size);
            compute_interface->unmap_buffer(context, upload_buffer);

            pnanovdb_compute_copy_buffer_params_t copy_params = {};
            copy_params.num_bytes = buf_size;
            copy_params.src = compute_interface->register_buffer_as_transient(context, upload_buffer);
            copy_params.dst = compute_interface->register_buffer_as_transient(context, ptr->nanovdb_buffer);
            copy_params.debug_label = "pipeline_nanovdb_load";
            compute_interface->copy_buffer(context, &copy_params);

            compute_interface->destroy_buffer(context, upload_buffer);

            if (array_from_path)
            {
                compute->destroy_array(array_from_path);
            }
        }
        output->buffer_buffers[name_idx] = ptr->nanovdb_buffer;
        output->buffer_sizes[name_idx] = ptr->key_size;
        output->buffer_versions[name_idx] = ptr->key_version;
    }

    static pnanovdb_pipeline_desc_t* get_desc()
    {
        static pnanovdb_pipeline_desc_t desc = {};
        desc.name = pnanovdb_get_compute()->get_token("nanovdb_load");
        desc.get_pipeline_params_type = get_pipeline_params_type;
        desc.create = create;
        desc.destroy = destroy;
        desc.get_requirements = get_requirements;
        desc.load = load;
        return &desc;
    }
}

namespace pnanovdb_pipeline_nanovdb_render
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
        const char* buffer_name;
        pnanovdb_pipeline_shader_t shader;
    };
    static const pnanovdb_pipeline_nanovdb_render_params_t pnanovdb_pipeline_nanovdb_render_params_t_default = {
        "nanovdb",
        {"editor/editor.slang", nullptr, nullptr, nullptr, nullptr} // shader
    };
    #define PNANOVDB_REFLECT_TYPE pnanovdb_pipeline_nanovdb_render_params_t
    PNANOVDB_REFLECT_BEGIN()
    PNANOVDB_REFLECT_POINTER(char, buffer_name, 0, 0)
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
        "editor/editor.slang",
        "editor/editor_surface.slang",
        "editor/wireframe.slang",
        "editor/gaussian.slang",
        "editor/icosahedron.slang"
    };
    static const pnanovdb_pipeline_nanovdb_render_metadata_t pnanovdb_pipeline_nanovdb_render_metadata_t_default = {
        {shader_names, 5u} // shader
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
            *output->p_name_count = 1u;
            return PNANOVDB_FALSE;
        }
        auto ptr = (pnanovdb_pipeline_nanovdb_render_params_t*)params->params_data;
        if (*output->p_name_count >= 1u) { output->names[0u] = ptr->buffer_name; }
        if (*output->p_name_count >= 1u)
        {
            *output->p_name_count = 1u;
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

        const char* buffer_name = "nanovdb";
        if (params->buffer_name)
        {
            buffer_name = params->buffer_name;
        }

        pnanovdb_uint64_t name_idx = 0u;
        for (;name_idx < input->buffer_count;name_idx++)
        {
            if (pnanovdb_reflect_string_compare(input->buffer_names[name_idx]->str, buffer_name)==0)
            {
                break;
            }
        }
        if (name_idx >= input->buffer_count)
        {
            printf("Missing nanovdb buffer\n");
            return;
        }
        if (!input->buffer_buffers[name_idx])
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

        pnanovdb_compute_resource_t resources[5u] = {};
        resources[0u].buffer_transient = compute_interface->register_buffer_as_transient(context, input->buffer_buffers[name_idx]);
        resources[1u].buffer_transient = image_buffer;
        resources[2u].texture_transient = compute_interface->register_texture_as_transient(context, target->color);
        resources[3u].buffer_transient = constant_transient;
        resources[4u].buffer_transient = shader_params_transient;

        compute->dispatch_shader(compute_interface, context, ptr->shader_ctx, resources, (target->width + 15u) / 16u,
                                (target->height + 7u) / 8u, 1u, "nanovdb_render");

        compute_interface->destroy_buffer(context, constant_buffer);
        compute_interface->destroy_buffer(context, shader_params_buffer);
    }

    static pnanovdb_pipeline_desc_t* get_desc()
    {
        static pnanovdb_pipeline_desc_t desc = {};
        desc.name = pnanovdb_get_compute()->get_token("nanovdb_render");
        desc.get_pipeline_params_type = get_pipeline_params_type;
        desc.create = create;
        desc.destroy = destroy;
        desc.get_requirements = get_requirements;
        desc.render = render;
        return &desc;
    }
}

namespace pnanovdb_pipeline_no_operation
{
    static pnanovdb_pipeline_desc_t* get_desc()
    {
        static pnanovdb_pipeline_desc_t desc = {};
        desc.name = pnanovdb_get_compute()->get_token("no_operation");
        return &desc;
    }
}

void pnanovdb_pipeline_register_defaults(const pnanovdb_compute_t* compute,
                                         const pnanovdb_compiler_t* compiler,
                                         pnanovdb_pipeline_registry_t* registry)
{
    pnanovdb_get_pipeline()->register_pipeline(registry, pnanovdb_pipeline_no_operation::get_desc());
    pnanovdb_get_pipeline()->register_pipeline(registry, pnanovdb_pipeline_nanovdb_load::get_desc());
    pnanovdb_get_pipeline()->register_pipeline(registry, pnanovdb_pipeline_nanovdb_render::get_desc());
}
