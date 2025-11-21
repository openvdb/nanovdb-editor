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
#include "nanovdb_editor/putil/Raster.h"

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
    \brief A single object in the scene

    Represents one object that can be tracked by the scene manager.
    Each object has a type and associated data pointers.
*/
struct SceneObject
{
    SceneObjectType type; ///< Type of scene object
    pnanovdb_editor_token_t* scene_token; ///< Scene identifier token
    pnanovdb_editor_token_t* name_token; ///< Object name token

    // Object data (only one will be non-null based on type)
    pnanovdb_compute_array_t* nanovdb_array = nullptr; ///< NanoVDB volume data
    pnanovdb_raster_gaussian_data_t* gaussian_data = nullptr; ///< Gaussian splat data
    pnanovdb_camera_view_t* camera_view = nullptr; ///< Camera view data

    // Optional per-object shader params storage (e.g. NanoVDB)
    pnanovdb_compute_array_t* shader_params_array = nullptr; ///< Backing array for shader params when needed

    // Ownership handles to ensure proper destruction
    std::shared_ptr<pnanovdb_compute_array_t> nanovdb_array_owner; ///< Destroys compute array on removal
    std::shared_ptr<pnanovdb_raster_gaussian_data_t> gaussian_data_owner; ///< Destroys gaussian data on removal
    std::shared_ptr<pnanovdb_compute_array_t> shader_params_array_owner; ///< Destroys params array on removal
    std::shared_ptr<pnanovdb_camera_view_t> camera_view_owner; ///< Destroys camera view on removal

    // Parameters
    void* shader_params = nullptr; ///< Associated shader parameters
    const pnanovdb_reflect_data_type_t* shader_params_data_type = nullptr; ///< Parameter type info
    pnanovdb_editor_shader_name_t shader_name = {}; ///< Shader name for this object (e.g., for NanoVDB rendering)
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

private:
    mutable std::mutex m_mutex; ///< Protects all operations
    std::map<uint64_t, SceneObject> m_objects; ///< Map of objects by combined token key
};

} // namespace pnanovdb_editor

#endif // NANOVDB_EDITOR_SCENE_MANAGER_H_HAS_BEEN_INCLUDED
