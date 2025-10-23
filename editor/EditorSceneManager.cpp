// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/editor/EditorSceneManager.cpp

    \author Andrew Reidmeyer

    \brief  Implementation of scene management system for tracking multiple objects.
*/

#include "EditorSceneManager.h"

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

    auto& obj = m_objects[key];
    obj.type = SceneObjectType::NanoVDB;
    obj.scene_token = scene;
    obj.name_token = name;
    obj.nanovdb_array = array;
    obj.shader_params_array = params_array;
    obj.shader_params = params_array ? params_array->data : nullptr;
    obj.shader_params_data_type = nullptr;
    obj.shader_name = shader_name ? shader_name : "";
    if (compute && params_array)
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

void EditorSceneManager::add_gaussian_data(pnanovdb_editor_token_t* scene,
                                           pnanovdb_editor_token_t* name,
                                           pnanovdb_raster_gaussian_data_t* gaussian_data,
                                           pnanovdb_compute_array_t* params_array,
                                           const pnanovdb_reflect_data_type_t* shader_params_data_type,
                                           const pnanovdb_compute_t* compute)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    uint64_t key = make_key(scene, name);

    auto& obj = m_objects[key];
    obj.type = SceneObjectType::GaussianData;
    obj.scene_token = scene;
    obj.name_token = name;
    obj.gaussian_data = gaussian_data;
    obj.shader_params_array = params_array;
    obj.shader_params = params_array ? params_array->data : nullptr;
    obj.shader_params_data_type = shader_params_data_type;
    if (compute && params_array)
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
    std::lock_guard<std::mutex> lock(m_mutex);
    uint64_t key = make_key(scene, name);

    auto& obj = m_objects[key];
    obj.type = SceneObjectType::Camera;
    obj.scene_token = scene;
    obj.name_token = name;
    obj.camera_view = camera_view;
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
