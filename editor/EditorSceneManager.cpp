// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/editor/EditorSceneManager.cpp

    \author Petra Hapalova

    \brief  Implementation of scene management system for tracking multiple objects.
*/

#include "EditorSceneManager.h"

#include <cstring>
#include <cstdio>

#ifdef _DEBUG
#    define SCENEMANAGER_LOG(...) printf(__VA_ARGS__)
#else
#    define SCENEMANAGER_LOG(...) (void)0
#endif

namespace pnanovdb_editor
{

uint64_t EditorSceneManager::make_key(pnanovdb_editor_token_t* scene, pnanovdb_editor_token_t* name)
{
    if (!scene || !name)
        return 0;
    // Combine the two IDs into a single key (simple hash)
    return ((uint64_t)scene->id << 32) | (uint64_t)name->id;
}

pnanovdb_compute_array_t* EditorSceneManager::create_params_array(const pnanovdb_compute_t* compute,
                                                                  const pnanovdb_reflect_data_type_t* data_type,
                                                                  size_t fallback_size)
{
    if (!compute)
    {
        return nullptr;
    }
    const void* default_value = data_type ? data_type->default_value : nullptr;
    size_t element_size = data_type ? data_type->element_size : fallback_size;
    return compute->create_array(element_size, 1u, default_value);
}

pnanovdb_compute_array_t* EditorSceneManager::create_initialized_shader_params(
    const pnanovdb_compute_t* compute,
    const char* shader_name,
    const char* shader_group,
    size_t fallback_size,
    const pnanovdb_reflect_data_type_t* fallback_data_type)
{
    if (!compute)
    {
        return nullptr;
    }

    // Ensure JSON with default is loaded from our own shader_params
    if (shader_group)
    {
        shader_params.loadGroup(shader_group, false);
    }
    else if (shader_name)
    {
        shader_params.load(shader_name, false);
    }

    pnanovdb_compute_array_t* params_array = nullptr;

    // Try to load from JSON defaults if shader_name is available
    if (shader_name)
    {
        params_array = shader_params.get_compute_array_for_shader(shader_name, compute);
    }

    // Fallback to empty/default params if JSON not loaded
    if (!params_array)
    {
        params_array = create_params_array(compute, fallback_data_type, fallback_size);
    }

    return params_array;
}

void EditorSceneManager::refresh_params_for_shader(const pnanovdb_compute_t* compute, const char* shader_name)
{
    if (!compute || !shader_name)
    {
        return;
    }

    pnanovdb_editor_token_t* shader_name_token = EditorToken::getInstance().getToken(shader_name);

    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& [key, obj] : m_objects)
    {
        if (obj.type == SceneObjectType::NanoVDB && tokens_equal(obj.shader_name.shader_name, shader_name_token))
        {
            // Destroy old params array owner first (if different)
            if (obj.shader_params_array_owner)
            {
                obj.shader_params_array_owner.reset();
            }

            // Recreate from JSON defaults for this shader
            pnanovdb_compute_array_t* params_array = shader_params.get_compute_array_for_shader(shader_name, compute);
            if (!params_array)
            {
                params_array = create_params_array(compute, nullptr, PNANOVDB_COMPUTE_CONSTANT_BUFFER_MAX_SIZE);
            }

            obj.shader_params_array = params_array;
            obj.shader_params = params_array ? params_array->data : nullptr;

            if (params_array)
            {
                obj.shader_params_array_owner = std::shared_ptr<pnanovdb_compute_array_t>(
                    params_array,
                    [destroy_fn = compute->destroy_array](pnanovdb_compute_array_t* ptr)
                    {
                        if (ptr)
                        {
                            destroy_fn(ptr);
                        }
                    });
            }
        }
    }
}

