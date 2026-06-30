// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   editor/PipelineRuntime.h

    \brief  Per-editor async pipeline runtime state
*/

#pragma once

#include "Pipeline.h"
#include "PipelineTypes.h"

#include "nanovdb_editor/putil/Raster.h"
#include "nanovdb_editor/putil/VoxelBVH.h"
#include "nanovdb_editor/putil/WorkerThread.hpp"

#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#if defined(_WIN32)
#    if defined(pnanovdbeditor_EXPORTS)
#        define PNANOVDB_PIPELINE_RUNTIME_EXPORT_CXX __declspec(dllexport)
#    else
#        define PNANOVDB_PIPELINE_RUNTIME_EXPORT_CXX __declspec(dllimport)
#    endif
#else
#    define PNANOVDB_PIPELINE_RUNTIME_EXPORT_CXX __attribute__((visibility("default")))
#endif

namespace pnanovdb_editor
{

struct SceneObject;
struct PipelineStage;
class EditorSceneManager;
class EditorScene;
class PipelineRuntime;

struct GaussianVoxelizeParams
{
    float voxels_per_unit = k_default_voxels_per_unit;
};

struct MeshLoadParams
{
    float inflation_radius = 0.f; //!< 0 = auto for line-based renders
    pnanovdb_uint32_t resolution = k_default_bvh_resolution; //!< 1..k_max_bvh_resolution
    pnanovdb_uint32_t show_debug = 0u; //!< nonzero -> triangles_debug_render
};

#define PNANOVDB_REFLECT_TYPE GaussianVoxelizeParams
PNANOVDB_REFLECT_BEGIN()
PNANOVDB_REFLECT_VALUE(float, voxels_per_unit, 0, 0)
PNANOVDB_REFLECT_END(0)
#undef PNANOVDB_REFLECT_TYPE

PNANOVDB_REFLECT_STRUCT_OPAQUE_IMPL(MeshLoadParams)

namespace detail
{
template <typename Params, typename Field>
inline Field params_field_get(const pnanovdb_pipeline_params_t* params, Field Params::*member, Field fallback)
{
    if (!params || !params->data || params->size < sizeof(Params))
    {
        return fallback;
    }
    return static_cast<const Params*>(params->data)->*member;
}

template <typename Params, typename Field>
inline bool params_field_set(pnanovdb_pipeline_params_t* params, Field Params::*member, Field value)
{
    if (!params || !params->data || params->size < sizeof(Params))
    {
        return false;
    }
    static_cast<Params*>(params->data)->*member = value;
    return true;
}
} // namespace detail

inline float voxels_per_unit_clamp(float value)
{
    if (!std::isfinite(value))
    {
        return k_default_voxels_per_unit;
    }
    return std::clamp(value, 1.0f, 512.0f);
}

inline float pipeline_params_get_voxels_per_unit(const pnanovdb_pipeline_params_t* params)
{
    return voxels_per_unit_clamp(
        detail::params_field_get(params, &GaussianVoxelizeParams::voxels_per_unit, k_default_voxels_per_unit));
}

inline bool pipeline_params_set_voxels_per_unit(pnanovdb_pipeline_params_t* params, float value)
{
    return detail::params_field_set(params, &GaussianVoxelizeParams::voxels_per_unit, voxels_per_unit_clamp(value));
}

inline float pipeline_params_get_mesh_load_inflation_radius(const pnanovdb_pipeline_params_t* params)
{
    return detail::params_field_get(params, &MeshLoadParams::inflation_radius, 0.f);
}

inline bool pipeline_params_set_mesh_load_inflation_radius(pnanovdb_pipeline_params_t* params, float value)
{
    return detail::params_field_set(params, &MeshLoadParams::inflation_radius, value);
}

inline pnanovdb_uint32_t pipeline_params_get_mesh_load_resolution(const pnanovdb_pipeline_params_t* params)
{
    return detail::params_field_get(params, &MeshLoadParams::resolution, k_default_bvh_resolution);
}

inline bool pipeline_params_set_mesh_load_resolution(pnanovdb_pipeline_params_t* params, pnanovdb_uint32_t value)
{
    return detail::params_field_set(params, &MeshLoadParams::resolution, value);
}

inline bool pipeline_params_get_mesh_load_show_debug(const pnanovdb_pipeline_params_t* params)
{
    return detail::params_field_get<MeshLoadParams, pnanovdb_uint32_t>(params, &MeshLoadParams::show_debug, 0u) != 0u;
}

inline bool pipeline_params_set_mesh_load_show_debug(pnanovdb_pipeline_params_t* params, bool value)
{
    return detail::params_field_set<MeshLoadParams, pnanovdb_uint32_t>(
        params, &MeshLoadParams::show_debug, value ? 1u : 0u);
}

inline void pipeline_params_release(pnanovdb_pipeline_params_t* params)
{
    if (!params)
    {
        return;
    }
    free(params->data);
    *params = {};
}

inline void pipeline_params_assign_copy(const pnanovdb_pipeline_params_t* src, pnanovdb_pipeline_params_t* dst)
{
    if (!dst)
    {
        return;
    }
    pipeline_params_release(dst);
    if (!src || !src->data || src->size == 0)
    {
        return;
    }
    void* copy = malloc(src->size);
    if (!copy)
    {
        return;
    }
    memcpy(copy, src->data, src->size);
    dst->data = copy;
    dst->size = src->size;
    dst->type = src->type;
}

inline void release_compute_array(const pnanovdb_compute_t* compute, pnanovdb_compute_array_t*& arr)
{
    if (compute && arr)
    {
        compute->destroy_array(arr);
    }
    arr = nullptr;
}

// ============================================================================
// RasterFileShaderParams - shared shader params for raster file imports
// ============================================================================

class RasterFileShaderParams
{
public:
    RasterFileShaderParams() = default;
    ~RasterFileShaderParams();

