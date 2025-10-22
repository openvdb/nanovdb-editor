// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/editor/Editor.h

    \author Petra Hapalova

    \brief
*/

#include "nanovdb_editor/putil/Reflect.h"
#include "nanovdb_editor/putil/Editor.h"

#include <thread>
#include <atomic>
#include <string>

namespace pnanovdb_editor
{
struct EditorWorker;
class EditorView;
}

struct pnanovdb_editor_impl_t
{
    const pnanovdb_compiler_t* compiler;
    const pnanovdb_compute_t* compute;
    pnanovdb_editor::EditorWorker* editor_worker;
    pnanovdb_compute_array_t* nanovdb_array;
    pnanovdb_compute_array_t* data_array;
    pnanovdb_raster_gaussian_data_t* gaussian_data;
    pnanovdb_camera_t* camera;
    pnanovdb_raster_context_t* raster_ctx;
    void* shader_params;
    const pnanovdb_reflect_data_type_t* shader_params_data_type;
    pnanovdb_editor::EditorView* views;
    pnanovdb_int32_t resolved_port;

    // Resources for _2 API methods
    pnanovdb_raster_t* raster;
    pnanovdb_compute_device_t* device;
    pnanovdb_compute_queue_t* queue;
};

namespace pnanovdb_editor
{
PNANOVDB_API pnanovdb_editor_t* pnanovdb_get_editor();

template <typename T>
struct PendingData
{
    std::atomic<T*> pending_data{ nullptr };

    T* set_pending(T* data)
    {
        return pending_data.exchange(data, std::memory_order_acq_rel);
    }

    // Returns true if there was pending data, and updates current_data/old_data
    bool process_pending(T*& current_data, T*& old_data)
    {
        T* data = pending_data.exchange(nullptr, std::memory_order_acq_rel);
        if (data)
        {
            old_data = current_data;
            current_data = data;
            return true;
        }
        return false;
    }
};

template <typename T>
struct ConstPendingData
{
    std::atomic<const T*> pending_data{ nullptr };

    // Returns previous pointer (if any) so caller can release it if needed
    const T* set_pending(const T* data)
    {
        return pending_data.exchange(data, std::memory_order_acq_rel);
    }

    // Returns true if there was pending data, and updates current_data/old_data
    bool process_pending(const T*& current_data, const T*& old_data)
    {
        const T* data = pending_data.exchange(nullptr, std::memory_order_acq_rel);
        if (data)
        {
            old_data = current_data;
            current_data = data;
            return true;
        }
        return false;
    }
};

struct EditorWorker
{
    std::thread* thread;
    std::atomic<bool> should_stop{ false };
    std::atomic<int> set_params{ 0 };
    std::atomic<int> get_params{ 0 };
    PendingData<pnanovdb_compute_array_t> pending_nanovdb;
    PendingData<pnanovdb_compute_array_t> pending_data_array;
    PendingData<pnanovdb_raster_context_t> pending_raster_ctx;
    PendingData<pnanovdb_raster_gaussian_data_t> pending_gaussian_data;
    PendingData<pnanovdb_camera_t> pending_camera;
    PendingData<void> pending_shader_params;
    ConstPendingData<pnanovdb_reflect_data_type_t> pending_shader_params_data_type;

    pnanovdb_editor_config_t config = {};
    std::string config_ip_address;
    std::string config_ui_profile_name;
};
}