void EditorSceneManager::add_nanovdb(pnanovdb_editor_token_t* scene,
                                     pnanovdb_editor_token_t* name,
                                     pnanovdb_compute_array_t* array,
                                     pnanovdb_compute_array_t* params_array,
                                     const pnanovdb_compute_t* compute,
                                     pnanovdb_editor_token_t* shader_name)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    uint64_t key = make_key(scene, name);

    // Check if object already exists
    auto it = m_objects.find(key);
    bool object_exists = (it != m_objects.end());

    // Store old pointers BEFORE getting the reference (which might create the object)
    pnanovdb_compute_array_t* old_array = nullptr;
    pnanovdb_compute_array_t* old_params = nullptr;

    if (object_exists)
    {
        old_array = it->second.nanovdb_array_owner ? it->second.nanovdb_array_owner.get() : nullptr;
        old_params = it->second.shader_params_array_owner ? it->second.shader_params_array_owner.get() : nullptr;
    }

    auto& obj = m_objects[key];

    obj.type = SceneObjectType::NanoVDB;
    obj.scene_token = scene;
    obj.name_token = name;
    obj.nanovdb_array = array;
    obj.shader_params_array = params_array;
    obj.shader_params = params_array ? params_array->data : nullptr;
    obj.shader_params_data_type = nullptr;
    obj.shader_name.shader_name = shader_name;

    // Set up ownership for nanovdb_array
    if (compute && array)
    {
        // Only create new shared_ptr if this is a different array than what we already own
        if (array != old_array)
        {
            // CRITICAL: Check if ANY other object already owns this array pointer!
            // If another scene has the same array, we must share the ownership (not create a new shared_ptr)
            std::shared_ptr<pnanovdb_compute_array_t> existing_owner;
            for (const auto& pair : m_objects)
            {
                // Skip the current object we're modifying
                if (pair.first != key && pair.second.nanovdb_array_owner && pair.second.nanovdb_array_owner.get() == array)
                {
                    existing_owner = pair.second.nanovdb_array_owner;
                    SCENEMANAGER_LOG(
                        "[SceneManager] Found existing owner for array %p, sharing ownership\n", (void*)array);
                    break;
                }
            }

            if (existing_owner)
            {
                // Share the existing ownership
                obj.nanovdb_array_owner = existing_owner;
                SCENEMANAGER_LOG("[SceneManager] Shared owner for array %p, ref_count now %ld\n", (void*)array,
                                 existing_owner.use_count());
            }
            else
            {
                // Create new ownership
                obj.nanovdb_array_owner = std::shared_ptr<pnanovdb_compute_array_t>(
                    array,
                    [destroy_fn = compute->destroy_array](pnanovdb_compute_array_t* ptr)
                    {
                        if (ptr)
                        {
                            SCENEMANAGER_LOG("[SceneManager] Destroying nanovdb array %p (data=%p)\n", (void*)ptr,
                                             (void*)(ptr->data));
                            destroy_fn(ptr);
                        }
                    });
                SCENEMANAGER_LOG("[SceneManager] Created new owner for array %p, ref_count %ld\n", (void*)array,
                                 obj.nanovdb_array_owner.use_count());
            }
        }
    }
    else if (!array)
    {
        // Clear ownership if array is nullptr
        obj.nanovdb_array_owner.reset();
    }

    // Set up ownership for shader_params_array
    if (compute && params_array)
    {
        // Only create new shared_ptr if this is a different array than what we already own
        if (params_array != old_params)
        {
            // CRITICAL: Check if ANY other object already owns this array pointer!
            // If another scene has the same params array, we must share the ownership
            std::shared_ptr<pnanovdb_compute_array_t> existing_owner;
            for (const auto& pair : m_objects)
            {
                // Skip the current object we're modifying
                if (pair.first != key && pair.second.shader_params_array_owner &&
                    pair.second.shader_params_array_owner.get() == params_array)
                {
                    existing_owner = pair.second.shader_params_array_owner;
                    SCENEMANAGER_LOG("[SceneManager] Found existing owner for params array %p, sharing ownership\n",
                                     (void*)params_array);
                    break;
                }
            }

            if (existing_owner)
            {
                // Share the existing ownership
                obj.shader_params_array_owner = existing_owner;
                SCENEMANAGER_LOG("[SceneManager] Shared owner for params array %p, ref_count now %ld\n",
                                 (void*)params_array, existing_owner.use_count());
            }
            else
            {
                // Create new ownership
                obj.shader_params_array_owner = std::shared_ptr<pnanovdb_compute_array_t>(
                    params_array,
                    [destroy_fn = compute->destroy_array](pnanovdb_compute_array_t* ptr)
                    {
                        if (ptr)
                        {
                            SCENEMANAGER_LOG("[SceneManager] Destroying params array %p (data=%p)\n", (void*)ptr,
                                             (void*)(ptr->data));
                            destroy_fn(ptr);
                        }
                    });
                SCENEMANAGER_LOG("[SceneManager] Created new owner for params array %p, ref_count %ld\n",
                                 (void*)params_array, obj.shader_params_array_owner.use_count());
            }
        }
    }
    else if (!params_array)
    {
        // Clear ownership if params_array is nullptr
        obj.shader_params_array_owner.reset();
    }
}