    RasterFileShaderParams(const RasterFileShaderParams&) = delete;
    RasterFileShaderParams& operator=(const RasterFileShaderParams&) = delete;

    void prepare(const pnanovdb_compute_t* compute, EditorSceneManager* scene_manager);
    void reset();

    pnanovdb_compute_array_t** arrays()
    {
        return m_arrays;
    }
    pnanovdb_raster_shader_params_t* params()
    {
        return &m_params;
    }

private:
    const pnanovdb_compute_t* m_compute = nullptr;
    const pnanovdb_reflect_data_type_t* m_data_type = nullptr;
    pnanovdb_compute_array_t* m_arrays[pnanovdb_raster::shader_param_count] = {};
    pnanovdb_raster_shader_params_t m_params{};
    bool m_initialized = false;
};

// ============================================================================
// VoxelBVH build request
// ============================================================================

enum class VoxelBVHBuildSource
{
    GaussianFile = 0,
    Triangles = 1,
    Lines = 2,
    GaussianArrays = 3,
};

struct VoxelBVHBuildRequest
{
    static constexpr int k_max_arrays = 6;

    VoxelBVHBuildSource source = VoxelBVHBuildSource::GaussianFile;
    pnanovdb_uint32_t resolution = k_default_bvh_resolution;
    float inflation_radius = 0.f;
    std::string filepath;
    std::shared_ptr<pnanovdb_compute_array_t> array_owners[k_max_arrays];
    pnanovdb_compute_array_t* array_ptrs[k_max_arrays] = {};
};

inline bool voxelbvh_interface_supports_user_cancel(const pnanovdb_voxelbvh_t* iface)
{
    return iface && iface->context_set_cancel;
}

/*!
    \brief Base class for per-pipeline async workers.
*/
class PNANOVDB_PIPELINE_RUNTIME_EXPORT_CXX AsyncWorker
{
public:
    virtual ~AsyncWorker() = default;

    AsyncWorker(const AsyncWorker&) = delete;
    AsyncWorker& operator=(const AsyncWorker&) = delete;
    AsyncWorker(AsyncWorker&&) = delete;
    AsyncWorker& operator=(AsyncWorker&&) = delete;

    void cancel_and_join()
    {
        request_cancel();
        m_worker.reset();
        m_enqueued = false;
        m_task_id = pnanovdb_util::WorkerThread::invalidTaskId();
    }

    void release()
    {
        if (m_released)
        {
            return;
        }
        cancel_and_join();
        release_after_join();
    }

    void release_after_join()
    {
        if (m_released)
        {
            return;
        }
        m_released = true;
        release_resources();
        clear_pending_cancel_ui();
        finish_task();
    }

    void prepare_for_session()
    {
        if (!m_released)
        {
            return;
        }
        m_worker = std::make_unique<pnanovdb_util::WorkerThread>();
        m_task_id = pnanovdb_util::WorkerThread::invalidTaskId();
        m_enqueued = false;
        m_released = false;
    }

