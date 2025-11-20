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

// Shader constants
constexpr const char* s_default_editor_shader = "editor/editor.slang";
constexpr const char* s_raster2d_shader_group = "raster/raster2d_group";
constexpr const char* s_raster2d_gaussian_shader = "raster/gaussian_rasterize_2d.slang";
}

// Thread Synchronization Model
// ----------------------------
// Worker Thread              Render Thread
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
    std::string shader_name = pnanovdb_editor::s_default_editor_shader;
    void* shader_params;
    const pnanovdb_reflect_data_type_t* shader_params_data_type;

    // Deferred gaussian data destruction (to avoid GPU accessing freed memory)
    std::shared_ptr<pnanovdb_raster_gaussian_data_t> gaussian_data_old;
    std::vector<std::shared_ptr<pnanovdb_raster_gaussian_data_t>> gaussian_data_destruction_queue_pending;
    std::vector<std::shared_ptr<pnanovdb_raster_gaussian_data_t>> gaussian_data_destruction_queue_ready;

    pnanovdb_editor_config_t config = {};
    std::string config_ip_address;
    std::string config_ui_profile_name;

    pnanovdb_int32_t resolved_port;

    std::atomic<bool> show_active{ false };

    // Temporary buffer for get_camera() to return scene-specific camera
    pnanovdb_camera_t* scene_camera;
    std::mutex scene_camera_mutex;
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

// Pending removal request for deferred execution
// If name is nullptr, this signals removal of the entire scene (after all objects are removed)
struct PendingRemoval
{
    pnanovdb_editor_token_t* scene;
    pnanovdb_editor_token_t* name;
};

struct EditorWorker
{
    std::thread* thread;
    std::atomic<bool> should_stop{ false };
    std::atomic<bool> is_starting{ true };
    std::atomic<bool> params_dirty{ false };
    std::atomic<bool> views_need_sync{ false }; // Signal that views need to sync from scene_manager
    std::recursive_mutex shader_params_mutex; // TODO: Use mutex per map_params()/unmap_params() call
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

    // Deferred removal queue (worker thread adds, render thread processes)
    std::mutex pending_removals_mutex;
    std::vector<PendingRemoval> pending_removals;

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

} // namespace pnanovdb_editor
