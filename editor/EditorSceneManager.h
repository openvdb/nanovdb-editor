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
#include "nanovdb_editor/putil/Editor.h"
#include "PipelineTypes.h"
#include "nanovdb_editor/putil/Raster.h"

#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>
#include <memory>

namespace pnanovdb_editor
{

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
    \brief Resource data for a scene object
*/
struct SceneObjectResources
{
    // Unnamed primary data (one active based on object type)
    pnanovdb_compute_array_t* nanovdb_array = nullptr;
    pnanovdb_raster_gaussian_data_t* gaussian_data = nullptr;
    pnanovdb_camera_view_t* camera_view = nullptr;

    // Named arrays - multiple arrays identified by name
    std::map<std::string, pnanovdb_compute_array_t*> named_arrays;

    // Converted/processed data (output of process pipeline)
    pnanovdb_compute_array_t* converted_nanovdb = nullptr;

    // Source file path (for re-conversion from file with different parameters)
    std::string source_filepath;

    // Ownership handles for automatic cleanup
    std::shared_ptr<pnanovdb_compute_array_t> nanovdb_array_owner;
    std::shared_ptr<pnanovdb_raster_gaussian_data_t> gaussian_data_owner;
    std::shared_ptr<pnanovdb_camera_view_t> camera_view_owner;
    std::shared_ptr<pnanovdb_compute_array_t> converted_nanovdb_owner;
};

/*!
    \brief Compile-time known parameters for a scene object
*/
struct SceneObjectParams
{
    // GPU-backed shader params storage
    pnanovdb_compute_array_t* shader_params_array = nullptr;
    std::shared_ptr<pnanovdb_compute_array_t> shader_params_array_owner;

    // Typed params pointer and reflection info
    void* shader_params = nullptr;
    const pnanovdb_reflect_data_type_t* shader_params_data_type = nullptr;

    // Associated shader name
    pnanovdb_editor_shader_name_t shader_name = {};
};

/*!
    \brief Per-shader parameter override
*/
struct ShaderOverride
{
    std::string shader_name; // Override shader name (empty = use pipeline default)

    // Dynamic parameter overrides: param_name -> serialized value
    std::map<std::string, std::vector<uint8_t>> param_overrides;

    bool has_shader_override() const
    {
        return !shader_name.empty();
    }
    bool has_param_overrides() const
    {
        return !param_overrides.empty();
    }
    bool is_empty() const
    {
        return !has_shader_override() && !has_param_overrides();
    }
};

/*!
    \brief Configuration for a single pipeline stage

    Owns the heap-allocated params.data via malloc/free.
*/
struct PipelineStage
{
    pnanovdb_pipeline_type_t type = pnanovdb_pipeline_type_noop;
    pnanovdb_pipeline_params_t params = {}; // Heap-allocated stage params (data owned by this struct)

    // Per-shader overrides (indexed by shader position in pipeline descriptor)
    std::vector<ShaderOverride> shader_overrides;

    bool dirty = true; // Needs re-execution

    PipelineStage() = default;

    ~PipelineStage()
    {
        free(params.data);
    }

    PipelineStage(const PipelineStage& other)
        : type(other.type), params{}, shader_overrides(other.shader_overrides), dirty(other.dirty)
    {
        if (other.params.data && other.params.size > 0)
        {
            params.data = malloc(other.params.size);
            if (params.data)
            {
                memcpy(params.data, other.params.data, other.params.size);
                params.size = other.params.size;
                params.type = other.params.type;
            }
            else
            {
                // Keep object in a fully default/safe state if deep-copy allocation fails
                type = pnanovdb_pipeline_type_noop;
                params = {};
                shader_overrides.clear();
                dirty = true;
            }
        }
    }

    PipelineStage& operator=(const PipelineStage& other)
    {
        if (this != &other)
        {
            void* new_data = nullptr;
            size_t new_size = 0;
            const pnanovdb_reflect_data_type_t* new_type = nullptr;
            if (other.params.data && other.params.size > 0)
            {
                new_data = malloc(other.params.size);
                if (!new_data)
                {
                    // Allocation failed: keep current object unchanged
                    return *this;
                }
                memcpy(new_data, other.params.data, other.params.size);
                new_size = other.params.size;
                new_type = other.params.type;
            }

            free(params.data);
            params = {};
            params.data = new_data;
            params.size = new_size;
            params.type = new_type;
            type = other.type;
            shader_overrides = other.shader_overrides;
            dirty = other.dirty;
        }
        return *this;
    }

