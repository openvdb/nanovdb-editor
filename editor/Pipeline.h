// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   editor/Pipeline.h

    \author Petra Hapalova, Andrew Reidmeyer

    \brief  This file provides pipeline interface and registration system.
*/

#pragma once

#include "nanovdb_editor/putil/Editor.h"
#include "nanovdb_editor/putil/Reflect.h"
#include "nanovdb_editor/putil/Raster.h"
#include "nanovdb_editor/putil/VoxelBVH.h"
#include "PipelineTypes.h"
#include "PipelineRegistry.h"
#include <memory>
#include <string>

// Opaque types for pipeline context (internal editor types)
struct pnanovdb_renderer_t;
typedef struct pnanovdb_renderer_t pnanovdb_renderer_t;

struct pnanovdb_scene_manager_t;
typedef struct pnanovdb_scene_manager_t pnanovdb_scene_manager_t;

struct pnanovdb_pipeline_context_t
{
    const pnanovdb_compute_t* compute;
    pnanovdb_compute_device_t* device;
    pnanovdb_compute_queue_t* queue;
    pnanovdb_compute_queue_t* compute_queue;
    pnanovdb_raster_t* raster;
    pnanovdb_raster_context_t* raster_ctx;
    pnanovdb_voxelbvh_t* voxelbvh;
    pnanovdb_voxelbvh_context_t* voxelbvh_ctx;
    pnanovdb_renderer_t* renderer;
    pnanovdb_scene_manager_t* scene_manager;
};

// ============================================================================
// Pipeline Registry
// ============================================================================

typedef enum pnanovdb_pipeline_voxelbvh_source_enum_t
{
    pnanovdb_pipeline_voxelbvh_source_gaussian_file = 0,
    pnanovdb_pipeline_voxelbvh_source_triangles = 1,
    pnanovdb_pipeline_voxelbvh_source_lines = 2,
    pnanovdb_pipeline_voxelbvh_source_gaussian_arrays = 3,
} pnanovdb_pipeline_voxelbvh_source_t;

PNANOVDB_API bool pnanovdb_pipeline_voxelbvh_build_params_set_source_type(pnanovdb_pipeline_params_t* params,
                                                                          pnanovdb_pipeline_voxelbvh_source_t source);
PNANOVDB_API bool pnanovdb_pipeline_voxelbvh_build_params_set_inflation_radius(pnanovdb_pipeline_params_t* params,
                                                                               float radius);
PNANOVDB_API bool pnanovdb_pipeline_voxelbvh_build_params_set_resolution(pnanovdb_pipeline_params_t* params,
                                                                         pnanovdb_uint32_t resolution);
PNANOVDB_API pnanovdb_uint32_t pnanovdb_pipeline_voxelbvh_sanitize_resolution(pnanovdb_uint32_t resolution);

