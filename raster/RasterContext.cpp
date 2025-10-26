// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/raster/RasterContext.cpp

    \author Andrew Reidmeyer

    \brief
*/

#define PNANOVDB_BUF_BOUNDS_CHECK
#include "Raster.h"

#include "nanovdb_editor/PNanoVDBExt.h"
#include "nanovdb_editor/putil/ThreadPool.hpp"

#include <stdlib.h>
#include <cstring>
#include <math.h>
#include <vector>
#include <future>

namespace pnanovdb_raster
{

pnanovdb_raster_context_t* create_context(const pnanovdb_compute_t* compute, pnanovdb_compute_queue_t* queue)
{
    raster_context_t* ctx = new raster_context_t();

    pnanovdb_parallel_primitives_load(&ctx->parallel_primitives, compute);
    ctx->parallel_primitives_ctx = ctx->parallel_primitives.create_context(compute, queue);
    if (!ctx->parallel_primitives_ctx)
    {
        return nullptr;
    }

    pnanovdb_grid_build_load(&ctx->grid_build, compute);
    ctx->grid_build_ctx = ctx->grid_build.create_context(compute, queue);
    if (!ctx->grid_build_ctx)
    {
        return nullptr;
    }

    pnanovdb_compiler_settings_t compile_settings = {};
    pnanovdb_compiler_settings_init(&compile_settings);

    pnanovdb_util::ThreadPool pool;
    std::vector<std::future<bool>> futures;

    for (pnanovdb_uint32_t idx = 0u; idx < shader_count; idx++)
    {
        auto future = pool.enqueue(
            [compute, queue, ctx, idx, &compile_settings]() -> bool
            {
                ctx->shader_ctx[idx] = compute->create_shader_context(s_shader_names[idx]);
                return compute->init_shader(compute, queue, ctx->shader_ctx[idx], &compile_settings) == PNANOVDB_TRUE;
            });
        futures.push_back(std::move(future));
    }

    for (auto& future : futures)
    {
        bool success = future.get();
        if (!success)
        {
            return nullptr;
        }
    }

    return cast(ctx);
}

void destroy_context(const pnanovdb_compute_t* compute,
                     pnanovdb_compute_queue_t* queue,
                     pnanovdb_raster_context_t* context_in)
{
    auto ctx = cast(context_in);

    for (pnanovdb_uint32_t idx = 0u; idx < shader_count; idx++)
    {
        compute->destroy_shader_context(compute, queue, ctx->shader_ctx[idx]);
    }

    ctx->parallel_primitives.destroy_context(compute, queue, ctx->parallel_primitives_ctx);
    pnanovdb_parallel_primitives_free(&ctx->parallel_primitives);
    ctx->grid_build.destroy_context(compute, queue, ctx->grid_build_ctx);
    pnanovdb_grid_build_free(&ctx->grid_build);

    delete ctx;
}

pnanovdb_raster_gaussian_data_t* create_gaussian_data(const pnanovdb_compute_t* compute,
                                                      pnanovdb_compute_queue_t* queue,
                                                      pnanovdb_raster_context_t* context,
                                                      pnanovdb_compute_array_t* means,
                                                      pnanovdb_compute_array_t* quaternions,
                                                      pnanovdb_compute_array_t* scales,
                                                      pnanovdb_compute_array_t* colors,
                                                      pnanovdb_compute_array_t* sh_0,
                                                      pnanovdb_compute_array_t* sh_n,
                                                      pnanovdb_compute_array_t* opacities,
                                                      pnanovdb_compute_array_t** shader_params_arrays,
                                                      pnanovdb_raster_shader_params_t* raster_params) // TDO0: remove in
                                                                                                      // 0.2.0
{
    auto ptr = new gaussian_data_t();

    ptr->point_count = means->element_count / 3u;
    ptr->sh_stride = !sh_n ? 0u : (pnanovdb_uint32_t)(sh_n->element_count / means->element_count);

    ptr->has_uploaded = PNANOVDB_FALSE;

    ptr->means_cpu_array = compute->create_array(means->element_size, means->element_count, means->data);
    ptr->quaternions_cpu_array =
        compute->create_array(quaternions->element_size, quaternions->element_count, quaternions->data);
    ptr->scales_cpu_array = compute->create_array(scales->element_size, scales->element_count, scales->data);
    ptr->colors_cpu_array = compute->create_array(colors->element_size, colors->element_count, colors->data);
    ptr->sh_0_cpu_array = compute->create_array(sh_0->element_size, sh_0->element_count, sh_0->data);
    if (sh_n)
    {
        ptr->sh_n_cpu_array = compute->create_array(sh_n->element_size, sh_n->element_count, sh_n->data);
    }
    else
    {
        ptr->sh_n_cpu_array = compute->create_array(0u, 0u, nullptr);
    }
    ptr->opacities_cpu_array = compute->create_array(opacities->element_size, opacities->element_count, opacities->data);
    ptr->shader_params_cpu_arrays = new pnanovdb_compute_array_t*[shader_param_count];

    for (pnanovdb_uint32_t idx = 0u; idx < shader_param_count; idx++)
    {
        ptr->shader_params_cpu_arrays[idx] = nullptr;
        if (shader_params_arrays == nullptr || shader_params_arrays[idx] == nullptr)
        {
            continue;
        }

        pnanovdb_compute_array_t* shader_params = shader_params_arrays[idx];
        if (shader_params->element_count == 0u)
        {
            continue;
        }
        ptr->shader_params_cpu_arrays[idx] =
            compute->create_array(shader_params->element_size, shader_params->element_count, shader_params->data);
    }

    ptr->means_gpu_array = gpu_array_create();
    ptr->quaternions_gpu_array = gpu_array_create();
    ptr->scales_gpu_array = gpu_array_create();
    ptr->colors_gpu_array = gpu_array_create();
    ptr->sh_0_gpu_array = gpu_array_create();
    ptr->sh_n_gpu_array = gpu_array_create();
    ptr->opacities_gpu_array = gpu_array_create();
    ptr->shader_params_gpu_arrays = new compute_gpu_array_t*[shader_param_count];

    pnanovdb_compute_interface_t* compute_interface = compute->device_interface.get_compute_interface(queue);
    pnanovdb_compute_context_t* compute_context = compute->device_interface.get_compute_context(queue);
    pnanovdb_compute_buffer_desc_t buf_desc = {};
    buf_desc.usage = PNANOVDB_COMPUTE_BUFFER_USAGE_CONSTANT;
    buf_desc.format = PNANOVDB_COMPUTE_FORMAT_UNKNOWN;
    buf_desc.structure_stride = 0u;
    buf_desc.size_in_bytes = PNANOVDB_COMPUTE_CONSTANT_BUFFER_MAX_SIZE;

    // copy shader params
    auto create_and_init_buffer = [&](const void* src, size_t size) -> pnanovdb_compute_buffer_t*
    {
        pnanovdb_compute_buffer_t* buffer =
            compute_interface->create_buffer(compute_context, PNANOVDB_COMPUTE_MEMORY_TYPE_UPLOAD, &buf_desc);
        void* mapped = compute_interface->map_buffer(compute_context, buffer);
        if (src && size > 0)
        {
            std::memcpy(mapped, src, size);
        }
        else
        {
            std::memset(mapped, 0, PNANOVDB_COMPUTE_CONSTANT_BUFFER_MAX_SIZE);
        }
        compute_interface->unmap_buffer(compute_context, buffer);
        return buffer;
    };

    for (pnanovdb_uint32_t idx = 0u; idx < shader_param_count; idx++)
    {
        pnanovdb_compute_buffer_t* buffer = nullptr;
        if (ptr->shader_params_cpu_arrays[idx] && shader_params_arrays && shader_params_arrays[idx] &&
            shader_params_arrays[idx]->element_count > 0u)
        {
            buffer = create_and_init_buffer(shader_params_arrays[idx]->data, shader_params_arrays[idx]->element_count *
                                                                                 shader_params_arrays[idx]->element_size);
        }
        else
        {
            buffer = create_and_init_buffer(nullptr, 0);
        }
        ptr->shader_params_gpu_arrays[idx] = gpu_array_create();
        ptr->shader_params_gpu_arrays[idx]->device_buffer = buffer;
    }

    return cast(ptr);
}

void upload_gaussian_data(const pnanovdb_compute_t* compute,
                          pnanovdb_compute_queue_t* queue,
                          pnanovdb_raster_context_t* context,
                          pnanovdb_raster_gaussian_data_t* data)
{
    auto ptr = cast(data);

    if (!ptr->has_uploaded)
    {
        ptr->has_uploaded = PNANOVDB_TRUE;

        gpu_array_upload(compute, queue, ptr->means_gpu_array, ptr->means_cpu_array);
        gpu_array_upload(compute, queue, ptr->quaternions_gpu_array, ptr->quaternions_cpu_array);
        gpu_array_upload(compute, queue, ptr->scales_gpu_array, ptr->scales_cpu_array);
        gpu_array_upload(compute, queue, ptr->colors_gpu_array, ptr->colors_cpu_array);
        gpu_array_upload(compute, queue, ptr->sh_0_gpu_array, ptr->sh_0_cpu_array);
        gpu_array_upload(compute, queue, ptr->sh_n_gpu_array, ptr->sh_n_cpu_array);
        gpu_array_upload(compute, queue, ptr->opacities_gpu_array, ptr->opacities_cpu_array);

        for (pnanovdb_uint32_t idx = 0u; idx < shader_param_count; idx++)
        {
            if (ptr->shader_params_cpu_arrays[idx])
            {
                gpu_array_upload(compute, queue, ptr->shader_params_gpu_arrays[idx], ptr->shader_params_cpu_arrays[idx]);
            }
        }
    }
}

void destroy_gaussian_data(const pnanovdb_compute_t* compute,
                           pnanovdb_compute_queue_t* queue,
                           pnanovdb_raster_gaussian_data_t* data)
{
    if (!data)
    {
        return;
    }

    auto ptr = cast(data);
    auto context = compute->device_interface.get_compute_context(queue);

    // Set minLifetime to 0 so buffers are freed immediately
    compute->device_interface.set_resource_min_lifetime(context, 0u);

    gpu_array_destroy(compute, queue, ptr->means_gpu_array);
    gpu_array_destroy(compute, queue, ptr->quaternions_gpu_array);
    gpu_array_destroy(compute, queue, ptr->scales_gpu_array);
    gpu_array_destroy(compute, queue, ptr->colors_gpu_array);
    gpu_array_destroy(compute, queue, ptr->sh_0_gpu_array);
    gpu_array_destroy(compute, queue, ptr->sh_n_gpu_array);
    gpu_array_destroy(compute, queue, ptr->opacities_gpu_array);

    compute->destroy_array(ptr->means_cpu_array);
    compute->destroy_array(ptr->quaternions_cpu_array);
    compute->destroy_array(ptr->scales_cpu_array);
    compute->destroy_array(ptr->colors_cpu_array);
    compute->destroy_array(ptr->sh_0_cpu_array);
    compute->destroy_array(ptr->sh_n_cpu_array);
    compute->destroy_array(ptr->opacities_cpu_array);

    for (pnanovdb_uint32_t idx = 0u; idx < shader_param_count; idx++)
    {
        if (ptr->shader_params_cpu_arrays[idx])
        {
            gpu_array_destroy(compute, queue, ptr->shader_params_gpu_arrays[idx]);
            compute->destroy_array(ptr->shader_params_cpu_arrays[idx]);
        }
    }

    delete[] ptr->shader_params_gpu_arrays;
    delete[] ptr->shader_params_cpu_arrays;
    delete ptr;

    compute->device_interface.wait_idle(queue);

    // Restore original minLifetime
    compute->device_interface.set_resource_min_lifetime(context, 60u);
}

}
