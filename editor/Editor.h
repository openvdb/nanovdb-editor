// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/editor/Editor.h

    \author Petra Hapalova

    \brief
*/

#include "nanovdb_editor/putil/Reflect.h"
#include "nanovdb_editor/putil/Editor.h"
#include "nanovdb_editor/putil/VoxelBVH.h"
#include "PipelineTypes.h"
#include "PipelineRegistry.h"

#include <thread>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <memory>

namespace pnanovdb_editor
{
struct EditorWorker;
class EditorSceneManager;
class SceneView;
class Renderer;
class EditorScene;
class ParamMapRegistry;
class PipelineRuntime;
}

// Thread Synchronization Model
// ----------------------------
// External Caller            Render Thread
// ━━━━━━━━━━━━━              ━━━━━━━━━━━━━
// add_xyz()                  show() render loop
//   └─ scene_manager (mutex)   ├─ scen_views (no mutex, render thread only)
//   └─ set views_need_sync ───►└─ sync_views_from_scene_manager()
//                                    └─ for_each_object() (mutex held)

struct pnanovdb_editor_impl_t
{
    pnanovdb_editor::EditorWorker* editor_worker;
    pnanovdb_editor::EditorSceneManager* scene_manager;
    pnanovdb_editor::SceneView* scene_view;
    pnanovdb_editor::Renderer* renderer;
    pnanovdb_editor::EditorScene* editor_scene;
    pnanovdb_editor::ParamMapRegistry* param_map_registry;
    std::unique_ptr<pnanovdb_editor::PipelineRuntime> pipeline_runtime;
    std::string pending_scene_path;
    pnanovdb_bool_t pending_scene_overwrite;

    // Currently used by the render thread in show()
    const pnanovdb_compiler_t* compiler;
    const pnanovdb_compute_t* compute;
    pnanovdb_compute_device_t* device;
    pnanovdb_compute_queue_t* device_queue;
    pnanovdb_compute_queue_t* compute_queue;
    pnanovdb_compute_array_t* nanovdb_array;
    pnanovdb_compute_array_t* data_array;
    pnanovdb_raster_gaussian_data_t* gaussian_data;
    pnanovdb_camera_t* camera;
    pnanovdb_camera_view_t* camera_view;
    pnanovdb_raster_t* raster;
    pnanovdb_raster_context_t* raster_ctx;
    pnanovdb_voxelbvh_t* voxelbvh;
    pnanovdb_voxelbvh_context_t* voxelbvh_ctx;
    std::string shader_name = pnanovdb_pipeline_get_shader_name(pnanovdb_pipeline_type_nanovdb_render);
    void* shader_params;
    const pnanovdb_reflect_data_type_t* shader_params_data_type;

    // Deferred gaussian data destruction (to avoid GPU accessing freed memory)
    std::shared_ptr<pnanovdb_raster_gaussian_data_t> gaussian_data_old;
    std::vector<std::shared_ptr<pnanovdb_raster_gaussian_data_t>> gaussian_data_destruction_queue_pending;
    std::vector<std::shared_ptr<pnanovdb_raster_gaussian_data_t>> gaussian_data_destruction_queue_ready;

    pnanovdb_editor_config_t config = {};
    std::string config_ip_address;
    std::string config_ui_profile_name;

    std::atomic<pnanovdb_int32_t> resolved_port;

    std::atomic<bool> show_active{ false };

    // Temporary buffer for get_camera() to return scene-specific camera
    pnanovdb_camera_t* scene_camera;
    std::mutex scene_camera_mutex;
};

namespace pnanovdb_editor
{
PNANOVDB_API pnanovdb_editor_t* pnanovdb_get_editor();

void defer_gaussian_data_destruction(pnanovdb_editor_impl_t* impl,
                                     std::shared_ptr<pnanovdb_raster_gaussian_data_t> owner);

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

struct RenderThreadTaskQueue
{
    struct Task
    {
        std::function<pnanovdb_bool_t()> run;
        bool blocking = true;
        std::mutex mutex;
        std::condition_variable cv;
        bool done = false;
        pnanovdb_bool_t result = PNANOVDB_FALSE;
    };

    std::mutex mutex;
    std::deque<std::shared_ptr<Task>> tasks;
    bool accepting = true;

