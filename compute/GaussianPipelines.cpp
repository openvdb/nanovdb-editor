// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   compute/GaussianPipelines.cpp

    \author Andrew Reidmeyer

    \brief  This file provides gaussian related pipelines
*/

#include "nanovdb_editor/putil/Compute.h"
#include "nanovdb_editor/putil/Pipeline.h"
#include "nanovdb_editor/putil/Camera.h"
#include "nanovdb_editor/putil/Reflect.h"
#include "nanovdb_editor/putil/FileFormat.h"
#include "nanovdb_editor/putil/Raster.h"

#include <vector>

#include <nanovdb/GridHandle.h>
#include <nanovdb/HostBuffer.h>
#include <nanovdb/tools/CreateNanoGrid.h>

namespace pnanovdb_pipeline_gaussian_load
{
    struct Instance
    {
        pnanovdb_uint64_t key_size = 0llu;
        pnanovdb_uint64_t key_version = 0llu;
        pnanovdb_compute_token_t* key_path = nullptr;

        pnanovdb_compute_buffer_t* means_buffer = nullptr;
        pnanovdb_compute_buffer_t* opacities_buffer = nullptr;
        pnanovdb_compute_buffer_t* quaternions_buffer = nullptr;
        pnanovdb_compute_buffer_t* scales_buffer = nullptr;
        pnanovdb_compute_buffer_t* sh_0_buffer = nullptr;
        pnanovdb_compute_buffer_t* sh_n_buffer = nullptr;

        pnanovdb_uint64_t buffer_sizes[7u] = {};

        pnanovdb_compute_buffer_t* color_buffer = nullptr;
    };
    PNANOVDB_CAST_PAIR(Instance, pnanovdb_pipeline_instance_t)

    struct pnanovdb_pipeline_gaussian_load_params_t
    {
        pnanovdb_uint64_t unused;
    };
    static const pnanovdb_pipeline_gaussian_load_params_t pnanovdb_pipeline_gaussian_load_params_t_default = {
        0u // unused
    };
    #define PNANOVDB_REFLECT_TYPE pnanovdb_pipeline_gaussian_load_params_t
    PNANOVDB_REFLECT_BEGIN()
    PNANOVDB_REFLECT_END(&pnanovdb_pipeline_gaussian_load_params_t_default)
    #undef PNANOVDB_REFLECT_TYPE