    bool is_running() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_worker || !m_enqueued)
        {
            return false;
        }
        return !m_worker->isTaskCompleted(m_task_id);
    }

    bool is_completed() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_worker && m_worker->isTaskCompleted(m_task_id);
    }

    bool is_busy() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_enqueued;
    }

    bool get_progress(std::string& text, float& value);

    bool pending_target_matches(uint64_t scene_id, uint64_t name_id, uint64_t lifetime_id) const;
    bool retarget_pending_target(
        uint64_t old_scene_id, uint64_t old_name_id, uint64_t new_scene_id, uint64_t new_name_id, uint64_t lifetime_id);

    virtual bool handle_completion() = 0;

    virtual pnanovdb_pipeline_type_t pipeline_type() const = 0;

    virtual bool supports_user_cancel() const
    {
        return false;
    }

    void request_user_cancel(SceneObject* scene_obj = nullptr);
    bool user_cancel_requested() const;

    // Worker has a finished task waiting for handle_completion()
    bool pending_completion() const
    {
        return is_busy() && is_completed();
    }

    virtual void init(const PipelineContext& ctx, EditorScene* editor_scene)
    {
        (void)ctx;
        (void)editor_scene;
    }

    // Optional load-stage entry point
    virtual bool start_from_request(const PipelineLoadRequest& request,
                                    EditorSceneManager* scene_manager,
                                    pnanovdb_editor_token_t* scene_token)
    {
        (void)request;
        (void)scene_manager;
        (void)scene_token;
        return false;
    }

protected:
    AsyncWorker() = default;

    virtual void release_resources()
    {
    }

    virtual void request_cancel()
    {
    }

    virtual const char* progress_waiting_text() const
    {
        return "Waiting for worker...";
    }

    virtual const char* progress_running_fallback_text() const
    {
        return "";
    }

    void set_pending_target(SceneObject* scene_obj, EditorSceneManager* scene_manager, const pnanovdb_compute_t* compute);

    void pending_target_tokens(pnanovdb_editor_token_t** scene, pnanovdb_editor_token_t** name) const;

    bool with_pending_object(const std::function<void(SceneObject*)>& fn);

    bool with_pending_process_step(const std::function<void(SceneObject*, PipelineStage&)>& fn);

    bool consume_user_cancelled();

    void clear_pending_cancel_ui();

    void finish_task()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_worker)
        {
            m_worker->removeCompletedTask(m_task_id);
        }
        m_pending_scene_token_id = 0;
        m_pending_name_token_id = 0;
        m_pending_object_lifetime_id = 0;
        m_pending_scene_manager = nullptr;
        m_pending_compute = nullptr;
        m_pending_process_step = -1;
        m_pending_process_type = pnanovdb_pipeline_type_noop;
        m_pending_process_revision = 0;
        m_enqueued = false;
        m_user_cancelled = false;
        m_cancel_snapshot_restored = false;
    }

    std::unique_ptr<pnanovdb_util::WorkerThread> m_worker = std::make_unique<pnanovdb_util::WorkerThread>();
    pnanovdb_util::WorkerThread::TaskId m_task_id = pnanovdb_util::WorkerThread::invalidTaskId();
    bool m_enqueued = false;
    mutable std::mutex m_mutex;

    uint64_t m_pending_scene_token_id = 0;
    uint64_t m_pending_name_token_id = 0;
    uint64_t m_pending_object_lifetime_id = 0;
    EditorSceneManager* m_pending_scene_manager = nullptr;
    const pnanovdb_compute_t* m_pending_compute = nullptr;
    int m_pending_process_step = -1;
    pnanovdb_pipeline_type_t m_pending_process_type = pnanovdb_pipeline_type_noop;
    uint64_t m_pending_process_revision = 0;
    bool m_released = false;
    bool m_user_cancelled = false;
    // request_user_cancel() restores the Run snapshot immediately. Remember
    // that fact so completion does not mistake the consumed snapshot for a
    // configuration edit and clear dirty work that the snapshot restored.
    bool m_cancel_snapshot_restored = false;
};

// ============================================================================
// GaussianVoxelizeWorker - Gaussian -> NanoVDB via raster_file (gaussian_voxelize pipeline)
// ============================================================================

class GaussianVoxelizeWorker : public AsyncWorker
{
public:
    static constexpr pnanovdb_pipeline_type_t kPipelineType = pnanovdb_pipeline_type_gaussian_voxelize;

    GaussianVoxelizeWorker() = default;
    ~GaussianVoxelizeWorker() override;

