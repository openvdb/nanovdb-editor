// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/raster/Common.h

    \author Andrew Reidmeyer

    \brief
*/

#pragma once

#include "nanovdb_editor/putil/Compute.h"

#include <math.h>
#include <vector>

#if defined(_WIN32)
#    include <Windows.h>
#else
#    include <time.h>
#endif

PNANOVDB_INLINE void timestamp_capture(pnanovdb_uint64_t* ptr)
{
#if defined(_WIN32)
    LARGE_INTEGER tmpCpuTime = {};
    QueryPerformanceCounter(&tmpCpuTime);
    (*ptr) = tmpCpuTime.QuadPart;
#else
    timespec timeValue = {};
    clock_gettime(CLOCK_MONOTONIC, &timeValue);
    (*ptr) = 1E9 * pnanovdb_uint64_t(timeValue.tv_sec) + pnanovdb_uint64_t(timeValue.tv_nsec);
#endif
}
PNANOVDB_INLINE pnanovdb_uint64_t timestamp_frequency()
{
#if defined(_WIN32)
    LARGE_INTEGER tmpCpuFreq = {};
    QueryPerformanceFrequency(&tmpCpuFreq);
    return tmpCpuFreq.QuadPart;
#else
    return 1E9;
#endif
}
PNANOVDB_INLINE float timestamp_diff(pnanovdb_uint64_t begin, pnanovdb_uint64_t end, pnanovdb_uint64_t freq)
{
    return (float)(((double)(end - begin) / (double)(freq)));
}

static const pnanovdb_uint64_t upload_chunk_size = 16llu * 1024llu * 1024llu;

struct compute_gpu_array_t
{
    std::vector<pnanovdb_compute_buffer_t*> upload_buffers;
    pnanovdb_compute_buffer_t* device_buffer;
    std::vector<pnanovdb_compute_buffer_t*>  readback_buffers;
    bool device_buffer_external;
};

static compute_gpu_array_t* gpu_array_create()
{
    compute_gpu_array_t* ptr = new compute_gpu_array_t();
    ptr->upload_buffers.resize(0);
    ptr->device_buffer = nullptr;
    ptr->readback_buffers.resize(0);
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

    for (size_t idx = 0u; idx < ptr->upload_buffers.size(); idx++)
    {
        if (ptr->upload_buffers[idx])
        {
            compute_interface->destroy_buffer(context, ptr->upload_buffers[idx]);
            ptr->upload_buffers[idx] = nullptr;
        }
    }
    if (ptr->device_buffer)
    {
        compute_interface->destroy_buffer(context, ptr->device_buffer);
        ptr->device_buffer = nullptr;
    }
    for (size_t idx = 0u; idx < ptr->readback_buffers.size(); idx++)
    {
        if (ptr->readback_buffers[idx])
        {
            compute_interface->destroy_buffer(context, ptr->readback_buffers[idx]);
            ptr->readback_buffers[idx] = nullptr;
        }
    }
    delete ptr;
}