    void get_pipeline_params_type(const pnanovdb_compute_t* compute,
                                  const pnanovdb_compiler_t* compiler,
                                  const pnanovdb_reflect_data_type_t** out_data_type,
                                  const pnanovdb_reflect_data_type_t** out_metadata_type)
    {
        *out_data_type = PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_pipeline_gaussian_load_params_t);
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
        pnanovdb_compute_buffer_t** buffers[7u] = {&ptr->means_buffer, &ptr->opacities_buffer, &ptr->quaternions_buffer,
            &ptr->scales_buffer, &ptr->sh_0_buffer, &ptr->sh_n_buffer, &ptr->color_buffer};
        for (pnanovdb_uint32_t idx = 0u; idx < 7u; idx++)
        {
            if (*buffers[idx])
            {
                compute_interface->destroy_buffer(context, *buffers[idx]);
                *buffers[idx] = nullptr;
            }
        }
        delete ptr;
    }

    pnanovdb_bool_t get_requirements(pnanovdb_pipeline_requirements_params_t* params,
                                     pnanovdb_pipeline_requirements_output_t* output)
    {
        if (!output->names)
        {
            *output->p_name_count = 8u;
            return PNANOVDB_FALSE;
        }
        if (*output->p_name_count >= 1u) {output->names[0u] = "ply";}
        if (*output->p_name_count >= 2u) {output->names[1u] = "means";}
        if (*output->p_name_count >= 3u) {output->names[2u] = "opacities";}
        if (*output->p_name_count >= 4u) {output->names[3u] = "quaternions";}
        if (*output->p_name_count >= 5u) {output->names[4u] = "scales";}
        if (*output->p_name_count >= 6u) {output->names[5u] = "sh_0";}
        if (*output->p_name_count >= 7u) {output->names[6u] = "sh_n";}
        if (*output->p_name_count >= 8u) {output->names[7u] = "colors";}
        if (*output->p_name_count >= 8u)
        {
            *output->p_name_count = 8u;
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

        pnanovdb_uint64_t name_idx = 0u;
        for (;name_idx < input->buffer_count;name_idx++)
        {
            if (pnanovdb_reflect_string_compare(input->buffer_names[name_idx]->str, "ply")==0)
            {
                break;
            }
        }
        if (name_idx >= input->buffer_count)
        {
            printf("Missing ply buffer\n");
        }

        if (input->buffer_array_versions[name_idx] != ptr->key_version ||
            input->buffer_paths[name_idx] != ptr->key_path)
        {
            ptr->key_version = input->buffer_array_versions[name_idx];
            ptr->key_path = input->buffer_paths[name_idx];
            ptr->key_size = 0llu;
            if (input->buffer_arrays[name_idx])
            {
                ptr->key_size = input->buffer_arrays[name_idx]->element_size * input->buffer_arrays[name_idx]->element_count;
            }

            pnanovdb_compute_buffer_t** buffers[7u] = {&ptr->means_buffer, &ptr->opacities_buffer, &ptr->quaternions_buffer,
                &ptr->scales_buffer, &ptr->sh_0_buffer, &ptr->sh_n_buffer, &ptr->color_buffer};
            for (pnanovdb_uint32_t idx = 0u; idx < 7u; idx++)
            {
                if (*buffers[idx])
                {
                    compute_interface->destroy_buffer(context, *buffers[idx]);
                    *buffers[idx] = nullptr;
                }
            }

            pnanovdb_compute_array_t* cpu_means = nullptr;
            pnanovdb_compute_array_t* cpu_opacities = nullptr;
            pnanovdb_compute_array_t* cpu_quaternions = nullptr;
            pnanovdb_compute_array_t* cpu_scales = nullptr;
            pnanovdb_compute_array_t* cpu_sh_0 = nullptr;
            pnanovdb_compute_array_t* cpu_sh_n = nullptr;
            if (ptr->key_path)
            {
                pnanovdb_fileformat_t fileformat = {};
                pnanovdb_fileformat_load(&fileformat, compute);

                const char* array_names_gaussian[] = { "means", "opacities", "quaternions", "scales", "sh_0", "sh_n" };
                pnanovdb_compute_array_t* arrays_gaussian[6] = {};

                pnanovdb_bool_t loaded_gaussian = fileformat.load_file(ptr->key_path->str, 6, array_names_gaussian, arrays_gaussian);
                if (loaded_gaussian == PNANOVDB_TRUE)
                {
                    cpu_means = arrays_gaussian[0];
                    cpu_opacities = arrays_gaussian[1];
                    cpu_quaternions = arrays_gaussian[2];
                    cpu_scales = arrays_gaussian[3];
                    cpu_sh_0 = arrays_gaussian[4];
                    cpu_sh_n = arrays_gaussian[5];
                }
                pnanovdb_fileformat_free(&fileformat);
            }
            if (cpu_means)
            {
                pnanovdb_uint64_t point_count = cpu_means->element_count / 3u;

                pnanovdb_compute_array_t* cpu_color = compute->create_array(4u, point_count * 3u, nullptr);

                float* mapped_quat = (float*)compute->map_array(cpu_quaternions);
                for (pnanovdb_uint64_t point_idx = 0u; point_idx < point_count; point_idx++)
                {
                    float x = mapped_quat[4u * point_idx + 1u];
                    float y = mapped_quat[4u * point_idx + 2u];
                    float z = mapped_quat[4u * point_idx + 3u];
                    float w = mapped_quat[4u * point_idx + 0u];

                    float magn_inv = 1.f / sqrtf(x * x + y * y + z * z + w * w);

                    mapped_quat[4u * point_idx + 1u] = x * magn_inv;
                    mapped_quat[4u * point_idx + 2u] = y * magn_inv;
                    mapped_quat[4u * point_idx + 3u] = z * magn_inv;
                    mapped_quat[4u * point_idx + 0u] = w * magn_inv;
                }
                compute->unmap_array(cpu_quaternions);

                float* mapped_scale = (float*)compute->map_array(cpu_scales);
                for (pnanovdb_uint64_t point_idx = 0u; point_idx < point_count; point_idx++)
                {
                    mapped_scale[3u * point_idx + 0u] = expf(mapped_scale[3u * point_idx + 0u]);
                    mapped_scale[3u * point_idx + 1u] = expf(mapped_scale[3u * point_idx + 1u]);
                    mapped_scale[3u * point_idx + 2u] = expf(mapped_scale[3u * point_idx + 2u]);
                }
                compute->unmap_array(cpu_scales);

                float* mapped_sh_0 = (float*)compute->map_array(cpu_sh_0);
                float* mapped_color = (float*)compute->map_array(cpu_color);
                for (pnanovdb_uint64_t point_idx = 0u; point_idx < point_count; point_idx++)
                {
                    const float c0 = 0.28209479177387814f;
                    mapped_color[3u * point_idx + 0u] = c0 * mapped_sh_0[3u * point_idx + 0u] + 0.5f;
                    mapped_color[3u * point_idx + 1u] = c0 * mapped_sh_0[3u * point_idx + 1u] + 0.5f;
                    mapped_color[3u * point_idx + 2u] = c0 * mapped_sh_0[3u * point_idx + 2u] + 0.5f;
                }
                compute->unmap_array(cpu_sh_0);
                compute->unmap_array(cpu_color);

                float* mapped_opacity = (float*)compute->map_array(cpu_opacities);
                for (pnanovdb_uint64_t point_idx = 0u; point_idx < point_count; point_idx++)
                {
                    mapped_opacity[point_idx] = 1.f / (1.f + expf(-mapped_opacity[point_idx]));
                }
                compute->unmap_array(cpu_opacities);

                const char* names[7u] = { "means", "opacities", "quaternions", "scales", "sh_0", "sh_n", "colors" };
                pnanovdb_compute_array_t** cpu_arrays[7u] = {&cpu_means, &cpu_opacities, &cpu_quaternions, &cpu_scales, &cpu_sh_0, &cpu_sh_n, &cpu_color};
                for (pnanovdb_uint32_t name_idx = 0u; name_idx < 7u; name_idx++)
                {
                    pnanovdb_compute_array_t* res_array = *cpu_arrays[name_idx];
                    pnanovdb_uint64_t res_size = res_array->element_count * res_array->element_size;
                    pnanovdb_uint32_t res_element_size = res_array->element_size;
                    void* res_data = res_array->data;

                    ptr->buffer_sizes[name_idx] = res_size;

                    pnanovdb_compute_buffer_desc_t buf_desc = {};
                    buf_desc.usage = PNANOVDB_COMPUTE_BUFFER_USAGE_COPY_SRC;
                    buf_desc.format = PNANOVDB_COMPUTE_FORMAT_UNKNOWN;
                    buf_desc.structure_stride = 0;
                    buf_desc.size_in_bytes = res_size;
                    pnanovdb_compute_buffer_t* upload_buffer =
                        compute_interface->create_buffer(context, PNANOVDB_COMPUTE_MEMORY_TYPE_UPLOAD, &buf_desc);

                    buf_desc.usage = PNANOVDB_COMPUTE_BUFFER_USAGE_STRUCTURED | PNANOVDB_COMPUTE_BUFFER_USAGE_RW_STRUCTURED |
                        PNANOVDB_COMPUTE_BUFFER_USAGE_COPY_SRC | PNANOVDB_COMPUTE_BUFFER_USAGE_COPY_DST;
                    buf_desc.format = PNANOVDB_COMPUTE_FORMAT_UNKNOWN;
                    buf_desc.structure_stride = res_element_size;
                    buf_desc.size_in_bytes = res_size;
                    *buffers[name_idx] =
                        compute_interface->create_buffer(context, PNANOVDB_COMPUTE_MEMORY_TYPE_DEVICE, &buf_desc);

                    void* mapped_upload = compute_interface->map_buffer(context, upload_buffer);
                    memcpy(mapped_upload, res_data, res_size);
                    compute_interface->unmap_buffer(context, upload_buffer);

                    pnanovdb_compute_copy_buffer_params_t copy_params = {};
                    copy_params.num_bytes = res_size;
                    copy_params.src = compute_interface->register_buffer_as_transient(context, upload_buffer);
                    copy_params.dst = compute_interface->register_buffer_as_transient(context, *buffers[name_idx]);
                    copy_params.debug_label = "pipeline_gaussian_load";
                    compute_interface->copy_buffer(context, &copy_params);

                    compute_interface->destroy_buffer(context, upload_buffer);
                }
            }
        }

        const char* names[7u] = { "means", "opacities", "quaternions", "scales", "sh_0", "sh_n", "colors" };
        pnanovdb_compute_buffer_t** buffers[7u] = {&ptr->means_buffer, &ptr->opacities_buffer, &ptr->quaternions_buffer,
            &ptr->scales_buffer, &ptr->sh_0_buffer, &ptr->sh_n_buffer, &ptr->color_buffer};
        for (pnanovdb_uint32_t idx = 0u; idx < 7u; idx++)
        {
            for (pnanovdb_uint32_t name_idx = 0u; name_idx < output->buffer_count; name_idx++)
            {
                if (pnanovdb_reflect_string_compare(names[idx], output->buffer_names[name_idx]->str) == 0)
                {
                    output->buffer_buffers[name_idx] = *buffers[idx];
                    output->buffer_sizes[name_idx] = ptr->buffer_sizes[idx];
                    output->buffer_versions[name_idx] = ptr->key_version;
                }
            }
        }
    }

    static pnanovdb_pipeline_desc_t* get_desc()
    {
        static pnanovdb_pipeline_desc_t desc = {};
        desc.name = pnanovdb_get_compute()->get_token("gaussian_load");
        desc.get_pipeline_params_type = get_pipeline_params_type;
        desc.create = create;
        desc.destroy = destroy;
        desc.get_requirements = get_requirements;
        desc.load = load;
        return &desc;
    }
}