    pnanovdb_pipeline_type_t pipeline_type() const override
    {
        return kPipelineType;
    }
    void init(const PipelineContext& ctx, EditorScene* editor_scene) override;

    bool start(SceneObject* scene_obj, EditorSceneManager* scene_manager, const pnanovdb_pipeline_context_t* ctx);
    bool handle_completion() override;

    float get_running_voxels_per_unit();

protected:
    void release_resources() override;

private:
    EditorScene* m_editor_scene = nullptr;

    pnanovdb_compute_array_t* m_pending_nanovdb_array = nullptr;
    std::string m_pending_filepath;
    pnanovdb_pipeline_params_t m_pending_params = {};

    RasterFileShaderParams m_shader_params;
};

// ============================================================================
// VoxelBVHWorker - Gaussian/triangles/lines/arrays -> NanoVDB via BVH
// ============================================================================

class PNANOVDB_PIPELINE_RUNTIME_EXPORT_CXX VoxelBVHWorker : public AsyncWorker
{
public:
    static constexpr pnanovdb_pipeline_type_t kPipelineType = pnanovdb_pipeline_type_voxelbvh_build;

    VoxelBVHWorker() = default;
    ~VoxelBVHWorker() override;

    pnanovdb_pipeline_type_t pipeline_type() const override
    {
        return kPipelineType;
    }
    bool supports_user_cancel() const override
    {
        return voxelbvh_interface_supports_user_cancel(m_iface);
    }
    void init(const PipelineContext& ctx, EditorScene* editor_scene) override
    {
        (void)ctx;
        m_editor_scene = editor_scene;
    }

    bool start(SceneObject* scene_obj,
               EditorSceneManager* scene_manager,
               const pnanovdb_pipeline_context_t* ctx,
               VoxelBVHBuildRequest req);
    bool handle_completion() override;

protected:
    void release_resources() override;

    void request_cancel() override
    {
        m_cancel.store(1u, std::memory_order_relaxed);
    }

    const char* progress_running_fallback_text() const override
    {
        return "Building VoxelBVH...";
    }

private:
    void finish_file_replacement(bool success);

    EditorScene* m_editor_scene = nullptr;
    pnanovdb_voxelbvh_t* m_iface = nullptr;
    pnanovdb_voxelbvh_context_t* m_worker_ctx = nullptr;
    pnanovdb_compute_queue_t* m_worker_queue = nullptr;

    VoxelBVHBuildRequest m_pending_request;
    pnanovdb_compute_array_t* m_pending_result = nullptr;
    std::atomic<pnanovdb_uint32_t> m_cancel{ 0u };
};

// ============================================================================
// VoxelBVHRgba8Worker - VoxelBVH NanoVDB grid -> RGBA8 NanoVDB image grid
// ============================================================================

class VoxelBVHRgba8Worker : public AsyncWorker
{
public:
    static constexpr pnanovdb_pipeline_type_t kPipelineType = pnanovdb_pipeline_type_voxelbvh_rgba8;

    VoxelBVHRgba8Worker() = default;
    ~VoxelBVHRgba8Worker() override;

    pnanovdb_pipeline_type_t pipeline_type() const override
    {
        return kPipelineType;
    }
    bool supports_user_cancel() const override
    {
        return voxelbvh_interface_supports_user_cancel(m_iface);
    }

    bool start(SceneObject* scene_obj,
               EditorSceneManager* scene_manager,
               const pnanovdb_pipeline_context_t* ctx,
               pnanovdb_compute_array_t* src_nanovdb,
               std::shared_ptr<pnanovdb_compute_array_t> src_owner,
               pnanovdb_uint32_t resolution,
               pnanovdb_bool_t upsample);
    bool handle_completion() override;

protected:
    void release_resources() override;

    void request_cancel() override
    {
        m_cancel.store(1u, std::memory_order_relaxed);
    }

    const char* progress_running_fallback_text() const override
    {
        return "Converting VoxelBVH to RGBA8...";
    }

private:
    pnanovdb_voxelbvh_t* m_iface = nullptr;
    pnanovdb_voxelbvh_context_t* m_worker_ctx = nullptr;
    pnanovdb_compute_queue_t* m_worker_queue = nullptr;

    pnanovdb_compute_array_t* m_pending_src = nullptr;
    std::shared_ptr<pnanovdb_compute_array_t> m_pending_src_owner;
    pnanovdb_uint32_t m_pending_resolution = 0u;
    pnanovdb_bool_t m_pending_upsample = PNANOVDB_FALSE;
    pnanovdb_compute_array_t* m_pending_result = nullptr;

