// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/editor/EditorSceneManager.cpp

    \author Petra Hapalova

    \brief  Implementation of scene management system for tracking multiple objects.
*/

#include "EditorSceneManager.h"
#include <cstring>

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

void EditorSceneManager::add_nanovdb(pnanovdb_editor_token_t* scene,
                                     pnanovdb_editor_token_t* name,
                                     pnanovdb_compute_array_t* array,
                                     pnanovdb_compute_array_t* params_array,
                                     const pnanovdb_compute_t* compute,
                                     const char* shader_name)
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
    obj.shader_name = shader_name ? shader_name : "";

    // Set up ownership for nanovdb_array
    if (compute && array)
    {
        // Only create new shared_ptr if this is a different array than what we already own
        if (array != old_array)
        {
            obj.nanovdb_array_owner = std::shared_ptr<pnanovdb_compute_array_t>(
                array,
                [destroy_fn = compute->destroy_array](pnanovdb_compute_array_t* ptr)
                {
                    if (ptr)
                        destroy_fn(ptr);
                });
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
            obj.shader_params_array_owner = std::shared_ptr<pnanovdb_compute_array_t>(
                params_array,
                [destroy_fn = compute->destroy_array](pnanovdb_compute_array_t* ptr)
                {
                    if (ptr)
                        destroy_fn(ptr);
                });
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
    obj.shader_name = shader_name ? shader_name : "";

    // Set up ownership for gaussian_data
    // Only create new shared_ptr if this is a different object than what we already own
    if (raster && compute && queue && gaussian_data && gaussian_data != old_gaussian)
    {
        obj.gaussian_data_owner = std::shared_ptr<pnanovdb_raster_gaussian_data_t>(
            gaussian_data,
            [destroy_fn = raster->destroy_gaussian_data, compute_ptr = compute,
             queue_ptr = queue](pnanovdb_raster_gaussian_data_t* ptr)
            {
                if (ptr)
                    destroy_fn(compute_ptr, queue_ptr, ptr);
            });
    }

    // Set up ownership for shader_params_array
    // Only create new shared_ptr if this is a different array than what we already own
    if (compute && params_array && params_array != old_params)
    {
        obj.shader_params_array_owner = std::shared_ptr<pnanovdb_compute_array_t>(
            params_array,
            [destroy_fn = compute->destroy_array](pnanovdb_compute_array_t* ptr)
            {
                if (ptr)
                    destroy_fn(ptr);
            });
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
    return m_objects.erase(key) > 0;
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

} // namespace pnanovdb_editor