namespace pnanovdb_pipeline_gaussian_render
{
    struct Instance
    {
        pnanovdb_raster_t raster = {};
        pnanovdb_raster_context_t* raster_ctx = nullptr;
    };
    PNANOVDB_CAST_PAIR(Instance, pnanovdb_pipeline_instance_t)

    struct pnanovdb_pipeline_gaussian_render_params_t
    {
        pnanovdb_raster_shader_params_t shader_params;
    };
    static const pnanovdb_pipeline_gaussian_render_params_t pnanovdb_pipeline_gaussian_render_params_t_default = {{
        0.3f, // eps2d
        0.f, // min_radius_2d
        16u, // tile_size
        -1, // sh_degree override, <0 means loaded SH degree
        0, // sh_stride_rgbrgbrgb override, 0 means SH are packed rrr...ggg...bbb
        NULL, // data_type
        NULL // name
    }};
    #define PNANOVDB_REFLECT_TYPE pnanovdb_pipeline_gaussian_render_params_t
    PNANOVDB_REFLECT_BEGIN()
    PNANOVDB_REFLECT_VALUE(pnanovdb_raster_shader_params_t, shader_params, 0, 0)
    PNANOVDB_REFLECT_END(&pnanovdb_pipeline_gaussian_render_params_t_default)
    #undef PNANOVDB_REFLECT_TYPE