    std::atomic<pnanovdb_uint32_t> m_cancel{ 0u };
};

// ============================================================================
// GaussianLoadWorker - file import -> Gaussian splats / NanoVDB (gaussian_load pipeline)
// ============================================================================

class GaussianLoadWorker : public AsyncWorker
{
public:
    static constexpr pnanovdb_pipeline_type_t kPipelineType = pnanovdb_pipeline_type_gaussian_load;

    GaussianLoadWorker() = default;
    ~GaussianLoadWorker() override;

    pnanovdb_pipeline_type_t pipeline_type() const override
    {
        return kPipelineType;
    }
    void init(const PipelineContext& ctx, EditorScene* editor_scene) override;

    bool handle_completion() override;

    bool start_from_request(const PipelineLoadRequest& request,
                            EditorSceneManager* scene_manager,
                            pnanovdb_editor_token_t* scene_token) override;

protected:
    void release_resources() override;

private:
    bool start(const char* raster_filepath,
               const pnanovdb_pipeline_params_t* process_params,
               bool rasterize_to_nanovdb,
               pnanovdb_pipeline_type_t process_pipeline,
               pnanovdb_pipeline_type_t render_pipeline,
               EditorSceneManager* scene_manager,
               pnanovdb_editor_token_t* scene_token,
               pnanovdb_editor_token_t* name_token,
               uint64_t reservation_id);

    pnanovdb_compute_queue_t* cleanup_queue() const
    {
        return m_compute_queue ? m_compute_queue : m_device_queue;
    }

    void release_pending_resources();

    const pnanovdb_compute_t* m_compute = nullptr;
    pnanovdb_raster_t* m_raster = nullptr;
    pnanovdb_compute_queue_t* m_device_queue = nullptr;
    pnanovdb_compute_queue_t* m_compute_queue = nullptr;

    EditorScene* m_editor_scene = nullptr;

    // Pending rasterization state
    std::string m_pending_filepath;
    pnanovdb_raster_gaussian_data_t* m_pending_gaussian_data = nullptr;
    pnanovdb_raster_context_t* m_pending_raster_ctx = nullptr;
    pnanovdb_compute_array_t* m_pending_nanovdb_array = nullptr;
    pnanovdb_editor_token_t* m_pending_scene_token = nullptr;
    pnanovdb_editor_token_t* m_pending_name_token = nullptr;
    uint64_t m_pending_reservation_id = 0;

    // Pipeline config for pending import
    pnanovdb_pipeline_type_t m_pending_process_pipeline = pnanovdb_pipeline_type_noop;
    pnanovdb_pipeline_type_t m_pending_render_pipeline = pnanovdb_pipeline_type_gaussian_splat;
    pnanovdb_pipeline_params_t m_pending_process_params = {};

    RasterFileShaderParams m_shader_params;
};

// ============================================================================
// MeshLoadWorker - PLY file import -> position/index/(color) compute arrays
// ============================================================================

class MeshLoadWorker : public AsyncWorker
{
public:
    static constexpr pnanovdb_pipeline_type_t kPipelineType = pnanovdb_pipeline_type_mesh_load;

    MeshLoadWorker() = default;
    ~MeshLoadWorker() override;

    pnanovdb_pipeline_type_t pipeline_type() const override
    {
        return kPipelineType;
    }
    void init(const PipelineContext& ctx, EditorScene* editor_scene) override;

    bool start(const char* mesh_filepath,
               const MeshLoadParams& load_params,
               pnanovdb_editor_token_t* scene_token,
               pnanovdb_editor_token_t* name_token,
               uint64_t reservation_id);

    bool handle_completion() override;

    bool start_from_request(const PipelineLoadRequest& request,
                            EditorSceneManager* scene_manager,
                            pnanovdb_editor_token_t* scene_token) override;

protected:
    void release_resources() override;

    const char* progress_running_fallback_text() const override
    {
        return "Loading mesh...";
    }

private:
    void release_pending_arrays();

    const pnanovdb_compute_t* m_compute = nullptr;
    EditorScene* m_editor_scene = nullptr;

    // Worker-thread output staging; nulled after handover to the scene.
    std::string m_pending_filepath;
    MeshLoadParams m_pending_params{};
    pnanovdb_editor_token_t* m_pending_scene_token = nullptr;
    pnanovdb_editor_token_t* m_pending_name_token = nullptr;
    uint64_t m_pending_reservation_id = 0;
    pnanovdb_pipeline_type_t m_pending_process_pipeline = pnanovdb_pipeline_type_voxelbvh_build;

