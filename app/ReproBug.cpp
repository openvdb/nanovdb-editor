// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/app/ReproBug.cpp

    \author Petra Hapalova, Andrew Reidmeyer

    \brief
*/

#include <nanovdb_editor/putil/Editor.h>

#include "Log.h"

#include <stdio.h>

namespace
{
struct compute_gpu_array_t
{
    pnanovdb_compute_buffer_t* upload_buffer;
    pnanovdb_compute_buffer_t* device_buffer;
    pnanovdb_compute_buffer_t* readback_buffer;
    bool device_buffer_external;
};

static compute_gpu_array_t* gpu_array_create()
{
    compute_gpu_array_t* ptr = new compute_gpu_array_t();
    ptr->upload_buffer = nullptr;
    ptr->device_buffer = nullptr;
    ptr->readback_buffer = nullptr;
    return ptr;
}

static void gpu_array_destroy(const pnanovdb_compute_t* compute, pnanovdb_compute_queue_t* queue, compute_gpu_array_t* ptr)
{
    if (!ptr)
    {
        return;
    }

    pnanovdb_compute_interface_t* compute_interface = compute->device_interface.get_compute_interface(queue);
    pnanovdb_compute_context_t* context = compute->device_interface.get_compute_context(queue);

    if (ptr->upload_buffer)
    {
        compute_interface->destroy_buffer(context, ptr->upload_buffer);
        ptr->upload_buffer = nullptr;
    }
    if (ptr->device_buffer)
    {
        compute_interface->destroy_buffer(context, ptr->device_buffer);
        ptr->device_buffer = nullptr;
    }
    if (ptr->readback_buffer)
    {
        compute_interface->destroy_buffer(context, ptr->readback_buffer);
        ptr->readback_buffer = nullptr;
    }
    delete ptr;
}

static void gpu_array_alloc_device(const pnanovdb_compute_t* compute,
                                   pnanovdb_compute_queue_t* queue,
                                   compute_gpu_array_t* ptr,
                                   pnanovdb_compute_array_t* arr,
                                   pnanovdb_bool_t is_constant)
{
    pnanovdb_compute_interface_t* compute_interface = compute->device_interface.get_compute_interface(queue);
    pnanovdb_compute_context_t* context = compute->device_interface.get_compute_context(queue);

    pnanovdb_compute_buffer_desc_t buf_desc = {};
    if (!ptr->device_buffer)
    {
        buf_desc.usage = PNANOVDB_COMPUTE_BUFFER_USAGE_RW_STRUCTURED | PNANOVDB_COMPUTE_BUFFER_USAGE_COPY_SRC |
                         PNANOVDB_COMPUTE_BUFFER_USAGE_COPY_DST;
        if (is_constant)
        {
            buf_desc.usage = PNANOVDB_COMPUTE_BUFFER_USAGE_CONSTANT | PNANOVDB_COMPUTE_BUFFER_USAGE_COPY_SRC |
                             PNANOVDB_COMPUTE_BUFFER_USAGE_COPY_DST;
        }
        buf_desc.format = PNANOVDB_COMPUTE_FORMAT_UNKNOWN;
        buf_desc.structure_stride = 4u;
        buf_desc.size_in_bytes = arr->element_count * arr->element_size;
        if (buf_desc.size_in_bytes < 65536u)
        {
            buf_desc.size_in_bytes = 65536u;
        }
        ptr->device_buffer = compute_interface->create_buffer(context, PNANOVDB_COMPUTE_MEMORY_TYPE_DEVICE, &buf_desc);
    }
}

static void gpu_array_upload(const pnanovdb_compute_t* compute,
                             pnanovdb_compute_queue_t* queue,
                             compute_gpu_array_t* ptr,
                             pnanovdb_compute_array_t* arr,
                             pnanovdb_bool_t is_constant)
{
    pnanovdb_compute_interface_t* compute_interface = compute->device_interface.get_compute_interface(queue);
    pnanovdb_compute_context_t* context = compute->device_interface.get_compute_context(queue);

    pnanovdb_compute_buffer_desc_t buf_desc = {};
    if (!ptr->upload_buffer)
    {
        buf_desc.usage = PNANOVDB_COMPUTE_BUFFER_USAGE_COPY_SRC;
        buf_desc.format = PNANOVDB_COMPUTE_FORMAT_UNKNOWN;
        buf_desc.structure_stride = 0u;
        buf_desc.size_in_bytes = arr->element_count * arr->element_size;
        if (buf_desc.size_in_bytes < 65536u)
        {
            buf_desc.size_in_bytes = 65536u;
        }
        ptr->upload_buffer = compute_interface->create_buffer(context, PNANOVDB_COMPUTE_MEMORY_TYPE_UPLOAD, &buf_desc);
    }
    gpu_array_alloc_device(compute, queue, ptr, arr, is_constant);

    // copy arr
    void* mapped_arr = compute_interface->map_buffer(context, ptr->upload_buffer);
    memcpy(mapped_arr, arr->data, arr->element_count * arr->element_size);
    compute_interface->unmap_buffer(context, ptr->upload_buffer);

    // upload arr
    pnanovdb_compute_copy_buffer_params_t copy_params = {};
    copy_params.num_bytes = arr->element_count * arr->element_size;
    copy_params.src = compute_interface->register_buffer_as_transient(context, ptr->upload_buffer);
    copy_params.dst = compute_interface->register_buffer_as_transient(context, ptr->device_buffer);
    copy_params.debug_label = "gpu_array_upload";
    compute_interface->copy_buffer(context, &copy_params);
}

static void gpu_array_copy(const pnanovdb_compute_t* compute,
                           pnanovdb_compute_queue_t* queue,
                           compute_gpu_array_t* ptr,
                           pnanovdb_compute_buffer_t* src_buffer,
                           pnanovdb_uint64_t src_offset,
                           pnanovdb_uint64_t num_bytes)
{
    pnanovdb_compute_interface_t* compute_interface = compute->device_interface.get_compute_interface(queue);
    pnanovdb_compute_context_t* context = compute->device_interface.get_compute_context(queue);

    // copy src to device array
    pnanovdb_compute_copy_buffer_params_t copy_params = {};
    copy_params.num_bytes = num_bytes;
    copy_params.src_offset = src_offset;
    copy_params.src = compute_interface->register_buffer_as_transient(context, src_buffer);
    copy_params.dst = compute_interface->register_buffer_as_transient(context, ptr->device_buffer);
    copy_params.debug_label = "gpu_array_copy";
    compute_interface->copy_buffer(context, &copy_params);
}

static void gpu_array_readback(const pnanovdb_compute_t* compute,
                               pnanovdb_compute_queue_t* queue,
                               compute_gpu_array_t* ptr,
                               pnanovdb_compute_array_t* arr)
{
    pnanovdb_compute_interface_t* compute_interface = compute->device_interface.get_compute_interface(queue);
    pnanovdb_compute_context_t* context = compute->device_interface.get_compute_context(queue);

    pnanovdb_compute_buffer_desc_t buf_desc = {};
    if (!ptr->readback_buffer)
    {
        buf_desc.usage = PNANOVDB_COMPUTE_BUFFER_USAGE_COPY_DST;
        buf_desc.format = PNANOVDB_COMPUTE_FORMAT_UNKNOWN;
        buf_desc.structure_stride = 0u;
        buf_desc.size_in_bytes = arr->element_count * arr->element_size;
        if (buf_desc.size_in_bytes < 65536u)
        {
            buf_desc.size_in_bytes = 65536u;
        }
        ptr->readback_buffer =
            compute_interface->create_buffer(context, PNANOVDB_COMPUTE_MEMORY_TYPE_READBACK, &buf_desc);
    }

    // readback arr
    pnanovdb_compute_copy_buffer_params_t copy_params = {};
    copy_params.num_bytes = arr->element_count * arr->element_size;
    copy_params.src = compute_interface->register_buffer_as_transient(context, ptr->device_buffer);
    copy_params.dst = compute_interface->register_buffer_as_transient(context, ptr->readback_buffer);
    copy_params.debug_label = "gpu_array_readback";
    compute_interface->copy_buffer(context, &copy_params);
}

static void gpu_array_map(const pnanovdb_compute_t* compute,
                          const pnanovdb_compute_queue_t* queue,
                          compute_gpu_array_t* ptr,
                          pnanovdb_compute_array_t* arr)
{
    pnanovdb_compute_interface_t* compute_interface = compute->device_interface.get_compute_interface(queue);
    pnanovdb_compute_context_t* context = compute->device_interface.get_compute_context(queue);

    // copy arr
    void* mapped_arr = compute_interface->map_buffer(context, ptr->readback_buffer);
    memcpy(arr->data, mapped_arr, arr->element_count * arr->element_size);
    compute_interface->unmap_buffer(context, ptr->readback_buffer);
}
}

