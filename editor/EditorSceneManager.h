// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/editor/EditorSceneManager.h

    \author Petra Hapalova

    \brief  Scene management system for tracking multiple objects with token-based lookups.
*/

#ifndef NANOVDB_EDITOR_SCENE_MANAGER_H_HAS_BEEN_INCLUDED
#define NANOVDB_EDITOR_SCENE_MANAGER_H_HAS_BEEN_INCLUDED

#include "SceneObject.h"
#include "ShaderParams.h"
#include "PipelineParams.h"
#include "CustomSceneParams.h"

#include <cassert>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace pnanovdb_editor
{
class SceneView;

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

class PNANOVDB_SCENE_MANAGER_EXPORT_CXX EditorSceneManager
{
public:
    EditorSceneManager() = default;
    ~EditorSceneManager() = default;

    // Shader parameters for all scenes
    ShaderParams shader_params;

    // Reflection-driven Properties UI + JSON hint cache for pipeline (process-stage) params
    PipelineParams pipeline_params;

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
        \brief Restore the pool of \p shader_name to its JSON-declared defaults
               and propagate the result to every object using that shader.

        \return true if the shader had a parameter layout to reset.

        \note Exported so it can be called from gtest binaries that link
              against the editor's hidden-visibility shared library.
    */
    bool reset_shader_params_to_defaults(const pnanovdb_compute_t* compute, const char* shader_name);

    //! Same as \ref reset_shader_params_to_defaults, applied to every shader
    //! referenced by the named group file.
    bool reset_group_params_to_defaults(const pnanovdb_compute_t* compute, const char* group_file_path);

    /*!
        \brief Reinitialize a single object's shader params buffer from the
               JSON defaults of its current shader_name.

        Used after a shader_name change (e.g. via map_params(shader_name_t)) to
        ensure the per-object parameter buffer matches the layout of the new
        shader.

        \param compute Compute interface
        \param scene   Scene token
        \param name    Object name token

        \return true if the object was found and its params were refreshed.

        \note Thread-safe.
    */
    bool refresh_params_for_object(const pnanovdb_compute_t* compute,
                                   pnanovdb_editor_token_t* scene,
                                   pnanovdb_editor_token_t* name);

    /*!
        \brief Same as the token-based overload, but operates on a resolved
               SceneObject reference.

        Intended for callers that already hold the scene manager's mutex
        (e.g. from inside with_object() / for_each_object()) and want to
        mutate shader_name() and refresh the buffer in a single critical
        section, leaving no window for a concurrent render-thread sync to
        copy stale bytes into the new shader's parameter pool.

        \pre Caller MUST hold the scene manager's mutex.
        \return true if the object's params buffer was refreshed.
    */
    bool refresh_params_for_object(const pnanovdb_compute_t* compute, SceneObject& obj);

    /*!
        \brief Create a unique key from scene and name tokens

        Combines two token IDs into a single 64-bit key for use as a map key.
        The scene ID is shifted to the upper 32 bits, name ID in lower 32 bits.

        \param scene Scene token
        \param name Object name token
        \return Combined 64-bit key, or 0 if either token is NULL
    */
    static inline uint64_t make_key(pnanovdb_editor_token_t* scene, pnanovdb_editor_token_t* name)
    {
        if (!scene || !name)
            return 0;
        return ((uint64_t)scene->id << 32) | (uint64_t)name->id;
    }

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
    bool add_nanovdb(pnanovdb_editor_token_t* scene,
                     pnanovdb_editor_token_t* name,
                     pnanovdb_compute_array_t* array,
                     pnanovdb_compute_array_t* params_array,
                     const pnanovdb_compute_t* compute,
                     pnanovdb_editor_token_t* shader_name = nullptr,
                     std::shared_ptr<pnanovdb_raster_gaussian_data_t>* old_gaussian_owner_out = nullptr);

    //! With explicit pipeline configuration (thread-safe, atomic)
    bool add_nanovdb(pnanovdb_editor_token_t* scene,
                     pnanovdb_editor_token_t* name,
                     pnanovdb_compute_array_t* array,
                     pnanovdb_compute_array_t* params_array,
                     const pnanovdb_compute_t* compute,
                     pnanovdb_editor_token_t* shader_name,
                     pnanovdb_pipeline_type_t process_pipeline,
                     pnanovdb_pipeline_type_t render_pipeline,
                     std::shared_ptr<pnanovdb_raster_gaussian_data_t>* old_gaussian_owner_out = nullptr);

    bool reserve_load_target(pnanovdb_editor_token_t* scene,
                             pnanovdb_editor_token_t* name,
                             uint64_t* lifetime_id,
                             bool replace_existing = false,
                             bool* replacing = nullptr);

    void cancel_load_target(pnanovdb_editor_token_t* scene, pnanovdb_editor_token_t* name, uint64_t lifetime_id);

    bool commit_reserved_nanovdb(pnanovdb_editor_token_t* scene,
                                 pnanovdb_editor_token_t* name,
                                 uint64_t lifetime_id,
                                 pnanovdb_compute_array_t* array,
                                 pnanovdb_compute_array_t* params_array,
                                 const pnanovdb_compute_t* compute,
                                 pnanovdb_editor_token_t* shader_name,
                                 pnanovdb_pipeline_type_t process_pipeline,
                                 pnanovdb_pipeline_type_t render_pipeline,
                                 std::shared_ptr<pnanovdb_raster_gaussian_data_t>* old_gaussian_owner_out = nullptr);

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
    bool add_gaussian_data(pnanovdb_editor_token_t* scene,
                           pnanovdb_editor_token_t* name,
                           pnanovdb_raster_gaussian_data_t* gaussian_data,
                           pnanovdb_compute_array_t* params_array,
                           const pnanovdb_reflect_data_type_t* shader_params_data_type,
                           const pnanovdb_compute_t* compute,
                           const pnanovdb_raster_t* raster,
                           pnanovdb_compute_queue_t* queue,
                           const char* shader_name = nullptr,
                           std::shared_ptr<pnanovdb_raster_gaussian_data_t>* old_owner_out = nullptr);

    bool commit_reserved_gaussian_data(pnanovdb_editor_token_t* scene,
                                       pnanovdb_editor_token_t* name,
                                       uint64_t lifetime_id,
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

    //! With explicit pipeline configuration (thread-safe, atomic)
    bool add_gaussian_data(pnanovdb_editor_token_t* scene,
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
    bool add_camera(pnanovdb_editor_token_t* scene, pnanovdb_editor_token_t* name, pnanovdb_camera_view_t* camera_view);

    /*!
        \brief Add a mesh (triangle or line) object backed by named arrays.

        Creates an Array-typed SceneObject whose named_arrays map holds the
        provided positions/indices/colors.

        \param scene Scene token
        \param name Object name token
        \param indices uint32 face/edge indices (3*N for triangles, 2*N for lines)
        \param positions float vertex positions (3*V floats)
        \param colors float vertex colors (3*V floats); pass nullptr to skip ownership
                      (the build pipeline will synthesize white colors when missing)
        \param compute Compute interface used to destroy the arrays on cleanup
        \param process_pipeline Initial process pipeline
        \param render_pipeline Initial render pipeline

        \note Thread-safe
    */
    bool add_mesh(pnanovdb_editor_token_t* scene,
                  pnanovdb_editor_token_t* name,
                  pnanovdb_compute_array_t* indices,
                  pnanovdb_compute_array_t* positions,
                  pnanovdb_compute_array_t* colors,
                  const pnanovdb_compute_t* compute,
                  pnanovdb_pipeline_type_t process_pipeline,
                  pnanovdb_pipeline_type_t render_pipeline,
                  std::shared_ptr<pnanovdb_raster_gaussian_data_t>* old_gaussian_owner_out = nullptr);

    bool commit_reserved_mesh(pnanovdb_editor_token_t* scene,
                              pnanovdb_editor_token_t* name,
                              uint64_t lifetime_id,
                              pnanovdb_compute_array_t* indices,
                              pnanovdb_compute_array_t* positions,
                              pnanovdb_compute_array_t* colors,
                              const pnanovdb_compute_t* compute,
                              pnanovdb_pipeline_type_t process_pipeline,
                              pnanovdb_pipeline_type_t render_pipeline,
                              std::shared_ptr<pnanovdb_raster_gaussian_data_t>* old_gaussian_owner_out = nullptr);

    /*!
        \brief Add an Array-typed scene object that will be filled by a
               file-backed process pipeline (e.g. voxelbvh build from a
               Gaussian .ply/.npy/.npz).

        \param scene Scene token
        \param name Object name token
        \param compute Compute interface
        \param process_pipeline Initial process pipeline (typically
                                pnanovdb_pipeline_type_voxelbvh_build)
        \param render_pipeline Initial render pipeline
        \param replace_existing Whether an existing render object may be replaced

        \note Thread-safe
    */
    bool add_file_object(pnanovdb_editor_token_t* scene,
                         pnanovdb_editor_token_t* name,
                         const pnanovdb_compute_t* compute,
                         pnanovdb_pipeline_type_t process_pipeline,
                         pnanovdb_pipeline_type_t render_pipeline,
                         bool replace_existing = false,
                         std::shared_ptr<pnanovdb_raster_gaussian_data_t>* old_gaussian_owner_out = nullptr);

    bool stage_file_object_replacement(pnanovdb_editor_token_t* scene,
                                       pnanovdb_editor_token_t* name,
                                       uint64_t lifetime_id,
                                       const pnanovdb_compute_t* compute,
                                       pnanovdb_pipeline_type_t process_pipeline,
                                       pnanovdb_pipeline_type_t render_pipeline);

    bool finish_file_object_replacement(uint64_t lifetime_id,
                                        bool success,
                                        std::shared_ptr<pnanovdb_raster_gaussian_data_t>* old_gaussian_owner_out = nullptr);

    bool has_file_object_replacement_in_progress() const;

    /*!
        \brief Register an existing camera with shared ownership

        Used for UI-created cameras that already have a shared_ptr owner.
        Unlike add_camera(), this does NOT create a copy - it shares ownership.

        \param scene Scene token
        \param name Object name token
        \param camera_view_owner Existing shared_ptr to the camera view

        \note Thread-safe
    */
    bool register_camera(pnanovdb_editor_token_t* scene,
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

    // Return the exact generation currently occupying a scene/name key.
    uint64_t object_lifetime(pnanovdb_editor_token_t* scene, pnanovdb_editor_token_t* name) const;

    // Remove only the exact object generation. Used by delayed cleanup so a
    // newer object reusing the same key cannot be erased by stale work.
    bool remove_if_lifetime(pnanovdb_editor_token_t* scene, pnanovdb_editor_token_t* name, uint64_t lifetime_id);

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
        \brief Rename a single object within a scene

        Re-keys the object identified by (\p scene, \p old_name) so it is now
       identified by
        (\p scene, \p new_name), preserving all of its data. Rejected if another
       object already uses \p new_name in \p scene.

        \param scene Scene the object belongs to
        \param old_name Current object name token
        \param new_name Desired object name token
        \return true on success, false on collision, invalid tokens, or if the
       object is missing

        \note Thread-safe
    */
    bool rename_object(pnanovdb_editor_token_t* scene,
                       pnanovdb_editor_token_t* old_name,
                       pnanovdb_editor_token_t* new_name);

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
        \brief Iterate over all objects with a callback while holding the lock

        Holds the mutex during iteration to prevent race conditions where objects
        might be removed during iteration.

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
        \brief Access an object by its stable lifetime, tolerating a concurrent
       rename.

        The supplied key is tried first. If that key moved, the globally unique
        lifetime identifies the same object under its current scene/name tokens.

        \note Thread-safe. Holds mutex during callback execution.
    */
    template <typename Func>
    void with_object_lifetime(pnanovdb_editor_token_t* scene,
                              pnanovdb_editor_token_t* name,
                              uint64_t lifetime_id,
                              Func callback)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        SceneObject* obj = nullptr;
        auto it = m_objects.find(make_key(scene, name));
        if (it != m_objects.end() && it->second.lifetime_id == lifetime_id)
        {
            obj = &it->second;
        }
        else if (lifetime_id != 0)
        {
            auto index_it = m_lifetime_to_key.find(lifetime_id);
            if (index_it != m_lifetime_to_key.end())
            {
                auto candidate = m_objects.find(index_it->second);
                if (candidate != m_objects.end() && candidate->second.lifetime_id == lifetime_id)
                {
                    obj = &candidate->second;
                }
            }
        }
        callback(obj);
    }

    /*!
        \brief Run a callback against an object, creating a data-less placeholder
               if none exists yet.

        \note Thread-safe. The callback always receives a non-null pointer.
    */
    template <typename Func>
    void with_object_or_create(pnanovdb_editor_token_t* scene, pnanovdb_editor_token_t* name, Func callback)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        uint64_t key = make_key(scene, name);
        auto it = m_objects.find(key);
        SceneObject* obj = nullptr;
        if (it != m_objects.end())
        {
            obj = &it->second;
        }
        else
        {
            obj = &reset_object_locked(key, scene, name, SceneObjectType::Uninitialized);
        }
        callback(obj);
    }

    /*!
        \brief Run a callback over the whole object map while holding the lock.

        \warning The callback MUST NOT call back into other EditorSceneManager
                 methods: m_mutex is already held and is not recursive, so doing
                 so would deadlock. Likewise, do not retain the map reference or
                 any element pointers past the callback.

        \note Thread-safe. Holds mutex for the entire callback.
    */
    template <typename Func>
    void with_objects_locked(Func callback)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        callback(m_objects);
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

    bool set_custom_scene_params(pnanovdb_editor_token_t* scene,
                                 pnanovdb_editor_token_t* json,
                                 std::string* error_message = nullptr);
    std::shared_ptr<CustomSceneParams> get_custom_scene_params(pnanovdb_editor_token_t* scene);

private:
    void begin_object_lifetime(SceneObject& obj, uint64_t key)
    {
        if (obj.lifetime_id != 0)
        {
            m_lifetime_to_key.erase(obj.lifetime_id);
        }
        obj.lifetime_id = m_next_object_lifetime_id++;
        m_lifetime_to_key[obj.lifetime_id] = key;
    }

    void forget_object_lifetime(const SceneObject& obj)
    {
        if (obj.lifetime_id != 0)
        {
            m_lifetime_to_key.erase(obj.lifetime_id);
        }
    }

    void reindex_object_lifetime(const SceneObject& obj, uint64_t new_key)
    {
        if (obj.lifetime_id != 0)
        {
            m_lifetime_to_key[obj.lifetime_id] = new_key;
        }
    }

    SceneObject& reset_object_locked(uint64_t key,
                                     pnanovdb_editor_token_t* scene,
                                     pnanovdb_editor_token_t* name,
                                     SceneObjectType type)
    {
        SceneObject& obj = m_objects[key];
        begin_object_lifetime(obj, key);
        obj.reset_source();
        obj.type = type;
        obj.scene_token = scene;
        obj.name_token = name;
        obj.ensure_shader_name_storage().object_key = key;
        return obj;
    }

    template <typename Impl>
    bool add_with_lock(pnanovdb_editor_token_t* scene,
                       pnanovdb_editor_token_t* name,
                       const uint64_t* reserved_lifetime,
                       Impl&& impl)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (reserved_lifetime && !load_reservation_matches(make_key(scene, name), *reserved_lifetime))
        {
            return false;
        }
        return impl();
    }

    // Private implementation helpers (called with mutex already held)
    bool add_nanovdb_impl(pnanovdb_editor_token_t* scene,
                          pnanovdb_editor_token_t* name,
                          pnanovdb_compute_array_t* array,
                          pnanovdb_compute_array_t* params_array,
                          const pnanovdb_compute_t* compute,
                          pnanovdb_editor_token_t* shader_name,
                          pnanovdb_pipeline_type_t process_pipeline,
                          pnanovdb_pipeline_type_t render_pipeline,
                          std::shared_ptr<pnanovdb_raster_gaussian_data_t>* old_gaussian_owner_out);

    bool add_gaussian_data_impl(pnanovdb_editor_token_t* scene,
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

    bool load_reservation_matches(uint64_t key, uint64_t lifetime_id) const;
    bool render_insertion_allowed(uint64_t key, pnanovdb_editor_token_t* name) const;
    bool add_mesh_impl(pnanovdb_editor_token_t* scene,
                       pnanovdb_editor_token_t* name,
                       pnanovdb_compute_array_t* indices,
                       pnanovdb_compute_array_t* positions,
                       pnanovdb_compute_array_t* colors,
                       const pnanovdb_compute_t* compute,
                       pnanovdb_pipeline_type_t process_pipeline,
                       pnanovdb_pipeline_type_t render_pipeline,
                       std::shared_ptr<pnanovdb_raster_gaussian_data_t>* old_gaussian_owner_out);

    mutable std::mutex m_mutex; ///< Protects all operations
    uint64_t m_next_object_lifetime_id = 1; ///< Assigned while holding m_mutex
    std::map<uint64_t, SceneObject> m_objects; ///< Map of objects by combined token key
    std::map<uint64_t, uint64_t> m_lifetime_to_key; ///< lifetime_id -> current object key (live objects only)
    std::map<uint64_t, SceneObject> m_file_replacement_backups; ///< Old objects retained by staged file builds
    std::map<uint64_t, std::shared_ptr<CustomSceneParams>> m_scene_custom_params; ///< Map of scene params by scene key
};

/*!
    \brief Capture a shader's JSON-default parameter bytes into a caller-owned
           buffer.

    Loads the compiled-shader JSON (which must exist on disk), populates the
    shader_params pool, then materialises the constant-buffer-sized blob and
    copies up to \p buf_size bytes into \p out_buf. Returns the number of
    bytes copied (zero if the JSON could not be loaded).
*/
PNANOVDB_SCENE_MANAGER_EXPORT_CXX size_t capture_shader_default_params(EditorSceneManager& scene_manager,
                                                                       const pnanovdb_compute_t* compute,
                                                                       const char* shader_name,
                                                                       size_t buf_size,
                                                                       void* out_buf);

// Create a SceneView scene and publish each of its cameras into the manager's
// unified object namespace before a render object is admitted.
PNANOVDB_SCENE_MANAGER_EXPORT_CXX bool ensure_scene_with_registered_cameras(EditorSceneManager& scene_manager,
                                                                            SceneView& scene_view,
                                                                            pnanovdb_editor_token_t* scene_token,
                                                                            bool create_default_camera = true);

// Ensure a restored scene has one usable viewport camera.
PNANOVDB_SCENE_MANAGER_EXPORT_CXX bool normalize_scene_viewport_camera(EditorSceneManager& scene_manager,
                                                                       SceneView& scene_view,
                                                                       pnanovdb_editor_token_t* scene_token);

} // namespace pnanovdb_editor

#endif // NANOVDB_EDITOR_SCENE_MANAGER_H_HAS_BEEN_INCLUDED