    pnanovdb_compute_array_t* m_pending_positions = nullptr;
    pnanovdb_compute_array_t* m_pending_indices = nullptr;
    pnanovdb_compute_array_t* m_pending_colors = nullptr;
};

// ============================================================================
// PipelineRuntime - container for all per-editor pipeline state
// ============================================================================

class PipelineRuntime
{
public:
    PipelineRuntime();
    ~PipelineRuntime();

    PipelineRuntime(const PipelineRuntime&) = delete;
    PipelineRuntime& operator=(const PipelineRuntime&) = delete;
    PipelineRuntime(PipelineRuntime&&) = delete;
    PipelineRuntime& operator=(PipelineRuntime&&) = delete;

    const std::vector<std::unique_ptr<AsyncWorker>>& workers() const
    {
        return m_workers;
    }

    AsyncWorker* worker_for(pnanovdb_pipeline_type_t type) const
    {
        for (const auto& w : m_workers)
        {
            if (w && w->pipeline_type() == type)
            {
                return w.get();
            }
        }
        return nullptr;
    }

    template <typename T>
    T* worker() const
    {
        AsyncWorker* w = worker_for(T::kPipelineType);
        return w ? static_cast<T*>(w) : nullptr;
    }

    // ------------------------------------------------------------------------
    // Single in-flight async task invariant
    // ------------------------------------------------------------------------
    AsyncWorker* running_worker() const
    {
        for (const auto& w : m_workers)
        {
            if (w && w->is_running())
            {
                return w.get();
            }
        }
        return nullptr;
    }

    bool any_worker_running() const
    {
        return running_worker() != nullptr;
    }

    AsyncWorker* busy_worker() const
    {
        for (const auto& w : m_workers)
        {
            if (w && w->is_busy())
            {
                return w.get();
            }
        }
        return nullptr;
    }

    bool any_worker_busy() const
    {
        return busy_worker() != nullptr;
    }

    void set_editor_scene(EditorScene* editor_scene);
    void begin_quiesce();
    void finish_quiesce();
    EditorScene* editor_scene() const
    {
        return m_editor_scene;
    }

    void handle_completions()
    {
        for (const auto& w : m_workers)
        {
            if (w && w->pending_completion())
            {
                w->handle_completion();
            }
        }
    }

    void quiesce();

private:
    std::vector<std::unique_ptr<AsyncWorker>> m_workers;
    EditorScene* m_editor_scene = nullptr;
    bool m_session_active = false;
};

// ============================================================================
// Worker factory registry (powers PNANOVDB_REGISTER_WORKER)
// ============================================================================

using AsyncWorkerFactory = std::unique_ptr<AsyncWorker> (*)();

namespace detail
{
void register_worker_factory(AsyncWorkerFactory factory);
const std::vector<AsyncWorkerFactory>& get_worker_factories();
} // namespace detail

struct AsyncWorkerRegistrar
{
    explicit AsyncWorkerRegistrar(AsyncWorkerFactory factory)
    {
        detail::register_worker_factory(factory);
    }
};

#define PNANOVDB_REGISTER_WORKER(WorkerType)                                                                           \
    static const ::pnanovdb_editor::AsyncWorkerRegistrar WorkerType##_async_worker_registrar(                          \
        []() -> std::unique_ptr<::pnanovdb_editor::AsyncWorker> { return std::make_unique<WorkerType>(); })

PipelineRuntime* current_runtime();

template <typename Fallback, typename F>
auto with_runtime(Fallback fallback, F&& fn) -> Fallback
{
    if (PipelineRuntime* rt = current_runtime())
    {
        return fn(*rt);
    }
    return fallback;
}

namespace detail
{
void set_current_runtime(PipelineRuntime* runtime);
} // namespace detail

class RuntimeScope
{
public:
    explicit RuntimeScope(PipelineRuntime* runtime);
    ~RuntimeScope();

    RuntimeScope(const RuntimeScope&) = delete;
    RuntimeScope& operator=(const RuntimeScope&) = delete;
    RuntimeScope(RuntimeScope&&) = delete;
    RuntimeScope& operator=(RuntimeScope&&) = delete;

private:
    PipelineRuntime* m_previous;
};

} // namespace pnanovdb_editor
