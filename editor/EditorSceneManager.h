// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/editor/EditorSceneManager.h

    \author Andrew Reidmeyer

    \brief  Scene management system for tracking multiple objects with token-based lookups.
*/

#ifndef NANOVDB_EDITOR_SCENE_MANAGER_H_HAS_BEEN_INCLUDED
#define NANOVDB_EDITOR_SCENE_MANAGER_H_HAS_BEEN_INCLUDED

#include "nanovdb_editor/putil/Editor.h"
#include "nanovdb_editor/putil/Compute.h"
#include "nanovdb_editor/putil/Raster.h"
#include "nanovdb_editor/putil/Reflect.h"
#include "nanovdb_editor/putil/Camera.h"

#include <map>
#include <mutex>
#include <vector>

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
    pnanovdb_raster_context_t* raster_ctx = nullptr; ///< Raster context for gaussian data
    pnanovdb_compute_array_t* data_array = nullptr; ///< Generic array data
    pnanovdb_camera_view_t* camera_view = nullptr; ///< Camera view data

    // Parameters
    void* shader_params = nullptr; ///< Associated shader parameters
    const pnanovdb_reflect_data_type_t* shader_params_data_type = nullptr; ///< Parameter type info
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

        \param scene Scene token
        \param name Object name token
        \param array NanoVDB array data

        \note Thread-safe
    */
    void add_nanovdb(pnanovdb_editor_token_t* scene, pnanovdb_editor_token_t* name, pnanovdb_compute_array_t* array);

    /*!
        \brief Add or update Gaussian data

        If an object with the same (scene, name) already exists, it will be replaced.

        \param scene Scene token
        \param name Object name token
        \param gaussian_data Gaussian splat data
        \param raster_ctx Raster context for the gaussian data
        \param shader_params Associated shader parameters
        \param shader_params_data_type Type information for the parameters

        \note Thread-safe
    */
    void add_gaussian_data(pnanovdb_editor_token_t* scene,
                           pnanovdb_editor_token_t* name,
                           pnanovdb_raster_gaussian_data_t* gaussian_data,
                           pnanovdb_raster_context_t* raster_ctx,
                           void* shader_params,
                           const pnanovdb_reflect_data_type_t* shader_params_data_type);

    /*!
        \brief Add or update a camera view

        If an object with the same (scene, name) already exists, it will be replaced.

        \param scene Scene token
        \param name Object name token
        \param camera_view Camera view data

        \note Thread-safe
    */
    void add_camera(pnanovdb_editor_token_t* scene, pnanovdb_editor_token_t* name, pnanovdb_camera_view_t* camera_view);

    /*!
        \brief Remove an object by tokens

        \param scene Scene token
        \param name Object name token
        \return true if object was found and removed, false if not found

        \note Thread-safe
    */
    bool remove(pnanovdb_editor_token_t* scene, pnanovdb_editor_token_t* name);

    /*!
        \brief Get object by tokens

        \param scene Scene token
        \param name Object name token
        \return Pointer to object or nullptr if not found

        \note Thread-safe. The returned pointer remains valid only while the
              mutex could be acquired. Don't store this pointer long-term.
    */
    SceneObject* get(pnanovdb_editor_token_t* scene, pnanovdb_editor_token_t* name);

    /*!
        \brief Get all objects in the scene

        Returns a snapshot of all object pointers. Note that these pointers
        become invalid if objects are added/removed.

        \return Vector of pointers to all scene objects

        \note Thread-safe. Creates a snapshot with mutex held.
    */
    std::vector<SceneObject*> get_all_objects();

    /*!
        \brief Get count of objects

        \return Number of objects currently in the scene manager

        \note Thread-safe
    */
    size_t get_count() const;

    /*!
        \brief Clear all objects

        Removes all objects from the scene manager. Does not destroy
        the actual object data (caller responsible for cleanup).

        \note Thread-safe
    */
    void clear();

private:
    mutable std::mutex m_mutex; ///< Protects all operations
    std::map<uint64_t, SceneObject> m_objects; ///< Map of objects by combined token key
};

} // namespace pnanovdb_editor

#endif // NANOVDB_EDITOR_SCENE_MANAGER_H_HAS_BEEN_INCLUDED