static pnanovdb_compute_array_t* read_file(
    const pnanovdb_compute_t* compute, const char* file_path)
{
    FILE* file = fopen(file_path, "rb");
    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);
    pnanovdb_compute_array_t* arr = compute->create_array(8u, (file_size + 7u) / 8u, nullptr);
    fread(arr->data, 1u, file_size, file);
    fclose(file);
    return arr;
}

static void repro_run_shader(const pnanovdb_compute_t* compute,
                             pnanovdb_compute_queue_t* queue)
{
    pnanovdb_compute_interface_t* compute_interface = compute->device_interface.get_compute_interface(queue);
    pnanovdb_compute_context_t* context = compute->device_interface.get_compute_context(queue);

    pnanovdb_shader_context_t* shader_ctx = nullptr;
    pnanovdb_compiler_settings_t compile_settings = {};
    pnanovdb_compiler_settings_init(&compile_settings);
    shader_ctx = compute->create_shader_context("raster/voxelbvh/voxelbvh_nanovdb_add_count.slang");
    compute->init_shader(compute, queue, shader_ctx, &compile_settings);

    pnanovdb_compute_array_t* arr_constants = read_file(compute, "debug_constants.raw");
    pnanovdb_compute_array_t* arr_nanovdb = read_file(compute, "debug_nanovdb.raw");
    pnanovdb_compute_array_t* arr_node_mask = read_file(compute, "debug_node_mask.raw");
    pnanovdb_compute_array_t* arr_workgroup_count = read_file(compute, "debug_workgroup_count.raw");

    compute_gpu_array_t* gpu_constants = gpu_array_create();
    compute_gpu_array_t* gpu_nanovdb = gpu_array_create();
    compute_gpu_array_t* gpu_node_mask = gpu_array_create();
    compute_gpu_array_t* gpu_workgroup_count = gpu_array_create();
    gpu_array_upload(compute, queue, gpu_constants, arr_constants, PNANOVDB_TRUE);
    gpu_array_upload(compute, queue, gpu_nanovdb, arr_nanovdb, PNANOVDB_FALSE);
    gpu_array_upload(compute, queue, gpu_node_mask, arr_node_mask, PNANOVDB_FALSE);
    gpu_array_upload(compute, queue, gpu_workgroup_count, arr_workgroup_count, PNANOVDB_FALSE);

    uint64_t word_count = 256u * 256u;
    pnanovdb_compute_array_t* debug_array = compute->create_array(4u, word_count, nullptr);
    compute_gpu_array_t* debug_gpu_array = gpu_array_create();
    gpu_array_upload(compute, queue, debug_gpu_array, debug_array, PNANOVDB_FALSE);

    pnanovdb_compute_resource_t resources[5u] = {};
    resources[0u].buffer_transient = compute_interface->register_buffer_as_transient(context, gpu_constants->device_buffer);
    resources[1u].buffer_transient = compute_interface->register_buffer_as_transient(context, gpu_nanovdb->device_buffer);
    resources[2u].buffer_transient = compute_interface->register_buffer_as_transient(context, gpu_node_mask->device_buffer);
    resources[3u].buffer_transient = compute_interface->register_buffer_as_transient(context, gpu_workgroup_count->device_buffer);
    resources[4u].buffer_transient = compute_interface->register_buffer_as_transient(context, debug_gpu_array->device_buffer);

    compute->dispatch_shader(compute_interface, context, shader_ctx,
                                resources, 256u, 1u, 1u, "voxelbvh_nanovdb_add_count");

    gpu_array_readback(compute, queue, gpu_workgroup_count, arr_workgroup_count);
    gpu_array_readback(compute, queue, debug_gpu_array, debug_array);

    pnanovdb_uint64_t flushed_frame = 0llu;
    compute->device_interface.flush(queue, &flushed_frame, nullptr, nullptr);
    compute->device_interface.wait_idle(queue);

    printf("workgroup_count buffer:\n");
    gpu_array_map(compute, queue, gpu_workgroup_count, arr_workgroup_count);
    uint64_t* mapped_workgroup_count = (uint64_t*)arr_workgroup_count->data;
    for (uint64_t idx = 0u; idx < 4u * 256u; idx++)
    {
        uint64_t a = mapped_workgroup_count[idx];
        if (a != 0llu)
        {
            printf("[%zu]%zu,", idx, a);
        }
    }
    printf("\n");

    printf("debug buffer:\n");
    gpu_array_map(compute, queue, debug_gpu_array, debug_array);
    uint32_t* mapped_debug = (uint32_t*)debug_array->data;
    for (uint32_t idx = 0u; idx < word_count; idx++)
    {
        uint32_t a = mapped_debug[idx];
        if (a != 0llu)
        {
            printf("[%u]%u,", idx, a);
        }
    }
    printf("\n");
    gpu_array_destroy(compute, queue, debug_gpu_array);

    compute->destroy_shader_context(compute, queue, shader_ctx);
}

void repro_bug(uint32_t device_index)
{
    pnanovdb_compiler_t compiler = {};
    pnanovdb_compiler_load(&compiler);

    pnanovdb_compute_t compute = {};
    pnanovdb_compute_load(&compute, &compiler);

    pnanovdb_compute_device_desc_t device_desc = {};
    device_desc.log_print = pnanovdb_compute_log_print;
    device_desc.device_index = device_index;

    pnanovdb_compute_device_manager_t* device_manager =
        compute.device_interface.create_device_manager(PNANOVDB_FALSE);
    pnanovdb_compute_device_t* device = compute.device_interface.create_device(device_manager, &device_desc);

    pnanovdb_compute_queue_t* queue = compute.device_interface.get_device_queue(device);

    repro_run_shader(&compute, queue);

    compute.device_interface.destroy_device(device_manager, device);
    compute.device_interface.destroy_device_manager(device_manager);

    pnanovdb_compute_free(&compute);
    pnanovdb_compiler_free(&compiler);

}
