// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/editor/EditorSceneManager.h

    \author Petra Hapalova

    \brief  Scene management system for tracking multiple objects with token-based lookups.
*/

#ifndef NANOVDB_EDITOR_SCENE_MANAGER_H_HAS_BEEN_INCLUDED
#define NANOVDB_EDITOR_SCENE_MANAGER_H_HAS_BEEN_INCLUDED

#include "EditorToken.h"
#include "ShaderParams.h"
#include "Renderer.h"
#include "EditorPipeline.h"
#include "nanovdb_editor/putil/Editor.h"
#include "nanovdb_editor/putil/Raster.h"

#include <map>
#include <mutex>
#include <vector>
#include <memory>

namespace pnanovdb_editor
{

/// Type alias for gaussian data shared pointer (used frequently in deferred destruction)
using GaussianDataPtr = std::shared_ptr<pnanovdb_raster_gaussian_data_t>;

/*!
    \brief Helper for GPU-safe deferred destruction of gaussian data

    GPU resources cannot be destroyed immediately while in use. This struct
    manages a 2-frame destruction delay: current frame's old data goes to slot,
    previous frame's data moves from slot to queue for final destruction.
*/
struct DeferredDestroyQueue
{
    GaussianDataPtr& slot; ///< Single-frame delay slot
    std::vector<GaussianDataPtr>& queue; ///< Multi-frame pending destruction queue

    /// Push old owner for deferred destruction
    void push(GaussianDataPtr&& owner)
    {
        if (slot)
        {
            queue.push_back(std::move(slot));
        }
        slot = std::move(owner);
    }
};

/*!
    \brief Scene object types that can be managed
*/
enum class SceneObjectType
{
    NanoVDB, ///< Volume data
    GaussianData, ///< Gaussian splatting data
    Array, ///< Generic array data
    Camera ///< Camera view
};

/*!
    \brief Named component within a scene object
*/
struct NamedComponent
{
    pnanovdb_editor_token_t* name_token = nullptr; ///< Component name
    pnanovdb_compute_array_t* array = nullptr; ///< Array data
    std::shared_ptr<pnanovdb_compute_array_t> array_owner; ///< Ownership

    // Metadata for the array
    std::string description; ///< Optional description
    const pnanovdb_reflect_data_type_t* data_type = nullptr; ///< Type information
};

/*!
    \brief A single object in the scene

    Represents one object that can be tracked by the scene manager.
    A scene object combines components to describe something that can be visualized.

    Components fall into two categories:
    - Resources: Data that would be serialized to dedicated file formats (NanoVDB, Gaussian data, arrays)
    - Pipelines: Configurations with parameters, shaders, and shader params (suitable for JSON)

    All components can be set()/get()/map() independently. For example:
    - set() Gaussian data
    - map() Raster3D pipeline to configure parameters
    - get() the output NanoVDB to save to file

    Named vs Unnamed Components:
    - NanoVDB and Gaussian Data: unnamed (typically single entry per scene object)
    - Pipelines: unnamed (one of each type initially)
    - Custom arrays: named (can have multiple, referenced by name in pipeline parameters)

    Scene objects are created when any component is added. The object may not yet
    contain enough components to produce useful output on screen.

    \note The 'type' field indicates the primary data source but does NOT mean
          only one type of data can exist. A scene object can have Gaussian data,
          named arrays, and output NanoVDB simultaneously.
*/
struct SceneObject
{
    SceneObjectType type; ///< Primary data source type (hint, not exclusive)
    pnanovdb_editor_token_t* scene_token; ///< Scene identifier token
    pnanovdb_editor_token_t* name_token; ///< Object name token

    // ========================================================================
    // INPUT DATA COMPONENTS (unnamed - typically single entry per scene object)
    // ========================================================================

    pnanovdb_compute_array_t* nanovdb_array = nullptr; ///< Input NanoVDB volume data
    pnanovdb_raster_gaussian_data_t* gaussian_data = nullptr; ///< Input Gaussian splat data
    pnanovdb_camera_view_t* camera_view = nullptr; ///< Camera view data