void EditorSceneManager::add_gaussian_data(pnanovdb_editor_token_t* scene,
                                           pnanovdb_editor_token_t* name,
                                           pnanovdb_raster_gaussian_data_t* gaussian_data,
                                           pnanovdb_compute_array_t* params_array,
                                           const pnanovdb_reflect_data_type_t* shader_params_data_type,
                                           const pnanovdb_compute_t* compute,
                                           const pnanovdb_raster_t* raster,
                                           pnanovdb_compute_queue_t* queue,
                                           const char* shader_name,
                                           std::shared_ptr<pnanovdb_raster_gaussian_data_t>* old_owner_out)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    uint64_t key = make_key(scene, name);

    // Store old pointers BEFORE potentially moving them out
    pnanovdb_raster_gaussian_data_t* old_gaussian = nullptr;
    pnanovdb_compute_array_t* old_params = nullptr;

    auto it = m_objects.find(key);
    if (it != m_objects.end())
    {
        old_gaussian = it->second.gaussian_data_owner ? it->second.gaussian_data_owner.get() : nullptr;
        old_params = it->second.shader_params_array_owner ? it->second.shader_params_array_owner.get() : nullptr;

        // Extract the old owner for deferred destruction if requested
        if (it->second.type == SceneObjectType::GaussianData && old_owner_out)
        {
            *old_owner_out = std::move(it->second.gaussian_data_owner);
        }
    }

    auto& obj = m_objects[key];

    obj.type = SceneObjectType::GaussianData;
    obj.scene_token = scene;
    obj.name_token = name;
    obj.gaussian_data = gaussian_data;
    obj.shader_params_array = params_array;
    obj.shader_params = params_array ? params_array->data : nullptr;
    obj.shader_params_data_type = shader_params_data_type;
    obj.shader_name.shader_name = EditorToken::getInstance().getToken(shader_name);

    // Set up ownership for gaussian_data
    // Only create new shared_ptr if this is a different object than what we already own
    if (raster && compute && queue && gaussian_data && gaussian_data != old_gaussian)
    {
        // CRITICAL: Check if ANY other object already owns this gaussian data pointer!
        std::shared_ptr<pnanovdb_raster_gaussian_data_t> existing_owner;
        for (const auto& pair : m_objects)
        {
            if (pair.first != key && pair.second.gaussian_data_owner &&
                pair.second.gaussian_data_owner.get() == gaussian_data)
            {
                existing_owner = pair.second.gaussian_data_owner;
                SCENEMANAGER_LOG("[SceneManager] Found existing owner for gaussian data %p, sharing ownership\n",
                                 (void*)gaussian_data);
                break;
            }
        }

        if (existing_owner)
        {
            obj.gaussian_data_owner = existing_owner;
            SCENEMANAGER_LOG("[SceneManager] Shared owner for gaussian data %p, ref_count now %ld\n",
                             (void*)gaussian_data, existing_owner.use_count());
        }
        else
        {
            obj.gaussian_data_owner = std::shared_ptr<pnanovdb_raster_gaussian_data_t>(
                gaussian_data,
                [destroy_fn = raster->destroy_gaussian_data, compute_ptr = compute,
                 queue_ptr = queue](pnanovdb_raster_gaussian_data_t* ptr)
                {
                    if (ptr)
                    {
                        SCENEMANAGER_LOG("[SceneManager] Destroying gaussian data %p\n", (void*)ptr);
                        destroy_fn(compute_ptr, queue_ptr, ptr);
                    }
                });
            SCENEMANAGER_LOG("[SceneManager] Created new owner for gaussian data %p, ref_count %ld\n",
                             (void*)gaussian_data, obj.gaussian_data_owner.use_count());
        }
    }

    // Set up ownership for shader_params_array
    // Only create new shared_ptr if this is a different array than what we already own
    if (compute && params_array && params_array != old_params)
    {
        // CRITICAL: Check if ANY other object already owns this params array pointer!
        std::shared_ptr<pnanovdb_compute_array_t> existing_owner;
        for (const auto& pair : m_objects)
        {
            if (pair.first != key && pair.second.shader_params_array_owner &&
                pair.second.shader_params_array_owner.get() == params_array)
            {
                existing_owner = pair.second.shader_params_array_owner;
                SCENEMANAGER_LOG("[SceneManager] Found existing owner for params array %p (gaussian), sharing ownership\n",
                                 (void*)params_array);
                break;
            }
        }

        if (existing_owner)
        {
            obj.shader_params_array_owner = existing_owner;
            SCENEMANAGER_LOG("[SceneManager] Shared owner for params array %p (gaussian), ref_count now %ld\n",
                             (void*)params_array, existing_owner.use_count());
        }
        else
        {
            obj.shader_params_array_owner = std::shared_ptr<pnanovdb_compute_array_t>(
                params_array,
                [destroy_fn = compute->destroy_array](pnanovdb_compute_array_t* ptr)
                {
                    if (ptr)
                    {
                        SCENEMANAGER_LOG("[SceneManager] Destroying params array %p (gaussian)\n", (void*)ptr);
                        destroy_fn(ptr);
                    }
                });
            SCENEMANAGER_LOG("[SceneManager] Created new owner for params array %p (gaussian), ref_count %ld\n",
                             (void*)params_array, obj.shader_params_array_owner.use_count());
        }
    }
}