    PipelineStage(PipelineStage&& other) noexcept
        : type(other.type), params(other.params), shader_overrides(std::move(other.shader_overrides)), dirty(other.dirty)
    {
        other.params = {};
    }

    PipelineStage& operator=(PipelineStage&& other) noexcept
    {
        if (this != &other)
        {
            free(params.data);
            type = other.type;
            params = other.params;
            shader_overrides = std::move(other.shader_overrides);
            dirty = other.dirty;
            other.params = {};
        }
        return *this;
    }
};

/*!
    \brief Pipeline configuration for a scene object
*/
struct SceneObjectPipeline
{
    PipelineStage stages[pnanovdb_pipeline_stage_count];

    // Convenience accessors
    PipelineStage& load()
    {
        return stages[pnanovdb_pipeline_stage_load];
    }
    PipelineStage& process()
    {
        return stages[pnanovdb_pipeline_stage_process];
    }
    PipelineStage& render()
    {
        return stages[pnanovdb_pipeline_stage_render];
    }
    const PipelineStage& load() const
    {
        return stages[pnanovdb_pipeline_stage_load];
    }
    const PipelineStage& process() const
    {
        return stages[pnanovdb_pipeline_stage_process];
    }
    const PipelineStage& render() const
    {
        return stages[pnanovdb_pipeline_stage_render];
    }
};

/*!
    \brief A single object in the scene

    Represents one object that can be tracked by the scene manager.
*/
struct SceneObject
{
    SceneObjectType type; ///< Type of scene object
    pnanovdb_editor_token_t* scene_token; ///< Scene identifier token
    pnanovdb_editor_token_t* name_token; ///< Object name token

    SceneObjectResources resources; ///< Binary data (files)
    SceneObjectParams params; ///< Compile-time schemas (JSON)
    SceneObjectPipeline pipeline; ///< Dynamic overrides (JSON overrides)

    bool visible = true;

    // Resources
    pnanovdb_compute_array_t*& nanovdb_array()
    {
        return resources.nanovdb_array;
    }
    pnanovdb_raster_gaussian_data_t*& gaussian_data()
    {
        return resources.gaussian_data;
    }
    pnanovdb_camera_view_t*& camera_view()
    {
        return resources.camera_view;
    }
    std::map<std::string, pnanovdb_compute_array_t*>& named_arrays()
    {
        return resources.named_arrays;
    }

    // Params
    void*& shader_params()
    {
        return params.shader_params;
    }
    const pnanovdb_reflect_data_type_t*& shader_params_data_type()
    {
        return params.shader_params_data_type;
    }
    pnanovdb_editor_shader_name_t& shader_name()
    {
        return params.shader_name;
    }

    // Pipeline shortcuts
    pnanovdb_pipeline_type_t& load_pipeline()
    {
        return pipeline.load().type;
    }
    pnanovdb_pipeline_type_t& process_pipeline()
    {
        return pipeline.process().type;
    }
    pnanovdb_pipeline_type_t& render_pipeline()
    {
        return pipeline.render().type;
    }
    pnanovdb_pipeline_params_t& load_params()
    {
        return pipeline.load().params;
    }
    pnanovdb_pipeline_params_t& process_params()
    {
        return pipeline.process().params;
    }
    pnanovdb_pipeline_params_t& render_params()
    {
        return pipeline.render().params;
    }
    bool& process_dirty()
    {
        return pipeline.process().dirty;
    }
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

    //! With explicit pipeline configuration (thread-safe, atomic)
    void add_nanovdb(pnanovdb_editor_token_t* scene,
                     pnanovdb_editor_token_t* name,
                     pnanovdb_compute_array_t* array,
                     pnanovdb_compute_array_t* params_array,
                     const pnanovdb_compute_t* compute,
                     pnanovdb_editor_token_t* shader_name,
                     pnanovdb_pipeline_type_t process_pipeline,
                     pnanovdb_pipeline_type_t render_pipeline);