    // Ownership handles for input data
    std::shared_ptr<pnanovdb_compute_array_t> nanovdb_array_owner; ///< Destroys compute array on removal
    GaussianDataPtr gaussian_data_owner; ///< Destroys gaussian data on removal
    std::shared_ptr<pnanovdb_camera_view_t> camera_view_owner; ///< Destroys camera view on removal

    // ========================================================================
    // NAMED COMPONENTS (can have multiple per scene object)
    // ========================================================================

    // Named custom arrays - generic arrays that need names to be useful
    // Pipelines can reference these by name in their parameters
    std::map<uint64_t, NamedComponent> named_arrays; ///< Map from name token ID to component

    // ========================================================================
    // OUTPUT DATA COMPONENTS (unnamed - typically single output per pipeline)
    // ========================================================================

    // For conversion pipelines that produce NanoVDB output (e.g. Raster3D)
    // For Null pipeline, this remains nullptr and nanovdb_array is used directly
    pnanovdb_compute_array_t* output_nanovdb_array = nullptr;
    std::shared_ptr<pnanovdb_compute_array_t> output_nanovdb_array_owner;

    // ========================================================================
    // PIPELINE COMPONENTS (managed by EditorSceneManager::pipeline_manager)
    // ========================================================================
    // Note: Pipelines are managed centrally by EditorSceneManager::pipeline_manager
    // keyed by (scene, object, type). This keeps SceneObject lightweight and
    // ensures cleanup when objects are removed.

    // ========================================================================
    // SHADER PARAMETERS (supports reflection, not fixed structs)
    // ========================================================================

    // Parameters are dynamically typed via reflection, allowing them to be
    // extended/changed without recompilation. The pattern is:
    // 1. Load default values from JSON (via params_json_name in pipeline config)
    // 2. Override with scene object specific values
    //
    // For Gaussian splatting: defaults from JSON, then assigned from scene object

    // Backing storage for shader parameters (shared with pipeline)
    // Layout is defined by JSON (shader_params_json_name.json), not C structs
    pnanovdb_compute_array_t* shader_params_array = nullptr; ///< Backing array for shader params
    std::shared_ptr<pnanovdb_compute_array_t> shader_params_array_owner; ///< Shared ownership with pipeline

    // JSON name for loading parameter values (shader name, e.g. "raster/gaussian_rasterize_2d.slang")
    std::string shader_params_json_name; ///< Shader name for JSON params

    // Raw pointer to parameter data (points into shader_params_array)
    void* shader_params = nullptr; ///< Associated shader parameters

    // Reflection type for backward-compatible copy (tracks source version)
    // When copying params, use pnanovdb_reflect_copy_by_name with this as source type
    const pnanovdb_reflect_data_type_t* shader_params_reflect_type = nullptr;

    // Shader name for this object (e.g., for NanoVDB rendering)
    pnanovdb_editor_shader_name_t shader_name = {};
};

/*!
    \brief Manages multiple scene objects using token-based lookups

    This class provides a scene graph that allows multiple objects to coexist,
    each identified by a (scene, name) token pair. Objects can be added, removed,
    and their parameters accessed efficiently.

    Features:
    - Thread-safe operations with mutex protection
    - O(log n) lookups using combined token IDs as keys
    - Support for multiple object types (NanoVDB, Gaussian data, arrays)
    - Per-object parameter storage

    Usage:
    \code
    EditorSceneManager manager;

    // Add object
    manager.add_nanovdb(scene_token, name_token, array);

    // Get object
    SceneObject* obj = manager.get(scene_token, name_token);

    // Remove object
    bool removed = manager.remove(scene_token, name_token);
    \endcode
*/
class EditorSceneManager
{
public:
    EditorSceneManager() = default;
    ~EditorSceneManager() = default;

    // Shader parameters for all scenes
    ShaderParams shader_params;

    // Central pipeline manager for all pipelines (both scene-level and per-object)
    // Per-object pipelines are keyed by (scene, object, type)
    PipelineManager pipeline_manager;

    // Unified helper to create a compute-backed params array
    static pnanovdb_compute_array_t* create_params_array(const pnanovdb_compute_t* compute,
                                                         const pnanovdb_reflect_data_type_t* data_type,
                                                         size_t fallback_size);