    void get_pipeline_params_type(const pnanovdb_compute_t* compute,
                                  const pnanovdb_compiler_t* compiler,
                                  const pnanovdb_reflect_data_type_t** out_data_type,
                                  const pnanovdb_reflect_data_type_t** out_metadata_type)
    {
        *out_data_type = PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_pipeline_gaussian_render_params_t);
        *out_metadata_type = nullptr;
    }

    pnanovdb_pipeline_instance_t* create(const pnanovdb_compute_t* compute,
                                         pnanovdb_compute_queue_t* queue)
    {
        auto ptr = new Instance();

        pnanovdb_raster_load(&ptr->raster, compute);

        ptr->raster_ctx = ptr->raster.create_context(compute, queue);

        return cast(ptr);
    }

    void destroy(const pnanovdb_compute_t* compute,
                 pnanovdb_compute_queue_t* queue,
                 pnanovdb_pipeline_instance_t* instance)
    {
        auto ptr = cast(instance);

        pnanovdb_compute_interface_t* compute_interface = compute->device_interface.get_compute_interface(queue);
        pnanovdb_compute_context_t* context = compute->device_interface.get_compute_context(queue);
        if (ptr->raster_ctx)
        {
            ptr->raster.destroy_context(compute, queue, ptr->raster_ctx);
            ptr->raster_ctx = nullptr;
        }

        if (ptr->raster.raster_gaussian_2d_gpu)
        {
            pnanovdb_raster_free(&ptr->raster);
            ptr->raster.raster_gaussian_2d_gpu = nullptr;
        }

        delete ptr;
    }

    pnanovdb_bool_t get_requirements(pnanovdb_pipeline_requirements_params_t* params,
                                     pnanovdb_pipeline_requirements_output_t* output)
    {
        if (!output->names)
        {
            *output->p_name_count = 7u;
            return PNANOVDB_FALSE;
        }
        if (*output->p_name_count >= 1u) {output->names[0u] = "means";}
        if (*output->p_name_count >= 2u) {output->names[1u] = "opacities";}
        if (*output->p_name_count >= 3u) {output->names[2u] = "quaternions";}
        if (*output->p_name_count >= 4u) {output->names[3u] = "scales";}
        if (*output->p_name_count >= 5u) {output->names[4u] = "sh_0";}
        if (*output->p_name_count >= 6u) {output->names[5u] = "sh_n";}
        if (*output->p_name_count >= 7u) {output->names[6u] = "colors";}
        if (*output->p_name_count >= 7u)
        {
            *output->p_name_count = 7u;
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

        if (!pnanovdb_reflect_layout_compare(
                PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_pipeline_gaussian_render_params_t),
                input->params_data_type))
        {
            return;
        }
        pnanovdb_pipeline_gaussian_render_params_t* params = (pnanovdb_pipeline_gaussian_render_params_t*)input->params_data;

        pnanovdb_raster_gpu_buffers_t gpu_buffers = {};

        const char* names[7u] = { "means", "opacities", "quaternions", "scales", "sh_0", "sh_n", "colors" };
        pnanovdb_compute_buffer_t** buffers[7u] = {&gpu_buffers.means, &gpu_buffers.opacities, &gpu_buffers.quaternions,
            &gpu_buffers.scales, &gpu_buffers.sh_0, &gpu_buffers.sh_n, &gpu_buffers.colors};

        for (pnanovdb_uint64_t src_idx = 0u; src_idx < input->buffer_count; src_idx++)
        {
            for (pnanovdb_uint64_t dst_idx = 0u; dst_idx < 7u; dst_idx++)
            {
                if (pnanovdb_reflect_string_compare(names[dst_idx], input->buffer_names[src_idx]->str) == 0)
                {
                    *buffers[dst_idx] = input->buffer_buffers[src_idx];
                    if (dst_idx == 0u)
                    {
                        gpu_buffers.point_count = input->buffer_sizes[src_idx] / 12u;
                    }
                    if (dst_idx == 5u)
                    {
                        if (gpu_buffers.point_count != 0u)
                        {
                            gpu_buffers.sh_stride = input->buffer_sizes[src_idx] / (gpu_buffers.point_count * 12u);
                        }
                    }
                }
            }
        }

        // bail if any buffers not valid
        for (pnanovdb_uint64_t dst_idx = 0u; dst_idx < 7u; dst_idx++)
        {
            if (!(*buffers[dst_idx]))
            {
                return;
            }
        }
        ptr->raster.raster_gaussian_2d_gpu(compute,
                                           queue,
                                           ptr->raster_ctx,
                                           &gpu_buffers,
                                           target->color,
                                           target->width,
                                           target->height,
                                           &target->view,
                                           &target->projection,
                                           &params->shader_params,
                                           PNANOVDB_FALSE);
    }

    static pnanovdb_pipeline_desc_t* get_desc()
    {
        static pnanovdb_pipeline_desc_t desc = {};
        desc.name = pnanovdb_get_compute()->get_token("gaussian_render");
        desc.get_pipeline_params_type = get_pipeline_params_type;
        desc.create = create;
        desc.destroy = destroy;
        desc.get_requirements = get_requirements;
        desc.render = render;
        return &desc;
    }
}

void pnanovdb_pipeline_register_gaussians(const pnanovdb_compute_t* compute,
                                          const pnanovdb_compiler_t* compiler,
                                          pnanovdb_pipeline_registry_t* registry)
{
    pnanovdb_get_pipeline()->register_pipeline(registry, pnanovdb_pipeline_gaussian_load::get_desc());
    pnanovdb_get_pipeline()->register_pipeline(registry, pnanovdb_pipeline_gaussian_render::get_desc());
}