void EditorSceneManager::add_camera(pnanovdb_editor_token_t* scene,
                                    pnanovdb_editor_token_t* name,
                                    pnanovdb_camera_view_t* camera_view)
{
    if (!camera_view)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    uint64_t key = make_key(scene, name);

    auto& obj = m_objects[key];
    obj.type = SceneObjectType::Camera;
    obj.scene_token = scene;
    obj.name_token = name;

    // Create a deep copy of the camera view on the heap
    pnanovdb_camera_view_t* camera_copy = new pnanovdb_camera_view_t();

    // Copy all scalar fields
    camera_copy->name = camera_view->name;
    camera_copy->num_cameras = camera_view->num_cameras;
    camera_copy->axis_length = camera_view->axis_length;
    camera_copy->axis_thickness = camera_view->axis_thickness;
    camera_copy->frustum_line_width = camera_view->frustum_line_width;
    camera_copy->frustum_scale = camera_view->frustum_scale;
    camera_copy->frustum_color = camera_view->frustum_color;
    camera_copy->is_visible = camera_view->is_visible;

    // Deep copy the state and config arrays
    if (camera_view->num_cameras > 0)
    {
        if (camera_view->states)
        {
            camera_copy->states = new pnanovdb_camera_state_t[camera_view->num_cameras];
            std::memcpy(
                camera_copy->states, camera_view->states, sizeof(pnanovdb_camera_state_t) * camera_view->num_cameras);
        }
        else
        {
            camera_copy->states = nullptr;
        }

        if (camera_view->configs)
        {
            camera_copy->configs = new pnanovdb_camera_config_t[camera_view->num_cameras];
            std::memcpy(camera_copy->configs, camera_view->configs,
                        sizeof(pnanovdb_camera_config_t) * camera_view->num_cameras);
        }
        else
        {
            camera_copy->configs = nullptr;
        }
    }
    else
    {
        camera_copy->states = nullptr;
        camera_copy->configs = nullptr;
    }

    obj.camera_view = camera_copy;
    obj.camera_view_owner = std::shared_ptr<pnanovdb_camera_view_t>(camera_copy,
                                                                    [](pnanovdb_camera_view_t* ptr)
                                                                    {
                                                                        if (ptr)
                                                                        {
                                                                            delete[] ptr->states;
                                                                            delete[] ptr->configs;
                                                                            delete ptr;
                                                                        }
                                                                    });
}

