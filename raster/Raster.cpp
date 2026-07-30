// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/raster/Raster.cpp

    \author Andrew Reidmeyer

    \brief
*/

#define PNANOVDB_BUF_BOUNDS_CHECK
#include "Raster.h"

#include "nanovdb_editor/PNanoVDBExt.h"
#include "nanovdb_editor/putil/WorkerThread.hpp"

#include <stdlib.h>
#include <math.h>
#include <vector>

// #define PNANOVDB_RASTER_VALIDATE 1

namespace pnanovdb_raster
{

static void raster_profiler_report(void* userdata,
                                   pnanovdb_uint64_t capture_id,
                                   pnanovdb_uint32_t num_entries,
                                   pnanovdb_compute_profiler_entry_t* entries)
{
    printf("raster_to_nanovdb() profiler results capture_id(%llu):\n", (unsigned long long int)capture_id);
    for (pnanovdb_uint32_t idx = 0u; idx < num_entries; idx++)
    {
        printf("[%d] name(%s) cpu_ms(%f) gpu_ms(%f)\n", idx, entries[idx].label, 1000.f * entries[idx].cpu_delta_time,
               1000.f * entries[idx].gpu_delta_time);
    }
}

#if PNANOVDB_RASTER_VALIDATE
static void raster_validate(const pnanovdb_compute_t* compute,
                            float voxel_size,
                            pnanovdb_compute_array_t* positions,
                            pnanovdb_compute_array_t* colors,
                            pnanovdb_compute_array_t* nanovdb_arr);
#endif

void raster_gaussian_3d(const pnanovdb_compute_t* compute,
                        pnanovdb_compute_queue_t* queue,
                        pnanovdb_raster_context_t* context_in,
                        float voxel_size,
                        pnanovdb_raster_gaussian_data_t* data_in,
                        pnanovdb_compute_buffer_t* nanovdb_out,
                        pnanovdb_uint64_t nanovdb_word_count,
                        void* userdata)
{
    auto ctx = cast(context_in);
    auto data = cast(data_in);

    pnanovdb_compute_interface_t* compute_interface = compute->device_interface.get_compute_interface(queue);
    pnanovdb_compute_context_t* context = compute->device_interface.get_compute_context(queue);

    pnanovdb_util::WorkerThread* worker = static_cast<pnanovdb_util::WorkerThread*>(userdata);
    if (worker)
    {
        worker->updateTaskProgress(0.f, "Rastering gaussian data");
    }

    if (worker)
    {
        worker->updateTaskProgress(1.f);
    }
}

pnanovdb_compute_array_t* raster_to_nanovdb(const pnanovdb_compute_t* compute,
                                            pnanovdb_compute_queue_t* queue,
                                            float voxel_size,
                                            pnanovdb_compute_array_t* means,
                                            pnanovdb_compute_array_t* quaternions,
                                            pnanovdb_compute_array_t* scales,
                                            pnanovdb_compute_array_t* colors,
                                            pnanovdb_compute_array_t* sh_0,
                                            pnanovdb_compute_array_t* sh_n,
                                            pnanovdb_compute_array_t* opacities,
                                            pnanovdb_compute_array_t** shader_params_arrays,
                                            pnanovdb_profiler_report_t profiler_report,
                                            void* userdata)
{
    raster_context_t* ctx = cast(create_context(compute, queue));
    if (!ctx)
    {
        return nullptr;
    }

    pnanovdb_compute_interface_t* compute_interface = compute->device_interface.get_compute_interface(queue);
    pnanovdb_compute_context_t* context = compute->device_interface.get_compute_context(queue);

    compute->device_interface.enable_profiler(context, (void*)("raster"), profiler_report);

    // note: colors duplicate for now, since no SH in interface
    pnanovdb_raster_gaussian_data_t* data =
        create_gaussian_data(compute, queue, cast(ctx), means, quaternions, scales, colors, sh_0, sh_n, opacities,
                             shader_params_arrays, nullptr);

    upload_gaussian_data(compute, queue, cast(ctx), data);

    pnanovdb_uint64_t nanovdb_word_count = 3u * 256u * 1024u * 1024u;
    pnanovdb_compute_array_t* nanovdb_array = compute->create_array(4u, nanovdb_word_count, nullptr);

    compute_gpu_array_t* nanovdb_gpu_array = gpu_array_create();
    gpu_array_alloc_device(compute, queue, nanovdb_gpu_array, nanovdb_array);

    raster_gaussian_3d(
        compute, queue, cast(ctx), voxel_size, data, nanovdb_gpu_array->device_buffer, nanovdb_word_count, userdata);

    gpu_array_readback(compute, queue, nanovdb_gpu_array, nanovdb_array);

    pnanovdb_uint64_t flushed_frame = 0llu;
    compute->device_interface.flush(queue, &flushed_frame, nullptr, nullptr);

    compute->device_interface.wait_idle(queue);

    // to flush profile
    flushed_frame = 0llu;
    compute->device_interface.flush(queue, &flushed_frame, nullptr, nullptr);

    // restore min lifetime to default
    compute->device_interface.set_resource_min_lifetime(context, 60u);

    compute->device_interface.disable_profiler(context);

    destroy_context(compute, queue, cast(ctx));

    gpu_array_map(compute, queue, nanovdb_gpu_array, nanovdb_array);

    destroy_gaussian_data(compute, queue, data);

    gpu_array_destroy(compute, queue, nanovdb_gpu_array);

    // to flush destroys
    for (pnanovdb_uint32_t flush_count = 0u; flush_count < 64u; flush_count++)
    {
        compute->device_interface.flush(queue, &flushed_frame, nullptr, nullptr);
    }

#if PNANOVDB_RASTER_VALIDATE
    raster_validate(compute, voxel_size, means, colors, nanovdb_array);
#endif

    {
        pnanovdb_uint32_t* mapped_nanovdb = (pnanovdb_uint32_t*)compute->map_array(nanovdb_array);

        pnanovdb_buf_t buf = pnanovdb_make_buf(mapped_nanovdb, nanovdb_array->element_count);

        pnanovdb_grid_handle_t grid = {};
        pnanovdb_uint64_t grid_size = pnanovdb_grid_get_grid_size(buf, grid);
        printf("grid_size(%llu)\n", (unsigned long long int)grid_size);
        pnanovdb_tree_handle_t tree = pnanovdb_grid_get_tree(buf, grid);
        pnanovdb_uint32_t upper_count = pnanovdb_tree_get_node_count_upper(buf, tree);
        pnanovdb_uint32_t lower_count = pnanovdb_tree_get_node_count_lower(buf, tree);
        pnanovdb_uint32_t leaf_count = pnanovdb_tree_get_node_count_leaf(buf, tree);
        printf("upper_count(%llu), lower_count(%llu), leaf_count(%llu)\n", (unsigned long long int)upper_count,
               (unsigned long long int)lower_count, (unsigned long long int)leaf_count);
        pnanovdb_gridblindmetadata_handle_t meta = pnanovdb_grid_get_gridblindmetadata(buf, grid, 1u);
        pnanovdb_uint64_t value_count = pnanovdb_gridblindmetadata_get_value_count(buf, meta);
        pnanovdb_uint32_t value_size = pnanovdb_gridblindmetadata_get_value_size(buf, meta);
        printf("value_count(%llu) value_size(%u)\n", (unsigned long long int)value_count, value_size);

        pnanovdb_address_t bbox_addr = pnanovdb_grid_get_gridblindmetadata_value_address(buf, grid, 0u);
        pnanovdb_coord_t bbox_min = pnanovdb_read_coord(buf, pnanovdb_address_offset(bbox_addr, 0u));
        pnanovdb_coord_t bbox_max = pnanovdb_read_coord(buf, pnanovdb_address_offset(bbox_addr, 12u));
        printf("bbox_min(%d,%d,%d) bbox_max(%d,%d,%d)\n", bbox_min.x, bbox_min.y, bbox_min.z, bbox_max.x, bbox_max.y,
               bbox_max.z);

        // trim element count to save upload size later
        if (grid_size <= nanovdb_array->element_size * nanovdb_array->element_count)
        {
            nanovdb_array->element_count = (grid_size + nanovdb_array->element_size - 1u) / nanovdb_array->element_size;
        }
        printf("nanovdb_array size trimmed to %llu bytes\n",
               (unsigned long long int)nanovdb_array->element_size * nanovdb_array->element_count);

        compute->unmap_array(nanovdb_array);
    }

    return nanovdb_array;
}
}