static void gpu_array_alloc_device(const pnanovdb_compute_t* compute,
                                   pnanovdb_compute_queue_t* queue,
                                   compute_gpu_array_t* ptr,
                                   pnanovdb_compute_array_t* arr)
{
    pnanovdb_compute_interface_t* compute_interface = compute->device_interface.get_compute_interface(queue);
    pnanovdb_compute_context_t* context = compute->device_interface.get_compute_context(queue);

    pnanovdb_compute_buffer_desc_t buf_desc = {};
    if (!ptr->device_buffer)
    {
        buf_desc.usage = PNANOVDB_COMPUTE_BUFFER_USAGE_RW_STRUCTURED |
                         PNANOVDB_COMPUTE_BUFFER_USAGE_STRUCTURED |
                         PNANOVDB_COMPUTE_BUFFER_USAGE_COPY_SRC |
                         PNANOVDB_COMPUTE_BUFFER_USAGE_COPY_DST;
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
                             pnanovdb_compute_array_t* arr)
{
    pnanovdb_compute_interface_t* compute_interface = compute->device_interface.get_compute_interface(queue);
    pnanovdb_compute_context_t* context = compute->device_interface.get_compute_context(queue);

    gpu_array_alloc_device(compute, queue, ptr, arr);

    pnanovdb_uint64_t buf_size = arr->element_count * arr->element_size;
    if (buf_size < 65536u)
    {
        buf_size = 65536u;
    }

    size_t chunk_count = (buf_size + upload_chunk_size - 1) / upload_chunk_size;
    ptr->upload_buffers.resize(chunk_count);
    for (size_t chunk_idx = 0u; chunk_idx < chunk_count; chunk_idx++)
    {
        pnanovdb_uint64_t chunk_size = buf_size - chunk_idx * upload_chunk_size;
        if (chunk_size > upload_chunk_size)
        {
            chunk_size = upload_chunk_size;
        }

        pnanovdb_compute_buffer_desc_t buf_desc = {};
        if (!ptr->upload_buffers[chunk_idx])
        {
            buf_desc.usage = PNANOVDB_COMPUTE_BUFFER_USAGE_COPY_SRC;
            buf_desc.format = PNANOVDB_COMPUTE_FORMAT_UNKNOWN;
            buf_desc.structure_stride = 0u;
            buf_desc.size_in_bytes = chunk_size;
            ptr->upload_buffers[chunk_idx] = compute_interface->create_buffer(context, PNANOVDB_COMPUTE_MEMORY_TYPE_UPLOAD, &buf_desc);
        }

        const uint8_t* src_data = (const uint8_t*)arr->data;

        // copy arr
        void* mapped_arr = compute_interface->map_buffer(context, ptr->upload_buffers[chunk_idx]);
        memcpy(mapped_arr, src_data + chunk_idx * upload_chunk_size, chunk_size);
        compute_interface->unmap_buffer(context, ptr->upload_buffers[chunk_idx]);

        // upload arr
        pnanovdb_compute_copy_buffer_params_t copy_params = {};
        copy_params.num_bytes = chunk_size;
        copy_params.src = compute_interface->register_buffer_as_transient(context, ptr->upload_buffers[chunk_idx]);
        copy_params.dst = compute_interface->register_buffer_as_transient(context, ptr->device_buffer);
        copy_params.dst_offset = chunk_idx * upload_chunk_size;
        copy_params.debug_label = "gpu_array_upload";
        compute_interface->copy_buffer(context, &copy_params);
    }
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

    pnanovdb_uint64_t buf_size = arr->element_count * arr->element_size;
    if (buf_size < 65536u)
    {
        buf_size = 65536u;
    }

    size_t chunk_count = (buf_size + upload_chunk_size - 1) / upload_chunk_size;
    ptr->readback_buffers.resize(chunk_count);
    for (size_t chunk_idx = 0u; chunk_idx < chunk_count; chunk_idx++)
    {
        pnanovdb_uint64_t chunk_size = buf_size - chunk_idx * upload_chunk_size;
        if (chunk_size > upload_chunk_size)
        {
            chunk_size = upload_chunk_size;
        }

        pnanovdb_compute_buffer_desc_t buf_desc = {};
        if (!ptr->readback_buffers[chunk_idx])
        {
            buf_desc.usage = PNANOVDB_COMPUTE_BUFFER_USAGE_COPY_DST;
            buf_desc.format = PNANOVDB_COMPUTE_FORMAT_UNKNOWN;
            buf_desc.structure_stride = 0u;
            buf_desc.size_in_bytes = chunk_size;
            ptr->readback_buffers[chunk_idx] =
                compute_interface->create_buffer(context, PNANOVDB_COMPUTE_MEMORY_TYPE_READBACK, &buf_desc);
        }

        // readback arr
        pnanovdb_compute_copy_buffer_params_t copy_params = {};
        copy_params.num_bytes = chunk_size;
        copy_params.src_offset = chunk_idx * upload_chunk_size;
        copy_params.src = compute_interface->register_buffer_as_transient(context, ptr->device_buffer);
        copy_params.dst = compute_interface->register_buffer_as_transient(context, ptr->readback_buffers[chunk_idx]);
        copy_params.debug_label = "gpu_array_readback";
        compute_interface->copy_buffer(context, &copy_params);
    }
}

static void gpu_array_map(const pnanovdb_compute_t* compute,
                          const pnanovdb_compute_queue_t* queue,
                          compute_gpu_array_t* ptr,
                          pnanovdb_compute_array_t* arr)
{
    pnanovdb_compute_interface_t* compute_interface = compute->device_interface.get_compute_interface(queue);
    pnanovdb_compute_context_t* context = compute->device_interface.get_compute_context(queue);

    pnanovdb_uint64_t buf_size = arr->element_count * arr->element_size;

    // copy arr
    for (size_t chunk_idx = 0u; chunk_idx < ptr->readback_buffers.size(); chunk_idx++)
    {
        pnanovdb_uint64_t chunk_size = buf_size - chunk_idx * upload_chunk_size;
        if (chunk_size > upload_chunk_size)
        {
            chunk_size = upload_chunk_size;
        }

        uint8_t* dst_data = (uint8_t*)arr->data;

        void* mapped_arr = compute_interface->map_buffer(context, ptr->readback_buffers[chunk_idx]);
        memcpy(dst_data + chunk_idx * upload_chunk_size, mapped_arr, chunk_size);
        compute_interface->unmap_buffer(context, ptr->readback_buffers[chunk_idx]);
    }
}