    // Unified helper to create shader params initialized from JSON defaults
    pnanovdb_compute_array_t* create_initialized_shader_params(
        const pnanovdb_compute_t* compute,
        const char* shader_name,
        const char* shader_group,
        size_t fallback_size,
        const pnanovdb_reflect_data_type_t* fallback_data_type = nullptr);

    // Reinitialize params arrays for all NanoVDB objects using the given shader
    void refresh_params_for_shader(const pnanovdb_compute_t* compute, const char* shader_name);

    /*!
        \brief Create a unique key from scene and name tokens

        Combines two token IDs into a single 64-bit key for use as a map key.
        The scene ID is shifted to the upper 32 bits, name ID in lower 32 bits.

        \param scene Scene token
        \param name Object name token
        \return Combined 64-bit key, or 0 if either token is NULL
    */
    static uint64_t make_key(pnanovdb_editor_token_t* scene, pnanovdb_editor_token_t* name);

    /*!
        \brief Add or update a NanoVDB object

        If an object with the same (scene, name) already exists, it will be replaced.
        The manager takes ownership of the array and params_array through shared_ptr.

        \param scene Scene token
        \param name Object name token
        \param array NanoVDB array data
        \param params_array Shader parameters array (optional)
        \param compute Compute interface for proper cleanup
        \param shader_name Optional shader name identifier for this object

        \note Thread-safe
        \note The manager assumes ownership and will destroy both array and params_array
    */
    void add_nanovdb(pnanovdb_editor_token_t* scene,
                     pnanovdb_editor_token_t* name,
                     pnanovdb_compute_array_t* array,
                     pnanovdb_compute_array_t* params_array,
                     const pnanovdb_compute_t* compute,
                     pnanovdb_editor_token_t* shader_name = nullptr);

    /*!
        \brief Add or update Gaussian data

        If an object with the same (scene, name) already exists, it will be replaced.

        \param scene Scene token
        \param name Object name token
        \param gaussian_data Gaussian splat data
        \param params_array Shader parameters array
        \param compute Compute interface
        \param raster Raster interface for proper cleanup
        \param queue Device queue for proper cleanup
        \param shader_name Shader name identifier for this object
        \param deferred_destroy Deferred destruction queue for GPU-safe cleanup

        \note Thread-safe
        \note Deferred destruction is handled internally: when replacing an object, the old owner
              is chained through the slot/queue for GPU-safe delayed cleanup
    */
    void add_gaussian_data(pnanovdb_editor_token_t* scene,
                           pnanovdb_editor_token_t* name,
                           pnanovdb_raster_gaussian_data_t* gaussian_data,
                           const pnanovdb_compute_t* compute,
                           const pnanovdb_raster_t* raster,
                           pnanovdb_compute_queue_t* queue,
                           const char* shader_name,
                           DeferredDestroyQueue deferred_destroy);

    /*!
        \brief Add or update a camera view

        If an object with the same (scene, name) already exists, it will be replaced.
        The manager takes ownership of the camera_view through shared_ptr.

        \param scene Scene token
        \param name Object name token
        \param camera_view Camera view data (states and configs will be freed by manager)

        \note Thread-safe
        \note The manager assumes ownership and will destroy the camera view including
              its states and configs arrays on removal
    */
    void add_camera(pnanovdb_editor_token_t* scene, pnanovdb_editor_token_t* name, pnanovdb_camera_view_t* camera_view);

    /*!
        \brief Register an existing camera with shared ownership

        Used for UI-created cameras that already have a shared_ptr owner.
        Unlike add_camera(), this does NOT create a copy - it shares ownership.

        \param scene Scene token
        \param name Object name token
        \param camera_view_owner Existing shared_ptr to the camera view

        \note Thread-safe
    */
    void register_camera(pnanovdb_editor_token_t* scene,
                         pnanovdb_editor_token_t* name,
                         std::shared_ptr<pnanovdb_camera_view_t> camera_view_owner);

    /*!
        \brief Remove an object by tokens

        Removes the object and destroys all its associated data (arrays, gaussian data, etc.)
        through the manager's shared_ptr ownership handles.

        \param scene Scene token
        \param name Object name token
        \return true if object was found and removed, false if not found

        \note Thread-safe
        \note All associated memory is automatically freed via custom deleters
    */
    bool remove(pnanovdb_editor_token_t* scene, pnanovdb_editor_token_t* name);