pnanovdb_raster_t* pnanovdb_get_raster()
{
    static pnanovdb_raster_t raster = { PNANOVDB_REFLECT_INTERFACE_INIT(pnanovdb_raster_t) };

    raster.create_context = pnanovdb_raster::create_context;
    raster.destroy_context = pnanovdb_raster::destroy_context;
    raster.create_gaussian_data = pnanovdb_raster::create_gaussian_data;
    raster.upload_gaussian_data = pnanovdb_raster::upload_gaussian_data;
    raster.destroy_gaussian_data = pnanovdb_raster::destroy_gaussian_data;
    raster.raster_gaussian_3d = pnanovdb_raster::raster_gaussian_3d;
    raster.raster_gaussian_2d = pnanovdb_raster::raster_gaussian_2d;
    raster.raster_to_nanovdb = pnanovdb_raster::raster_to_nanovdb;
    raster.raster_file = pnanovdb_raster::raster_file;
    raster.raster_to_nanovdb_from_arrays = pnanovdb_raster::raster_to_nanovdb_from_arrays;
    raster.create_gaussian_data_from_arrays = pnanovdb_raster::create_gaussian_data_from_arrays;
    raster.create_gaussian_data_from_desc = pnanovdb_raster::create_gaussian_data_from_desc;

    return &raster;
}