namespace pnanovdb_editor
{

struct SceneObject;
class EditorScene;
class EditorSceneManager;
class Renderer;

// C++ PipelineContext (mirrors C struct but with typed pointers)
struct PipelineContext
{
    const pnanovdb_compute_t* compute = nullptr;
    pnanovdb_compute_device_t* device = nullptr;
    pnanovdb_compute_queue_t* queue = nullptr;
    pnanovdb_compute_queue_t* compute_queue = nullptr;
    pnanovdb_raster_t* raster = nullptr;
    pnanovdb_raster_context_t* raster_ctx = nullptr;
    pnanovdb_voxelbvh_t* voxelbvh = nullptr;
    pnanovdb_voxelbvh_context_t* voxelbvh_ctx = nullptr;
    Renderer* renderer = nullptr;
    EditorSceneManager* scene_manager = nullptr;
};

// C++ convenience wrappers (use C enum types directly)
pnanovdb_pipeline_render_method_t pipeline_get_render_method(pnanovdb_pipeline_type_t render_pipeline);
PNANOVDB_API pnanovdb_pipeline_result_t pipeline_execute_process(SceneObject* obj, const PipelineContext& ctx);
bool pipeline_needs_process(SceneObject* obj);
void pipeline_execute_pending(EditorSceneManager* manager, const PipelineContext& ctx);
const char* pipeline_get_shader(const SceneObject* obj);

// ============================================================================
// Async pipeline interface
// ============================================================================

/*!
    \brief Initialize this show() session's pipeline runtime.

    \param ctx          Pipeline context with compute/raster interfaces
    \param editor_scene Scene that will receive load-stage results
*/
void pipeline_init(const PipelineContext& ctx, EditorScene* editor_scene);

/*!
    \brief Pipeline-type-agnostic description of a load-stage task.
*/
struct PipelineLoadRequest
{
    pnanovdb_pipeline_type_t load_pipeline = pnanovdb_pipeline_type_noop;
    pnanovdb_pipeline_type_t process_pipeline = pnanovdb_pipeline_type_noop;
    pnanovdb_pipeline_type_t render_pipeline = pnanovdb_pipeline_type_noop;
    const char* source_filepath = nullptr;
    pnanovdb_editor_token_t* name_token = nullptr;
    const pnanovdb_pipeline_params_t* load_params = nullptr;
    const pnanovdb_pipeline_params_t* process_params = nullptr;
    bool replace_existing = false; // Preserve the current object until a generation-matched load publishes
    uint64_t reservation_id = 0;
};

/*!
    \brief Start a load-stage task.

    \param scene_manager Scene manager for shader params
    \param scene_token   Scene token captured at start time
    \param request       Pipeline-agnostic load description; see
                         PipelineLoadRequest
    \return true if the load task was started successfully
*/
bool pipeline_load(EditorSceneManager* scene_manager,
                   pnanovdb_editor_token_t* scene_token,
                   const PipelineLoadRequest& request);

/*!
    \brief Report whether a new load task can start.

    Scene restoration uses this to defer rather than discard load requests while
    the runtime's single async worker slot is occupied.
*/
bool pipeline_load_available();

/*!
    \brief Per-frame async update: drain every worker that completed this frame,
           then poll progress for any worker still in flight.

    \param progress_text  Filled with progress description while a task is
                          running
    \param progress_value Filled with 0.0..1.0 progress while a task is
                          running
    \return true iff at least one task is still in progress
*/
bool pipeline_update(std::string& progress_text, float& progress_value);

/*!
    \brief Create a new scene object for re-conversion with different parameters.

    Shares the source input resources and copies the complete process-stage
    configuration. Produced outputs are cleared and each non-noop process
    step is marked dirty so the variant rebuilds independently.

    \param scene_manager Scene manager to create the object in
    \param scene_token   Scene token for the new object
    \param source_name   Name token of the existing object to copy source from
    \param new_name      Name string for the new scene object
    \return true if the object was created successfully
*/
PNANOVDB_API bool pipeline_create_variant(EditorSceneManager* scene_manager,
                                          pnanovdb_editor_token_t* scene_token,
                                          pnanovdb_editor_token_t* source_name,
                                          const char* new_name);

bool pipeline_async_process_running_for(EditorSceneManager* scene_manager,
                                        pnanovdb_editor_token_t* scene,
                                        pnanovdb_editor_token_t* name);
bool pipeline_async_process_cancelling_for(EditorSceneManager* scene_manager,
                                           pnanovdb_editor_token_t* scene,
                                           pnanovdb_editor_token_t* name);

bool pipeline_async_process_cancel_available_for(EditorSceneManager* scene_manager,
                                                 pnanovdb_editor_token_t* scene,
                                                 pnanovdb_editor_token_t* name);

bool pipeline_cancel_async_process(EditorSceneManager* scene_manager,
                                   pnanovdb_editor_token_t* scene,
                                   pnanovdb_editor_token_t* name);

// Retarget an in-flight process worker after the same object is renamed.
bool pipeline_retarget_async_process_target(pnanovdb_editor_token_t* old_scene,
                                            pnanovdb_editor_token_t* old_name,
                                            pnanovdb_editor_token_t* new_scene,
                                            pnanovdb_editor_token_t* new_name,
                                            uint64_t lifetime_id);

void pipeline_process_pending_user_cancels(EditorSceneManager* scene_manager);

} // namespace pnanovdb_editor

// ============================================================================
// Opaque Type Casts (using reflection system)
// ============================================================================
PNANOVDB_CAST_PAIR(pnanovdb_renderer_t, pnanovdb_editor::Renderer)
PNANOVDB_CAST_PAIR(pnanovdb_scene_manager_t, pnanovdb_editor::EditorSceneManager)

// ============================================================================
// Pipeline Registration Macro
// ============================================================================

// Helper to define a pipeline in a single block
#define PNANOVDB_DEFINE_PIPELINE_SHADERS(name, ...)                                                                    \
    static const pnanovdb_pipeline_shader_entry_t name[] = { __VA_ARGS__ }

#define PNANOVDB_PIPELINE_SHADER(shader, group, overridable)                                                           \
    {                                                                                                                  \
        shader, group, overridable                                                                                     \
    }
