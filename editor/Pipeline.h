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
#include <memory>
#include <string>

// Pipeline parameter field descriptor for data-driven UI generation.
// Each field describes one editable parameter within a pipeline's params blob.
typedef struct pnanovdb_pipeline_param_field_t
{
    const char* name;
    const char* tooltip;
    pnanovdb_uint32_t type; // PNANOVDB_REFLECT_TYPE_FLOAT, _INT32, etc.
    pnanovdb_uint64_t offset; // offsetof() within params data
    float default_value;
    float min_value;
    float max_value;
    float step;
    const char* const* enum_labels;
    pnanovdb_uint32_t enum_count;
} pnanovdb_pipeline_param_field_t;

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

// Register a pipeline descriptor (call at startup)
void pnanovdb_pipeline_register(const pnanovdb_pipeline_descriptor_t* descriptor);

// Get registered pipeline count
pnanovdb_uint32_t pnanovdb_pipeline_get_count(void);

// Pipeline type utilities (use registered descriptors)
const char* pnanovdb_pipeline_get_shader_name(pnanovdb_pipeline_type_t type);
const char* pnanovdb_pipeline_get_shader_group(pnanovdb_pipeline_type_t type);
const pnanovdb_pipeline_descriptor_t* pnanovdb_pipeline_get_descriptor(pnanovdb_pipeline_type_t type);
void pnanovdb_pipeline_get_default_params(pnanovdb_pipeline_type_t type, pnanovdb_pipeline_params_t* params);

typedef enum pnanovdb_pipeline_voxelbvh_source_enum_t
{
    pnanovdb_pipeline_voxelbvh_source_gaussian_file = 0,
    pnanovdb_pipeline_voxelbvh_source_triangles = 1,
    pnanovdb_pipeline_voxelbvh_source_lines = 2,
    pnanovdb_pipeline_voxelbvh_source_gaussian_arrays = 3,
} pnanovdb_pipeline_voxelbvh_source_t;

bool pnanovdb_pipeline_voxelbvh_build_params_set_source_type(pnanovdb_pipeline_params_t* params,
                                                             pnanovdb_pipeline_voxelbvh_source_t source);
bool pnanovdb_pipeline_voxelbvh_build_params_set_inflation_radius(pnanovdb_pipeline_params_t* params, float radius);
bool pnanovdb_pipeline_voxelbvh_build_params_set_resolution(pnanovdb_pipeline_params_t* params,
                                                            pnanovdb_uint32_t resolution);

// Execute pipeline using registered function pointers
pnanovdb_pipeline_result_t pnanovdb_pipeline_execute(pnanovdb_pipeline_type_t type,
                                                     pnanovdb_scene_object_t* obj,
                                                     pnanovdb_pipeline_context_t* ctx);
pnanovdb_pipeline_render_method_t pnanovdb_pipeline_get_render_method(pnanovdb_pipeline_type_t type);

// ============================================================================
// Scene Object Pipeline Operations
// ============================================================================

void pnanovdb_scene_object_set_pipeline(pnanovdb_scene_object_t* obj,
                                        pnanovdb_pipeline_stage_t stage,
                                        pnanovdb_pipeline_type_t type);
pnanovdb_pipeline_type_t pnanovdb_scene_object_get_pipeline(pnanovdb_scene_object_t* obj,
                                                            pnanovdb_pipeline_stage_t stage);
void pnanovdb_scene_object_mark_dirty(pnanovdb_scene_object_t* obj);

void* pnanovdb_scene_object_map_params(pnanovdb_scene_object_t* obj, const pnanovdb_reflect_data_type_t* param_data_type);
void pnanovdb_scene_object_unmap_params(pnanovdb_scene_object_t* obj,
                                        const pnanovdb_reflect_data_type_t* param_data_type);

pnanovdb_uint32_t pnanovdb_scene_object_get_pipeline_stage_shader_count(pnanovdb_scene_object_t* obj,
                                                                        pnanovdb_pipeline_stage_t stage);
void pnanovdb_scene_object_set_pipeline_stage_shader(pnanovdb_scene_object_t* obj,
                                                     pnanovdb_pipeline_stage_t stage,
                                                     pnanovdb_uint32_t shader_idx,
                                                     const char* override_shader);
const char* pnanovdb_scene_object_get_pipeline_stage_shader(pnanovdb_scene_object_t* obj,
                                                            pnanovdb_pipeline_stage_t stage,
                                                            pnanovdb_uint32_t shader_idx);

pnanovdb_bool_t pnanovdb_scene_object_map_shader_params(pnanovdb_scene_object_t* obj,
                                                        pnanovdb_uint32_t shader_idx,
                                                        pnanovdb_shader_params_desc_t* out_desc);
void pnanovdb_scene_object_unmap_shader_params(pnanovdb_scene_object_t* obj, pnanovdb_uint32_t shader_idx);
const pnanovdb_reflect_data_type_t* pnanovdb_scene_object_get_shader_params_type(pnanovdb_scene_object_t* obj,
                                                                                 pnanovdb_uint32_t shader_idx);

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
pnanovdb_pipeline_result_t pipeline_execute_process(SceneObject* obj, const PipelineContext& ctx);
bool pipeline_needs_process(SceneObject* obj);
void pipeline_mark_dirty(SceneObject* obj);
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
    const pnanovdb_pipeline_params_t* load_params = nullptr;
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
    \brief Per-frame async update: poll progress and handle completion for
           whichever pipeline worker is in flight.

    \param progress_text  Filled with progress description while a task is
                          running
    \param progress_value Filled with 0.0..1.0 progress while a task is
                          running
    \return true iff a task is still in progress
*/
bool pipeline_update(std::string& progress_text, float& progress_value);

/*!
    \brief Create a new scene object for re-conversion with different parameters.

    Copies the source filepath, pipeline type, and a deep copy of the process
    params from the source object. The new object is marked dirty so the
    pipeline picks it up for conversion. The caller should write desired
    parameter values into the source object BEFORE calling this function
    (the generic Properties UI does this on every edit).

    \param scene_manager Scene manager to create the object in
    \param scene_token   Scene token for the new object
    \param source_name   Name token of the existing object to copy source from
    \param new_name      Name string for the new scene object
    \param compute       Compute interface used to register the new (empty)
                         nanovdb placeholder with scene_manager
    \return true if the object was created successfully
*/
bool pipeline_create_variant(EditorSceneManager* scene_manager,
                             pnanovdb_editor_token_t* scene_token,
                             pnanovdb_editor_token_t* source_name,
                             const char* new_name,
                             const pnanovdb_compute_t* compute);

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