    /*!
        \brief Add or update Gaussian data

        If an object with the same (scene, name) already exists, it will be replaced.

        \param scene Scene token
        \param name Object name token
        \param gaussian_data Gaussian splat data
        \param params_array Shader parameters array
        \param shader_params_data_type Type information for the parameters
        \param compute Compute interface
        \param raster Raster interface for proper cleanup
        \param queue Device queue for proper cleanup
        \param shader_name Optional shader name identifier for this object
        \param old_owner_out Optional output parameter to receive the old owner (for deferred destruction)

        \note Thread-safe
        \note If old_owner_out is provided and an object is being replaced, the old gaussian_data_owner
              will be moved to old_owner_out for deferred destruction
    */
    void add_gaussian_data(pnanovdb_editor_token_t* scene,
                           pnanovdb_editor_token_t* name,
                           pnanovdb_raster_gaussian_data_t* gaussian_data,
                           pnanovdb_compute_array_t* params_array,
                           const pnanovdb_reflect_data_type_t* shader_params_data_type,
                           const pnanovdb_compute_t* compute,
                           const pnanovdb_raster_t* raster,
                           pnanovdb_compute_queue_t* queue,
                           const char* shader_name = nullptr,
                           std::shared_ptr<pnanovdb_raster_gaussian_data_t>* old_owner_out = nullptr);

    //! With explicit pipeline configuration (thread-safe, atomic)
    void add_gaussian_data(pnanovdb_editor_token_t* scene,
                           pnanovdb_editor_token_t* name,
                           pnanovdb_raster_gaussian_data_t* gaussian_data,
                           pnanovdb_compute_array_t* params_array,
                           const pnanovdb_reflect_data_type_t* shader_params_data_type,
                           const pnanovdb_compute_t* compute,
                           const pnanovdb_raster_t* raster,
                           pnanovdb_compute_queue_t* queue,
                           const char* shader_name,
                           pnanovdb_pipeline_type_t process_pipeline,
                           pnanovdb_pipeline_type_t render_pipeline,
                           std::shared_ptr<pnanovdb_raster_gaussian_data_t>* old_owner_out = nullptr);

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
        \brief Rename a scene token across all managed objects

        Re-keys all objects that belong to \p old_scene so they now belong to \p new_scene.
        The operation is rejected if any object name in \p old_scene would collide with an
        existing object name in \p new_scene.

        \param old_scene Source scene token to rename from
        \param new_scene Destination scene token to rename to
        \return true on success, false on collision, invalid tokens, or no-op failure

        \note Thread-safe
    */
    bool rename_scene(pnanovdb_editor_token_t* old_scene, pnanovdb_editor_token_t* new_scene);

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

private:
    // Private implementation helpers (called with mutex already held)
    void add_nanovdb_impl(pnanovdb_editor_token_t* scene,
                          pnanovdb_editor_token_t* name,
                          pnanovdb_compute_array_t* array,
                          pnanovdb_compute_array_t* params_array,
                          const pnanovdb_compute_t* compute,
                          pnanovdb_editor_token_t* shader_name,
                          pnanovdb_pipeline_type_t process_pipeline,
                          pnanovdb_pipeline_type_t render_pipeline);

    void add_gaussian_data_impl(pnanovdb_editor_token_t* scene,
                                pnanovdb_editor_token_t* name,
                                pnanovdb_raster_gaussian_data_t* gaussian_data,
                                pnanovdb_compute_array_t* params_array,
                                const pnanovdb_reflect_data_type_t* shader_params_data_type,
                                const pnanovdb_compute_t* compute,
                                const pnanovdb_raster_t* raster,
                                pnanovdb_compute_queue_t* queue,
                                const char* shader_name,
                                pnanovdb_pipeline_type_t process_pipeline,
                                pnanovdb_pipeline_type_t render_pipeline,
                                std::shared_ptr<pnanovdb_raster_gaussian_data_t>* old_owner_out);

    mutable std::mutex m_mutex; ///< Protects all operations
    std::map<uint64_t, SceneObject> m_objects; ///< Map of objects by combined token key
};

} // namespace pnanovdb_editor

PNANOVDB_CAST_PAIR(pnanovdb_scene_object_t, pnanovdb_editor::SceneObject)

#endif // NANOVDB_EDITOR_SCENE_MANAGER_H_HAS_BEEN_INCLUDED
