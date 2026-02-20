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
#include "PipelineTypes.h"
#include <memory>
#include <string>

// Pipeline parameter field descriptor for data-driven UI generation.
// Each field describes one editable parameter within a pipeline's params blob.
typedef struct pnanovdb_pipeline_param_field_t
{
    const char* name;
    const char* tooltip;
    pnanovdb_uint32_t type;        // PNANOVDB_REFLECT_TYPE_FLOAT, _INT32, etc.
    pnanovdb_uint64_t offset;      // offsetof() within params data
    float default_value;
    float min_value;
    float max_value;
    float step;
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

void* pnanovdb_scene_object_map_params(pnanovdb_scene_object_t* obj, const char* param_type_name);
void pnanovdb_scene_object_unmap_params(pnanovdb_scene_object_t* obj, const char* param_type_name);

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

// ============================================================================
// Shader Parameter Provider System
// ============================================================================

pnanovdb_uint32_t pnanovdb_shader_param_provider_register(const pnanovdb_shader_param_provider_t* provider,
                                                          pnanovdb_uint32_t priority);
void pnanovdb_shader_param_provider_unregister(pnanovdb_uint32_t handle);
pnanovdb_bool_t pnanovdb_shader_param_get(const char* shader_name,
                                          const char* param_name,
                                          pnanovdb_shader_param_value_t* out_value);

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

// Initialize all built-in pipelines (call once at startup)
void pipeline_register_builtins();

// ============================================================================
// Rasterization (file import) - manages async worker for file->Gaussian/NanoVDB
// ============================================================================

/*!
    \brief Initialize the rasterization subsystem

    Must be called once before any rasterization functions. Typically called
    during editor init when compute/raster interfaces are available.

    \param ctx Pipeline context with compute/raster interfaces
*/
void pipeline_init_rasterizer(const PipelineContext& ctx);

/*!
    \brief Start rasterization task

    Pending pipelines are set only after confirming rasterization is not already
    in progress, so a failed start does not overwrite the config for the running task.

    \param raster_filepath Path to file to rasterize
    \param voxels_per_unit Voxels per unit for rasterization
    \param rasterize_to_nanovdb Whether to rasterize to NanoVDB or Gaussian
    \param process_pipeline Process pipeline type (used for queue selection and completion)
    \param render_pipeline Render pipeline type (used on completion)
    \param editor_scene Scene for handling results
    \param scene_manager Scene manager for shader params
    \param scene_token Scene token captured at start time
    \param ctx Pipeline context with compute/raster interfaces
    \return true if rasterization was started successfully
*/
bool pipeline_start_rasterization(const char* raster_filepath,
                                  float voxels_per_unit,
                                  bool rasterize_to_nanovdb,
                                  pnanovdb_pipeline_type_t process_pipeline,
                                  pnanovdb_pipeline_type_t render_pipeline,
                                  EditorScene* editor_scene,
                                  EditorSceneManager* scene_manager,
                                  pnanovdb_editor_token_t* scene_token,
                                  const PipelineContext& ctx);

void pipeline_set_pending_pipelines(pnanovdb_pipeline_type_t process, pnanovdb_pipeline_type_t render, float voxels_per_unit);
pnanovdb_pipeline_type_t pipeline_get_pending_process_pipeline();
pnanovdb_pipeline_type_t pipeline_get_pending_render_pipeline();
float pipeline_get_pending_process_voxels_per_unit();

/*!
    \brief Check if rasterization is in progress

    \return true if a rasterization task is running
*/
bool pipeline_is_rasterizing();

/*!
    \brief Get rasterization progress

    \param progress_text Output text describing progress
    \param progress_value Output progress value (0.0 to 1.0)
    \return true if rasterization is in progress
*/
bool pipeline_get_rasterization_progress(std::string& progress_text, float& progress_value);

/*!
    \brief Check and handle rasterization completion

    \param editor_scene Scene for handling results
    \param old_gaussian_data_ptr Output parameter for old gaussian data (for deferred destruction)
    \return true if a task completed this frame (successful or not)
*/
bool pipeline_handle_rasterization_completion(EditorScene* editor_scene,
                                              std::shared_ptr<pnanovdb_raster_gaussian_data_t>& old_gaussian_data_ptr);

// Unified async progress: rasterization (file import) and pipeline conversion.
// Checks rasterization first, then pipeline conversion. Returns true if any is in progress.
// Pass nullptr for editor_scene to skip rasterization (e.g. headless).
bool pipeline_update_async_progress(EditorScene* editor_scene,
                                    std::string& progress_text,
                                    float& progress_value,
                                    std::shared_ptr<pnanovdb_raster_gaussian_data_t>& old_gaussian_data_ptr);

// Check if any async pipeline operation is running (conversion only; does not include rasterization)
bool pipeline_is_async_running();

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
    \return true if the object was created successfully
*/
bool pipeline_create_variant(EditorSceneManager* scene_manager,
                             pnanovdb_editor_token_t* scene_token,
                             pnanovdb_editor_token_t* source_name,
                             const char* new_name);

// ============================================================================
// Shader Parameter Provider (C++ wrapper)
// ============================================================================

/*!
 * \brief RAII wrapper for registering a shader parameter provider
 * 
 * Usage:
 *   ShaderParamProviderScope provider(100, ctx, get_fn);
 *   // Provider is registered while in scope
 *   // Automatically unregistered when destroyed
 */
class ShaderParamProviderScope
{
public:
    ShaderParamProviderScope(uint32_t priority,
                             pnanovdb_shader_param_provider_ctx_t* ctx,
                             pnanovdb_shader_param_provider_fn get_fn);
    ~ShaderParamProviderScope();
    
    // Non-copyable
    ShaderParamProviderScope(const ShaderParamProviderScope&) = delete;
    ShaderParamProviderScope& operator=(const ShaderParamProviderScope&) = delete;
    
    // Movable
    ShaderParamProviderScope(ShaderParamProviderScope&& other) noexcept;
    ShaderParamProviderScope& operator=(ShaderParamProviderScope&& other) noexcept;
    
    uint32_t handle() const { return m_handle; }
    bool valid() const { return m_handle != 0; }
    
private:
    uint32_t m_handle = 0;
};

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
#define PNANOVDB_DEFINE_PIPELINE_SHADERS(name, ...) \
    static const pnanovdb_pipeline_shader_entry_t name[] = { __VA_ARGS__ }

#define PNANOVDB_PIPELINE_SHADER(shader, group, overridable) \
    { shader, group, overridable }