    // Any thread: enqueue and block until the render thread runs the task.
    // Returns PNANOVDB_FALSE without running if the queue was already closed.
    pnanovdb_bool_t run_blocking(std::function<pnanovdb_bool_t()> fn)
    {
        auto task = std::make_shared<Task>();
        task->run = std::move(fn);
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!accepting)
                return PNANOVDB_FALSE;
            tasks.push_back(task);
        }
        std::unique_lock<std::mutex> lock(task->mutex);
        task->cv.wait(lock, [&task]() { return task->done; });
        return task->result;
    }

    // Any thread: enqueue fire-and-forget work for the render thread. Dropped
    // (never run) if the queue is closed, matching the old pending-queue teardown.
    void run_async(std::function<void()> fn)
    {
        auto task = std::make_shared<Task>();
        task->blocking = false;
        task->run = [fn = std::move(fn)]()
        {
            fn();
            return PNANOVDB_TRUE;
        };
        std::lock_guard<std::mutex> lock(mutex);
        if (!accepting)
            return;
        tasks.push_back(std::move(task));
    }

    // Render thread: run every queued task; wake blocking callers with the result.
    void drain()
    {
        for (auto& task : take())
        {
            const pnanovdb_bool_t result = task->run();
            if (task->blocking)
                complete(task, result);
        }
    }

    // Stop accepting; fail blocking callers and drop async tasks.
    void close()
    {
        for (auto& task : take(/*stop_accepting*/ true))
        {
            if (task->blocking)
                complete(task, PNANOVDB_FALSE);
        }
    }

private:
    std::deque<std::shared_ptr<Task>> take(bool stop_accepting = false)
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (stop_accepting)
            accepting = false;
        std::deque<std::shared_ptr<Task>> pending;
        pending.swap(tasks);
        return pending;
    }

    static void complete(const std::shared_ptr<Task>& task, pnanovdb_bool_t result)
    {
        {
            std::lock_guard<std::mutex> lock(task->mutex);
            task->result = result;
            task->done = true;
        }
        task->cv.notify_one();
    }
};

struct EditorWorker
{
    std::thread* thread = nullptr;
    std::atomic<bool> should_stop{ false };
    std::atomic<bool> is_starting{ true };
    std::atomic<bool> params_dirty{ false };
    std::atomic<bool> views_need_sync{ false }; // Signal that views need to sync from scene_manager
    std::recursive_mutex shader_params_mutex; // TODO: Use mutex per map_params()/unmap_params() call
    std::recursive_mutex pipeline_params_mutex; // Protects pipeline stage params during map/unmap
    std::atomic<bool> pipeline_params_dirty{ false }; // Signal that pipeline params were modified
    PendingData<pnanovdb_compute_array_t> pending_nanovdb;
    PendingData<pnanovdb_compute_array_t> pending_data_array;
    PendingData<pnanovdb_raster_gaussian_data_t> pending_gaussian_data;
    PendingData<pnanovdb_camera_t> pending_camera;
    PendingData<pnanovdb_camera_view_t> pending_camera_view[32];
    std::atomic<uint32_t> pending_camera_view_idx;
    PendingData<void> pending_shader_params;
    ConstPendingData<pnanovdb_reflect_data_type_t> pending_shader_params_data_type;

    // Track last added view to auto-select after sync
    std::atomic<uint64_t> last_added_scene_token_id{ 0 };
    std::atomic<uint64_t> last_added_name_token_id{ 0 };

    RenderThreadTaskQueue render_thread_tasks;

    pnanovdb_editor_config_t config = {};
    std::string config_ip_address;
    std::string config_ui_profile_name;
};

// ------------------------------------------------ Token Utilities
// Compare two tokens for equality (fast comparison using IDs)
static inline pnanovdb_bool_t pnanovdb_editor_token_equal(const pnanovdb_editor_token_t* a,
                                                          const pnanovdb_editor_token_t* b)
{
    if (a == b)
        return PNANOVDB_TRUE;
    if (!a || !b)
        return PNANOVDB_FALSE;
    return (a->id == b->id) ? PNANOVDB_TRUE : PNANOVDB_FALSE;
}

// Get the string representation of a token (safe - returns NULL if token is NULL)
static inline const char* pnanovdb_editor_token_get_string(const pnanovdb_editor_token_t* token)
{
    return token ? token->str : NULL;
}

// Get the unique ID of a token
static inline pnanovdb_uint64_t pnanovdb_editor_token_get_id(const pnanovdb_editor_token_t* token)
{
    return token ? token->id : 0;
}

// Check if a token is valid (not NULL)
static inline pnanovdb_bool_t pnanovdb_editor_token_is_valid(const pnanovdb_editor_token_t* token)
{
    return token ? PNANOVDB_TRUE : PNANOVDB_FALSE;
}
// -----------------------------------------------------------

// Forward declaration for execute_removal function
void execute_removal(pnanovdb_editor_t* editor, pnanovdb_editor_token_t* scene, pnanovdb_editor_token_t* name);

// View selection
void select_render_view(pnanovdb_editor_t* editor, pnanovdb_editor_token_t* scene, pnanovdb_editor_token_t* name);

} // namespace pnanovdb_editor