bool EditorSceneManager::remove(pnanovdb_editor_token_t* scene, pnanovdb_editor_token_t* name)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    uint64_t key = make_key(scene, name);

    auto it = m_objects.find(key);
    if (it != m_objects.end())
    {
        SCENEMANAGER_LOG("[SceneManager] Removing object scene='%s', name='%s' (key=%llu) from manager\n",
                         scene ? scene->str : "null", name ? name->str : "null", (unsigned long long)key);

        // Log what shared_ptrs will be destroyed
        if (it->second.nanovdb_array_owner)
            SCENEMANAGER_LOG("[SceneManager]   - nanovdb_array %p, ref_count %ld\n",
                             (void*)it->second.nanovdb_array_owner.get(), it->second.nanovdb_array_owner.use_count());
        if (it->second.shader_params_array_owner)
            SCENEMANAGER_LOG("[SceneManager]   - params_array %p, ref_count %ld\n",
                             (void*)it->second.shader_params_array_owner.get(),
                             it->second.shader_params_array_owner.use_count());
        if (it->second.gaussian_data_owner)
            SCENEMANAGER_LOG("[SceneManager]   - gaussian_data %p, ref_count %ld\n",
                             (void*)it->second.gaussian_data_owner.get(), it->second.gaussian_data_owner.use_count());
    }

    bool result = m_objects.erase(key) > 0;
    SCENEMANAGER_LOG("[SceneManager] Remove result: %s\n", result ? "success" : "not found");
    return result;
}

SceneObject* EditorSceneManager::get(pnanovdb_editor_token_t* scene, pnanovdb_editor_token_t* name)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    uint64_t key = make_key(scene, name);
    auto it = m_objects.find(key);
    return (it != m_objects.end()) ? &it->second : nullptr;
}

std::vector<SceneObject*> EditorSceneManager::get_all_objects()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<SceneObject*> objects;
    objects.reserve(m_objects.size());
    for (auto& pair : m_objects)
    {
        objects.push_back(&pair.second);
    }
    return objects;
}

size_t EditorSceneManager::get_count() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_objects.size();
}

void EditorSceneManager::clear()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_objects.clear();
}

void EditorSceneManager::set_params_array(pnanovdb_editor_token_t* scene,
                                          pnanovdb_editor_token_t* name,
                                          pnanovdb_compute_array_t* params_array,
                                          const pnanovdb_compute_t* compute)
{
    if (!name)
    {
        return;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    uint64_t key = make_key(scene, name);
    auto it = m_objects.find(key);
    if (it == m_objects.end())
    {
        return;
    }

    SceneObject& obj = it->second;
    if (obj.type == SceneObjectType::Camera)
    {
        return;
    }

    // Reset old owner (safe even if null)
    obj.shader_params_array_owner.reset();

    obj.shader_params_array = params_array;
    obj.shader_params = params_array ? params_array->data : nullptr;

    if (compute && params_array)
    {
        obj.shader_params_array_owner = std::shared_ptr<pnanovdb_compute_array_t>(
            params_array,
            [destroy_fn = compute->destroy_array](pnanovdb_compute_array_t* ptr)
            {
                if (ptr)
                {
                    destroy_fn(ptr);
                }
            });
    }
}

} // namespace pnanovdb_editor