    /*!
        \brief Get object by tokens

        \param scene Scene token
        \param name Object name token
        \return Pointer to object or nullptr if not found

        \note Thread-safe. The returned pointer remains valid only while the
              mutex is held. Don't store or modify this pointer long-term,
              especially the ownership members (shared_ptr handles).
              Use with_object() or for_each_object() for safer access.
    */
    SceneObject* get(pnanovdb_editor_token_t* scene, pnanovdb_editor_token_t* name);

    /*!
        \brief Get all objects in the scene

        Returns a snapshot of all object pointers. Note that these pointers
        become invalid if objects are added/removed.

        \return Vector of pointers to all scene objects

        \note Thread-safe. Creates a snapshot with mutex held.
        \warning UNSAFE: Pointers may become invalid! Use for_each_object() instead.
    */
    std::vector<SceneObject*> get_all_objects();

    /*!
        \brief Iterate over all objects with a callback while holding the lock

        Safer alternative to get_all_objects() that holds the mutex during iteration.
        This prevents race conditions where objects might be removed during iteration.

        \param callback Function called for each object. Return false to stop iteration.

        \note Thread-safe. Holds mutex for entire iteration.
    */
    template <typename Func>
    void for_each_object(Func callback)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto& pair : m_objects)
        {
            if (!callback(&pair.second))
            {
                break;
            }
        }
    }

    /*!
        \brief Access a specific object with a callback while holding the lock

        Safer alternative to get() that ensures the pointer is only accessed while mutex is held.
        This prevents race conditions where the object might be removed after get() returns.

        \param scene Scene token
        \param name Object name token
        \param callback Function called with the object pointer (or nullptr if not found)

        \note Thread-safe. Holds mutex during callback execution.
    */
    template <typename Func>
    void with_object(pnanovdb_editor_token_t* scene, pnanovdb_editor_token_t* name, Func callback)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        uint64_t key = make_key(scene, name);
        auto it = m_objects.find(key);
        SceneObject* obj = (it != m_objects.end()) ? &it->second : nullptr;
        callback(obj);
    }

    /*!
        \brief Get count of objects

        \return Number of objects currently in the scene manager

        \note Thread-safe
    */
    size_t get_count() const;

    /*!
        \brief Clear all objects

        Removes all objects from the scene manager and destroys the actual
        object data through the manager's ownership handles (shared_ptr).
        All associated memory (arrays, gaussian data, camera views) is
        automatically freed via the custom deleters.

        \note Thread-safe
    */
    void clear();

    // Set (or replace) per-object params array for an object; updates ownership and raw pointer
    void set_params_array(pnanovdb_editor_token_t* scene,
                          pnanovdb_editor_token_t* name,
                          pnanovdb_compute_array_t* params_array,
                          const pnanovdb_compute_t* compute);

    // ========================================================================
    // Named Component Management
    // ========================================================================

    /*!
        \brief Add a named array component to a scene object

        Named arrays are generic arrays that require a name for identification.
        Multiple named arrays can exist per scene object.
        Pipelines can reference these arrays by name.

        \param scene Scene token
        \param object_name Object name token
        \param array_name Name for this array component
        \param array Array data
        \param compute Compute interface for ownership
        \param description Optional description
        \param data_type Optional type information

        \note Thread-safe
    */
    void add_named_array(pnanovdb_editor_token_t* scene,
                         pnanovdb_editor_token_t* object_name,
                         pnanovdb_editor_token_t* array_name,
                         pnanovdb_compute_array_t* array,
                         const pnanovdb_compute_t* compute,
                         const char* description = nullptr,
                         const pnanovdb_reflect_data_type_t* data_type = nullptr);

    /*!
        \brief Get a named array component

        \param scene Scene token
        \param object_name Object name token
        \param array_name Name of the array
        \return Pointer to array or nullptr if not found

        \note Thread-safe
    */
    pnanovdb_compute_array_t* get_named_array(pnanovdb_editor_token_t* scene,
                                              pnanovdb_editor_token_t* object_name,
                                              pnanovdb_editor_token_t* array_name);

    /*!
        \brief Remove a named array component

        \param scene Scene token
        \param object_name Object name token
        \param array_name Name of the array
        \return true if found and removed

        \note Thread-safe
    */
    bool remove_named_array(pnanovdb_editor_token_t* scene,
                            pnanovdb_editor_token_t* object_name,
                            pnanovdb_editor_token_t* array_name);

    // ========================================================================
    // Pipeline Management
    // ========================================================================

    /*!
        \brief Set or configure a conversion pipeline for a scene object

        Pipelines can be configured before data is added. Pipelines without
        data are no-op, making this ordering safe for multithreaded use.

        \param scene Scene token
        \param object_name Object name token
        \param type Pipeline type
        \param config Pipeline configuration
        \param pipeline_name Optional name for multiple pipelines of same type

        \note Thread-safe
    */
    void set_conversion_pipeline(pnanovdb_editor_token_t* scene,
                                 pnanovdb_editor_token_t* object_name,
                                 PipelineType type,
                                 const PipelineConfig& config,
                                 pnanovdb_editor_token_t* pipeline_name = nullptr);

    /*!
        \brief Set or configure a rendering pipeline for a scene object

        \param scene Scene token
        \param object_name Object name token
        \param config Pipeline configuration
        \param pipeline_name Optional name

        \note Thread-safe
    */
    void set_render_pipeline(pnanovdb_editor_token_t* scene,
                             pnanovdb_editor_token_t* object_name,
                             const PipelineConfig& config,
                             pnanovdb_editor_token_t* pipeline_name = nullptr);

    /*!
        \brief Get a pipeline configuration for modification

        Allows user to map() the pipeline to modify parameters.

        \param scene Scene token
        \param object_name Object name token
        \param type Pipeline type
        \param pipeline_name Optional name
        \return Pointer to pipeline config or nullptr

        \note Thread-safe via with_object()
    */
    PipelineConfig* get_pipeline(pnanovdb_editor_token_t* scene,
                                 pnanovdb_editor_token_t* object_name,
                                 PipelineType type,
                                 pnanovdb_editor_token_t* pipeline_name = nullptr);

    /*!
        \brief Execute a specific pipeline

        \param scene Scene token
        \param object_name Object name token
        \param type Pipeline type
        \param context Execution context
        \param pipeline_name Optional name
        \return Pipeline status

        \note Thread-safe
    */
    PipelineStatus execute_pipeline(pnanovdb_editor_token_t* scene,
                                    pnanovdb_editor_token_t* object_name,
                                    PipelineType type,
                                    PipelineExecutionContext* context,
                                    pnanovdb_editor_token_t* pipeline_name = nullptr);

    /*!
        \brief Mark pipeline as dirty (needs re-run)

        Called when input data or parameters change.

        \param scene Scene token
        \param object_name Object name token
        \param type Pipeline type
        \param pipeline_name Optional name

        \note Thread-safe
    */
    void mark_pipeline_dirty(pnanovdb_editor_token_t* scene,
                             pnanovdb_editor_token_t* object_name,
                             PipelineType type,
                             pnanovdb_editor_token_t* pipeline_name = nullptr);

    /*!
        \brief Execute all dirty pipelines across all scene objects

        Implements lazy execution for conversion pipelines.

        \param context Execution context

        \note Thread-safe
    */
    void execute_all_dirty_pipelines(PipelineExecutionContext* context);

    /*!
        \brief Get output NanoVDB from a scene object

        After a conversion pipeline runs (e.g. Raster3D), this retrieves
        the output NanoVDB. User can then save it to file.

        For Null pipeline (pass-through), returns the input nanovdb_array directly,
        implementing the design requirement that "input NanoVDB and output NanoVDB
        would be the same" with no copy cost via ref counting.

        \param scene Scene token
        \param object_name Object name token
        \return Output NanoVDB array or nullptr

        \note Thread-safe
    */
    pnanovdb_compute_array_t* get_output(pnanovdb_editor_token_t* scene, pnanovdb_editor_token_t* object_name);

private:
    mutable std::mutex m_mutex; ///< Protects all operations
    std::map<uint64_t, SceneObject> m_objects; ///< Map of objects by combined token key
};

} // namespace pnanovdb_editor

#endif // NANOVDB_EDITOR_SCENE_MANAGER_H_HAS_BEEN_INCLUDED
