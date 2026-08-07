// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/putil/Pipeline.cpp

    \author Andrew Reidmeyer

    \brief  This file provides pipeline runtime implementation.
*/

#include "nanovdb_editor/putil/Pipeline.h"
#include "nanovdb_editor/putil/Camera.h"
#include "nanovdb_editor/putil/Compute.h"
#include "nanovdb_editor/putil/Reflect.h"

#include <mutex>
#include <string>
#include <memory>
#include <unordered_map>
#include <vector>

void pnanovdb_pipeline_register_defaults(const pnanovdb_compute_t* compute,
                                         const pnanovdb_compiler_t* compiler,
                                         pnanovdb_pipeline_registry_t* registry);
void pnanovdb_pipeline_register_gaussians(const pnanovdb_compute_t* compute,
                                          const pnanovdb_compiler_t* compiler,
                                          pnanovdb_pipeline_registry_t* registry);

namespace
{

struct RegistryPipeline
{
    pnanovdb_pipeline_desc_t desc = {};
};

struct Registry
{
    const pnanovdb_compute_t* compute = nullptr;
    const pnanovdb_compiler_t* compiler = nullptr;

    std::vector<std::unique_ptr<RegistryPipeline>> pipelines;
    std::unordered_map<uint64_t, uint64_t> name_id_to_pipeline_id;
    std::mutex registry_mutex;
};
PNANOVDB_CAST_PAIR(Registry, pnanovdb_pipeline_registry_t)

pnanovdb_pipeline_registry_t* create_registry(const pnanovdb_compute_t* compute, const pnanovdb_compiler_t* compiler)
{
    auto ptr = new Registry();

    ptr->compute = compute;
    ptr->compiler = compiler;

    pnanovdb_pipeline_register_defaults(compute, compiler, cast(ptr));
    pnanovdb_pipeline_register_gaussians(compute, compiler, cast(ptr));

    return cast(ptr);
}

void destroy_registry(const pnanovdb_compute_t* compute,
                      const pnanovdb_compiler_t* compiler,
                      pnanovdb_pipeline_registry_t* registry)
{
    auto ptr = cast(registry);


    delete ptr;
}

RegistryPipeline* get_registry_pipeline(Registry* ptr, pnanovdb_compute_token_t* pipeline_name)
{
    auto pipeline_it = ptr->name_id_to_pipeline_id.find(pipeline_name->id);
    if (pipeline_it == ptr->name_id_to_pipeline_id.end())
    {
        // need to create scene
        ptr->name_id_to_pipeline_id[pipeline_name->id] = ptr->pipelines.size();
        ptr->pipelines.push_back(std::move(std::make_unique<RegistryPipeline>()));

        pipeline_it = ptr->name_id_to_pipeline_id.find(pipeline_name->id);
    }
    return ptr->pipelines[pipeline_it->second].get();
}

void register_pipeline(pnanovdb_pipeline_registry_t* registry, const pnanovdb_pipeline_desc_t* desc)
{
    auto ptr = cast(registry);

    std::scoped_lock lock(ptr->registry_mutex);

    RegistryPipeline* pipeline = get_registry_pipeline(ptr, desc->name);
    pipeline->desc = *desc;
}

pnanovdb_bool_t get_pipeline_type(pnanovdb_pipeline_registry_t* registry,
                                  pnanovdb_compute_token_t* name,
                                  pnanovdb_pipeline_type_t* out_type)
{
    auto ptr = cast(registry);

    std::scoped_lock lock(ptr->registry_mutex);

    auto pipeline_it = ptr->name_id_to_pipeline_id.find(name->id);
    if (pipeline_it == ptr->name_id_to_pipeline_id.end())
    {
        return PNANOVDB_FALSE;
    }

    // pipeline_type_t is the array index
    *out_type = pipeline_it->second;

    return PNANOVDB_TRUE;
}

pnanovdb_bool_t get_pipeline_metadesc(pnanovdb_pipeline_registry_t* registry,
                                     pnanovdb_pipeline_type_t type,
                                     pnanovdb_pipeline_metadesc_t* out_metadesc)
{
    auto ptr = cast(registry);

    std::scoped_lock lock(ptr->registry_mutex);

    if (type >= ptr->pipelines.size())
    {
        return PNANOVDB_FALSE;
    }

    pnanovdb_uint64_t stage_mask = 0llu;
    if (ptr->pipelines[type]->desc.load) {stage_mask|= (1u << pnanovdb_pipeline_stage_load);}
    if (ptr->pipelines[type]->desc.process) {stage_mask |= (1u << pnanovdb_pipeline_stage_process);}
    if (ptr->pipelines[type]->desc.render) {stage_mask |= (1u << pnanovdb_pipeline_stage_render);}

    out_metadesc->name = ptr->pipelines[type]->desc.name;
    out_metadesc->stage_mask = stage_mask;
    return PNANOVDB_TRUE;
}

struct ConfigParams
{
    const pnanovdb_reflect_data_type_t* data_type = nullptr;
    std::vector<uint8_t> data;
    const pnanovdb_reflect_data_type_t* metadata_type = nullptr;
    std::vector<uint8_t> metadata;
};

struct ConfigObject
{
    pnanovdb_compute_token_t* name;
    pnanovdb_uint64_t dirty_counter = 1llu;

    std::vector<pnanovdb_compute_array_t*> buffer_arrays;
    std::vector<pnanovdb_uint64_t> buffer_array_versions;
    std::vector<pnanovdb_compute_token_t*> buffer_paths;
    std::vector<pnanovdb_compute_token_t*> buffer_names;

    std::vector<pnanovdb_pipeline_type_t> pipeline_types[pnanovdb_pipeline_stage_count];
    std::vector<ConfigParams> pipeline_params[pnanovdb_pipeline_stage_count];

    std::unique_ptr<pnanovdb_camera_t> camera_default;
    std::unique_ptr<pnanovdb_camera_t> camera;
    std::unique_ptr<pnanovdb_camera_view_t> camera_view;
    std::vector<pnanovdb_camera_config_t> camera_view_configs;
    std::vector<pnanovdb_camera_state_t> camera_view_states;
    pnanovdb_bool_t is_visible = PNANOVDB_TRUE;
};

struct ConfigScene
{
    pnanovdb_compute_token_t* name;

    std::vector<std::unique_ptr<ConfigObject>> objects;

    pnanovdb_compute_token_t* active_camera = nullptr;
};

struct Config
{
    Registry* registry = nullptr;

    std::vector<std::unique_ptr<ConfigScene>> scenes;

    std::vector<pnanovdb_compute_array_t*> owned_arrays;

    pnanovdb_compute_token_t* active_scene = nullptr;

    std::mutex config_mutex;
    std::mutex scene_list_mutex;
    std::mutex object_list_mutex;
    std::mutex object_mutex;

    std::vector<pnanovdb_compute_token_t*> mapped_scene_names;
    pnanovdb_pipeline_scene_list_t mapped_scene_list;

    std::vector<pnanovdb_compute_token_t*> mapped_object_names;
    pnanovdb_pipeline_object_list_t mapped_object_list;

    std::vector<pnanovdb_pipeline_object_mapped_pipeline_t> mapped_pipelines;
    std::vector<pnanovdb_pipeline_object_mapped_stage_t> mapped_stages;
    pnanovdb_pipeline_object_mapped_t mapped_object;
};
PNANOVDB_CAST_PAIR(Config, pnanovdb_pipeline_config_t)

pnanovdb_pipeline_config_t* create_config(pnanovdb_pipeline_registry_t* registry)
{
    auto ctx = cast(registry);
    auto ptr = new Config();

    ptr->registry = ctx;

    return cast(ptr);
}

void destroy_config(pnanovdb_pipeline_registry_t* registry, pnanovdb_pipeline_config_t* config)
{
    auto ctx = cast(registry);
    auto ptr = cast(config);

    for (size_t idx = 0u; idx < ptr->owned_arrays.size(); idx++)
    {
        ctx->compute->destroy_array(ptr->owned_arrays[idx]);
        ptr->owned_arrays[idx] = nullptr;
    }
    ptr->owned_arrays.clear();

    delete ptr;
}

ConfigScene* get_config_scene(Config* ptr, pnanovdb_compute_token_t* scene_name)
{
    size_t idx = 0u;
    for (;idx < ptr->scenes.size(); idx++)
    {
        if (ptr->scenes[idx]->name == scene_name)
        {
            break;
        }
    }
    if (idx == ptr->scenes.size())
    {
        ptr->scenes.push_back(std::move(std::make_unique<ConfigScene>()));
        ptr->scenes.back()->name = scene_name;
    }
    return ptr->scenes[idx].get();
}

ConfigObject* get_config_object(Config* ptr, pnanovdb_compute_token_t* scene_name, pnanovdb_compute_token_t* object_name, bool do_create)
{
    ConfigScene* scene = get_config_scene(ptr, scene_name);

    size_t idx = 0u;
    for (;idx < scene->objects.size(); idx++)
    {
        if (scene->objects[idx]->name == object_name)
        {
            break;
        }
    }
    if (idx == scene->objects.size())
    {
        if (!do_create)
        {
            return nullptr;
        }
        scene->objects.push_back(std::move(std::make_unique<ConfigObject>()));
        scene->objects.back().get()->name = object_name;
    }
    return scene->objects[idx].get();
}

void config_add(pnanovdb_pipeline_config_t* config, pnanovdb_compute_token_t* scene_name, pnanovdb_compute_token_t* object_name)
{
    auto ptr = cast(config);

    if (scene_name && object_name)
    {
        std::scoped_lock lock(ptr->config_mutex, ptr->object_list_mutex);
        get_config_object(ptr, scene_name, object_name, true);
    }
    else if (scene_name)
    {
        std::scoped_lock lock(ptr->config_mutex, ptr->scene_list_mutex);
        get_config_scene(ptr, scene_name);
    }
}

void config_duplicate(pnanovdb_pipeline_config_t* config,
                      pnanovdb_compute_token_t* src_scene,
                      pnanovdb_compute_token_t* src_object,
                      pnanovdb_compute_token_t* dst_scene,
                      pnanovdb_compute_token_t* dst_object)
{
    auto ptr = cast(config);
    std::scoped_lock lock(ptr->config_mutex, ptr->object_list_mutex);

    ConfigObject* src_obj = get_config_object(ptr, src_scene, src_object, false);
    ConfigObject* dst_obj = get_config_object(ptr, dst_scene, dst_object, true);

    if (src_object)
    {
        // need to duplicate arrays to take ownership
        dst_obj->buffer_arrays.clear();
        for (size_t idx = 0u; idx < src_obj->buffer_arrays.size(); idx++)
        {
            pnanovdb_compute_array_t* src_arr = src_obj->buffer_arrays[idx];
            pnanovdb_compute_array_t* dst_arr = nullptr;
            if (src_arr)
            {
                dst_arr = ptr->registry->compute->duplicate_array(src_arr);
            }
            dst_obj->buffer_arrays.push_back(dst_arr);
        }
        dst_obj->buffer_array_versions = src_obj->buffer_array_versions;
        dst_obj->buffer_paths = src_obj->buffer_paths;
        dst_obj->buffer_names = src_obj->buffer_names;

        for (pnanovdb_uint32_t stage_idx = 0u; stage_idx < pnanovdb_pipeline_stage_count; stage_idx++)
        {
            dst_obj->pipeline_types[stage_idx] = src_obj->pipeline_types[stage_idx];
            dst_obj->pipeline_params[stage_idx] = src_obj->pipeline_params[stage_idx];
        }

        if (src_obj->camera_default)
        {
            dst_obj->camera_default = std::make_unique<pnanovdb_camera_t>(*src_obj->camera_default);
        }
        if (src_obj->camera)
        {
            dst_obj->camera = std::make_unique<pnanovdb_camera_t>(*src_obj->camera);
        }
        if (src_obj->camera_view)
        {
            dst_obj->camera_view = std::make_unique<pnanovdb_camera_view_t>(*src_obj->camera_view);

            // need to take ownership of arrays
            dst_obj->camera_view_configs = src_obj->camera_view_configs;
            dst_obj->camera_view_states = src_obj->camera_view_states;
            dst_obj->camera_view->configs = dst_obj->camera_view_configs.data();
            dst_obj->camera_view->states = dst_obj->camera_view_states.data();
        }
    }
}

void config_rename(pnanovdb_pipeline_config_t* config,
                   pnanovdb_compute_token_t* scene_name,
                   pnanovdb_compute_token_t* old_name,
                   pnanovdb_compute_token_t* new_name)
{
    auto ptr = cast(config);

    if (scene_name) // object rename
    {
        std::scoped_lock lock(ptr->config_mutex, ptr->object_list_mutex);
        ConfigScene* scene = get_config_scene(ptr, scene_name);
        ConfigObject* src_obj = get_config_object(ptr, scene_name, old_name, false);
        if (src_obj)
        {
            src_obj->name = new_name;
            // need to migrate camera name
            if (scene->active_camera == old_name)
            {
                scene->active_camera = new_name;
            }
        }
    }
    else // scene rename
    {
        std::scoped_lock lock(ptr->config_mutex, ptr->scene_list_mutex);
        ConfigScene* scene = get_config_scene(ptr, old_name);
        if (scene)
        {
            scene->name = new_name;
            // need to change active_scene if it matches this scene
            if (ptr->active_scene == old_name)
            {
                ptr->active_scene = new_name;
            }
        }
    }
}

void config_remove(pnanovdb_pipeline_config_t* config, pnanovdb_compute_token_t* scene_name, pnanovdb_compute_token_t* object_name)
{
    auto ptr = cast(config);

    if (object_name) // remove object
    {
        // lock object to avoid removal of an actively mapped object
        std::scoped_lock lock(ptr->config_mutex, ptr->object_list_mutex, ptr->object_mutex);
        ConfigScene* scene = get_config_scene(ptr, scene_name);
        if (scene)
        {
            size_t idx = 0u;
            for (;idx < scene->objects.size(); idx++)
            {
                if (scene->objects[idx]->name == object_name)
                {
                    scene->objects.erase(scene->objects.begin() + idx);
                    break;
                }
            }
            // need to invalidate active_camera if it matches this object
            if (scene->active_camera == object_name)
            {
                scene->active_camera = nullptr;
            }
        }
    }
    else // remove scene
    {
        // acquire all locks here, because all mapped types could be impacted
        std::scoped_lock lock(
            ptr->config_mutex, ptr->scene_list_mutex,
            ptr->object_list_mutex, ptr->object_mutex);
        size_t idx = 0u;
        for (;idx < ptr->scenes.size(); idx++)
        {
            if (ptr->scenes[idx]->name == scene_name)
            {
                ptr->scenes.erase(ptr->scenes.begin() + idx);
                break;
            }
        }
        // need to invalidate active_scene if it matches this scene
        if (ptr->active_scene == scene_name)
        {
            ptr->active_scene = nullptr;
        }
    }
}

void config_set_pipeline(pnanovdb_pipeline_config_t* config,
                         pnanovdb_compute_token_t* scene,
                         pnanovdb_compute_token_t* object,
                         pnanovdb_pipeline_stage_t stage,
                         pnanovdb_uint32_t step_idx,
                         pnanovdb_pipeline_type_t type)
{
    auto ptr = cast(config);
    std::scoped_lock lock(ptr->config_mutex, ptr->object_mutex);

    ConfigObject* obj = get_config_object(ptr, scene, object, true);
    pnanovdb_uint32_t stage_idx = pnanovdb_uint32_t(stage);
    if (stage_idx < pnanovdb_pipeline_stage_count)
    {
        if (step_idx >= obj->pipeline_types[stage_idx].size())
        {
            obj->pipeline_types[stage_idx].resize(step_idx + 1u);
        }
        obj->pipeline_types[stage_idx][step_idx] = type;
    }
}

void config_mark_pipeline_dirty(pnanovdb_pipeline_config_t* config,
                                pnanovdb_compute_token_t* scene,
                                pnanovdb_compute_token_t* object)
{
    auto ptr = cast(config);
    std::scoped_lock lock(ptr->config_mutex, ptr->object_mutex);

    ConfigObject* obj = get_config_object(ptr, scene, object, true);
    obj->dirty_counter++;
}

size_t get_config_object_buffer_array_idx(ConfigObject* obj, pnanovdb_compute_token_t* array_name)
{
    size_t idx = 0u;
    for (; idx < obj->buffer_names.size(); idx++)
    {
        if (obj->buffer_names[idx]->id == array_name->id)
        {
            break;
        }
    }
    if (idx == obj->buffer_names.size())
    {
        obj->buffer_names.push_back(array_name);
        obj->buffer_arrays.push_back(nullptr);
        obj->buffer_array_versions.push_back(0llu);
        obj->buffer_paths.push_back(nullptr);
    }
    return idx;
}

void config_set_buffer_array(pnanovdb_pipeline_config_t* config,
                            pnanovdb_compute_token_t* scene,
                            pnanovdb_compute_token_t* object,
                            pnanovdb_compute_token_t* array_name,
                            pnanovdb_compute_array_t* compute_array)
{
    auto ptr = cast(config);
    std::scoped_lock lock(ptr->config_mutex, ptr->object_mutex);

    ConfigObject* obj = get_config_object(ptr, scene, object, true);
    size_t idx = get_config_object_buffer_array_idx(obj, array_name);

    if (obj->buffer_arrays[idx])
    {
        ptr->registry->compute->destroy_array(obj->buffer_arrays[idx]);
    }
    obj->buffer_arrays[idx] = compute_array ? ptr->registry->compute->duplicate_array(compute_array) : nullptr;
    obj->buffer_array_versions[idx]++;
    ptr->owned_arrays.push_back(obj->buffer_arrays[idx]);
}

void config_set_buffer_path(pnanovdb_pipeline_config_t* config,
                           pnanovdb_compute_token_t* scene,
                           pnanovdb_compute_token_t* object,
                           pnanovdb_compute_token_t* array_name,
                           pnanovdb_compute_token_t* path)
{
    auto ptr = cast(config);
    std::scoped_lock lock(ptr->config_mutex, ptr->object_mutex);

    ConfigObject* obj = get_config_object(ptr, scene, object, true);
    size_t idx = get_config_object_buffer_array_idx(obj, array_name);

    obj->buffer_paths[idx] = path;
}

void config_set_camera(pnanovdb_pipeline_config_t* config,
                       pnanovdb_compute_token_t* scene,
                       pnanovdb_compute_token_t* object,
                       pnanovdb_camera_t* camera)
{
    if (!object)
    {
        return;
    }

    auto ptr = cast(config);
    std::scoped_lock lock(ptr->config_mutex, ptr->object_mutex);

    ConfigObject* obj = get_config_object(ptr, scene, object, true);
    if (!obj->camera_default.get())
    {
        obj->camera_default = std::make_unique<pnanovdb_camera_t>();
        *obj->camera_default = *camera;
    }
    if (!obj->camera.get())
    {
        obj->camera = std::make_unique<pnanovdb_camera_t>();
    }
    *obj->camera = *camera;
}

void config_set_camera_view(pnanovdb_pipeline_config_t* config,
                            pnanovdb_compute_token_t* scene,
                            pnanovdb_compute_token_t* object,
                            pnanovdb_camera_view_t* camera_view)
{
    auto ptr = cast(config);
    std::scoped_lock lock(ptr->config_mutex, ptr->object_mutex);

    ConfigObject* obj = get_config_object(ptr, scene, object, true);
    if (!obj->camera_view.get())
    {
        obj->camera_view = std::make_unique<pnanovdb_camera_view_t>();
    }
    *obj->camera_view = *camera_view;
    obj->camera_view_configs.resize(camera_view->num_cameras);
    obj->camera_view_states.resize(camera_view->num_cameras);
    obj->camera_view->configs = obj->camera_view_configs.data();
    obj->camera_view->states = obj->camera_view_states.data();
    for (pnanovdb_uint32_t idx = 0u; idx < camera_view->num_cameras; idx++)
    {
        obj->camera_view->configs[idx] = camera_view->configs[idx];
        obj->camera_view->states[idx] = camera_view->states[idx];
    }

}

static void config_object_sync_param_types(Config* ptr, ConfigObject* obj)
{
    for (size_t stage_idx = 0u; stage_idx < pnanovdb_pipeline_stage_count; stage_idx++)
    {
        for (size_t pipeline_idx = 0u; pipeline_idx < obj->pipeline_types[stage_idx].size(); pipeline_idx++)
        {
            pnanovdb_pipeline_type_t pipeline_type = obj->pipeline_types[stage_idx][pipeline_idx];
            RegistryPipeline* pipeline = ptr->registry->pipelines[pipeline_type].get();
            const pnanovdb_reflect_data_type_t* data_type = nullptr;
            const pnanovdb_reflect_data_type_t* metadata_type = nullptr;
            if (pipeline->desc.get_pipeline_params_type)
            {
                pipeline->desc.get_pipeline_params_type(
                    ptr->registry->compute, ptr->registry->compiler, &data_type, &metadata_type);
            }

            // align params array size to current pipeline count
            if (obj->pipeline_params[stage_idx].size() != obj->pipeline_types[stage_idx].size())
            {
                obj->pipeline_params[stage_idx].resize(obj->pipeline_types[stage_idx].size());
            }
            // align param entry to current type as need
            if (!data_type && obj->pipeline_params[stage_idx][pipeline_idx].data_type)
            {
                obj->pipeline_params[stage_idx][pipeline_idx].data_type = nullptr;
                obj->pipeline_params[stage_idx][pipeline_idx].data.clear();
            }
            else if (obj->pipeline_params[stage_idx][pipeline_idx].data_type != data_type)
            {
                obj->pipeline_params[stage_idx][pipeline_idx].data_type = data_type;
                obj->pipeline_params[stage_idx][pipeline_idx].data.resize(data_type->element_size);
                if (data_type->default_value)
                {
                    memcpy(obj->pipeline_params[stage_idx][pipeline_idx].data.data(), data_type->default_value,
                           data_type->element_size);
                }
                else
                {
                    memset(obj->pipeline_params[stage_idx][pipeline_idx].data.data(), 0, data_type->element_size);
                }
            }
            if (!metadata_type && obj->pipeline_params[stage_idx][pipeline_idx].metadata_type)
            {
                obj->pipeline_params[stage_idx][pipeline_idx].metadata_type = nullptr;
                obj->pipeline_params[stage_idx][pipeline_idx].metadata.clear();
            }
            else if (obj->pipeline_params[stage_idx][pipeline_idx].metadata_type != metadata_type)
            {
                obj->pipeline_params[stage_idx][pipeline_idx].metadata_type = metadata_type;
                obj->pipeline_params[stage_idx][pipeline_idx].metadata.resize(metadata_type->element_size);
                if (metadata_type->default_value)
                {
                    memcpy(obj->pipeline_params[stage_idx][pipeline_idx].metadata.data(), metadata_type->default_value,
                        metadata_type->element_size);
                }
                else
                {
                    memset(obj->pipeline_params[stage_idx][pipeline_idx].metadata.data(), 0, metadata_type->element_size);
                }
            }
        }
    }
}

pnanovdb_bool_t config_map_params(pnanovdb_pipeline_config_t* config,
                                  pnanovdb_compute_token_t* scene,
                                  pnanovdb_compute_token_t* object,
                                  const pnanovdb_reflect_data_type_t* map_data_type,
                                  void** mapped_data)
{
    auto ptr = cast(config);

    if (pnanovdb_reflect_layout_compare(map_data_type, PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_pipeline_scene_list_t)))
    {
        std::lock(ptr->config_mutex, ptr->scene_list_mutex);

        ptr->mapped_scene_names.resize(ptr->scenes.size());
        for (size_t idx = 0u; idx < ptr->scenes.size(); idx++)
        {
            ptr->mapped_scene_names[idx] = ptr->scenes[idx].get()->name;
        }
        ptr->mapped_scene_list.scenes = ptr->mapped_scene_names.data();
        ptr->mapped_scene_list.scene_count = ptr->mapped_scene_names.size();

        *mapped_data = &ptr->mapped_scene_list;
        ptr->config_mutex.unlock();
        return PNANOVDB_TRUE;
    }
    else if (pnanovdb_reflect_layout_compare(map_data_type, PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_pipeline_object_list_t)))
    {
        std::lock(ptr->config_mutex, ptr->object_list_mutex);

        ConfigScene* config_scene = get_config_scene(ptr, scene);

        ptr->mapped_object_names.resize(config_scene->objects.size());
        for (size_t idx = 0u; idx < config_scene->objects.size(); idx++)
        {
            ptr->mapped_object_names[idx] = config_scene->objects[idx].get()->name;
        }
        ptr->mapped_object_list.objects = ptr->mapped_object_names.data();
        ptr->mapped_object_list.object_count = ptr->mapped_object_names.size();

        *mapped_data = &ptr->mapped_object_list;
        ptr->config_mutex.unlock();
        return PNANOVDB_TRUE;
    }
    else if (pnanovdb_reflect_layout_compare(map_data_type, PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_pipeline_object_mapped_t)))
    {
        if (!object)
        {
            return PNANOVDB_FALSE;
        }
        std::lock(ptr->config_mutex, ptr->object_mutex);

        ConfigObject* obj = get_config_object(ptr, scene, object, true);

        // make sure param types/sizes are up to date
        config_object_sync_param_types(ptr, obj);

        ptr->mapped_stages.resize(pnanovdb_pipeline_stage_count);
        size_t total_pipelines = 0u;
        for (size_t stage_idx = 0u; stage_idx < pnanovdb_pipeline_stage_count; stage_idx++)
        {
            total_pipelines += obj->pipeline_types[stage_idx].size();
        }
        ptr->mapped_pipelines.resize(total_pipelines);

        size_t pipeline_offset = 0u;
        for (size_t stage_idx = 0u; stage_idx < pnanovdb_pipeline_stage_count; stage_idx++)
        {
            ptr->mapped_stages[stage_idx].pipelines = &ptr->mapped_pipelines[pipeline_offset];
            ptr->mapped_stages[stage_idx].pipeline_count = obj->pipeline_types[stage_idx].size();
            for (size_t pipeline_idx = 0u; pipeline_idx < obj->pipeline_types[stage_idx].size(); pipeline_idx++)
            {
                pnanovdb_pipeline_object_mapped_pipeline_t* mapped =
                    &ptr->mapped_stages[stage_idx].pipelines[pipeline_idx];
                mapped->pipeline_type = obj->pipeline_types[stage_idx][pipeline_idx];

                mapped->params_data = obj->pipeline_params[stage_idx][pipeline_idx].data.data();
                mapped->params_data_type = obj->pipeline_params[stage_idx][pipeline_idx].data_type;
                mapped->params_metadata = obj->pipeline_params[stage_idx][pipeline_idx].metadata.data();
                mapped->params_metadata_type = obj->pipeline_params[stage_idx][pipeline_idx].metadata_type;
            }
            pipeline_offset += obj->pipeline_types[stage_idx].size();
        }

        ptr->mapped_object.buffer_arrays = obj->buffer_arrays.data();
        ptr->mapped_object.buffer_array_versions = obj->buffer_array_versions.data();
        ptr->mapped_object.buffer_paths = obj->buffer_paths.data();
        ptr->mapped_object.buffer_names = obj->buffer_names.data();
        ptr->mapped_object.buffer_count = obj->buffer_paths.size();

        ptr->mapped_object.stages = ptr->mapped_stages.data();
        ptr->mapped_object.stage_count = ptr->mapped_stages.size();

        ptr->mapped_object.camera_default = obj->camera_default.get();
        ptr->mapped_object.camera = obj->camera.get();
        ptr->mapped_object.camera_view = obj->camera_view.get();
        ptr->mapped_object.is_visible = &obj->is_visible;

        *mapped_data = &ptr->mapped_object;
        ptr->config_mutex.unlock();
        return PNANOVDB_TRUE;
    }

    return PNANOVDB_FALSE;
}

void config_unmap_params(pnanovdb_pipeline_config_t* config,
                         pnanovdb_compute_token_t* scene,
                         pnanovdb_compute_token_t* object,
                         const pnanovdb_reflect_data_type_t* map_data_type)
{
    auto ptr = cast(config);

    if (pnanovdb_reflect_layout_compare(map_data_type, PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_pipeline_scene_list_t)))
    {
        ptr->scene_list_mutex.unlock();
    }
    else if (pnanovdb_reflect_layout_compare(map_data_type, PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_pipeline_object_list_t)))
    {
        ptr->object_list_mutex.unlock();
    }
    else if (pnanovdb_reflect_layout_compare(map_data_type, PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_pipeline_object_mapped_t)))
    {
        ptr->object_mutex.unlock();
    }
}

void config_set_active_camera(pnanovdb_pipeline_config_t* config,
                              pnanovdb_compute_token_t* scene,
                              pnanovdb_compute_token_t* object)
{
    auto ptr = cast(config);
    std::scoped_lock lock(ptr->config_mutex);

    ConfigScene* config_scene = get_config_scene(ptr, scene);
    config_scene->active_camera = object;
}

pnanovdb_compute_token_t* config_get_active_camera(pnanovdb_pipeline_config_t* config, pnanovdb_compute_token_t* scene)
{
    auto ptr = cast(config);
    std::scoped_lock lock(ptr->config_mutex);

    ConfigScene* config_scene = get_config_scene(ptr, scene);

    return config_scene->active_camera;
}

void config_set_active_scene(pnanovdb_pipeline_config_t* config, pnanovdb_compute_token_t* scene)
{
    auto ptr = cast(config);
    std::scoped_lock lock(ptr->config_mutex);

    ptr->active_scene = scene;
}

pnanovdb_compute_token_t* config_get_active_scene(pnanovdb_pipeline_config_t* config)
{
    auto ptr = cast(config);
    std::scoped_lock lock(ptr->config_mutex);

    // force valid scene
    if (!ptr->active_scene && !ptr->scenes.empty())
    {
        ptr->active_scene = ptr->scenes.front()->name;
    }
    return ptr->active_scene;
}

struct RuntimeObject
{
    // key to validate cached object is valid
    pnanovdb_compute_token_t* object_name = nullptr;

    // pipeline array indices
    std::vector<pnanovdb_uint64_t> pipeline_first_indices;

    std::vector<pnanovdb_compute_array_t*> buffer_arrays;
    std::vector<pnanovdb_uint64_t> buffer_array_versions;
    std::vector<pnanovdb_compute_buffer_t*> buffer_buffers;
    std::vector<pnanovdb_uint64_t> buffer_buffer_sizes;
    std::vector<pnanovdb_uint64_t> buffer_buffer_versions;
    std::vector<pnanovdb_compute_token_t*> buffer_paths;
    std::vector<pnanovdb_compute_token_t*> buffer_names;

    std::vector<pnanovdb_compute_buffer_t*> buffer_buffers_tmp;
    std::vector<pnanovdb_uint64_t> buffer_buffer_sizes_tmp;
    std::vector<pnanovdb_uint64_t> buffer_buffer_versions_tmp;
};

struct RuntimePipeline
{
    // key to validate cache pipeline is valid
    pnanovdb_uint32_t stage_idx = 0u;
    pnanovdb_uint32_t pipeline_idx = 0u;
    pnanovdb_pipeline_type_t pipeline_type = 0u;

    pnanovdb_pipeline_desc_t pipeline_desc = {};
    pnanovdb_pipeline_instance_t* instance = nullptr;
};

enum shader
{
    checkerboard_color_slang,
    clear_color_slang,
    composite_color_slang,

    shader_count
};

static const char* s_shader_names[shader_count] = {
    "compute/checkerboard_color.slang",
    "compute/clear_color.slang",
    "compute/composite_color.slang"
};

struct Runtime
{
    // key to validate cached runtime is valid
    pnanovdb_compute_token_t* scene_name = nullptr;
    std::vector<std::unique_ptr<RuntimeObject>> objects;
    std::vector<std::unique_ptr<RuntimePipeline>> pipelines;

    pnanovdb_shader_context_t* shader_ctx[shader_count];
};
PNANOVDB_CAST_PAIR(Runtime, pnanovdb_pipeline_runtime_t)

pnanovdb_pipeline_runtime_t* create_runtime(const pnanovdb_compute_t* compute, pnanovdb_compute_queue_t* queue)
{
    auto ptr = new Runtime();

    pnanovdb_compiler_settings_t compile_settings = {};
    pnanovdb_compiler_settings_init(&compile_settings);
    for (pnanovdb_uint32_t idx = 0u; idx < shader_count; idx++)
    {
        ptr->shader_ctx[idx] = compute->create_shader_context(s_shader_names[idx]);
        compute->init_shader(compute, queue, ptr->shader_ctx[idx], &compile_settings);
    }

    return cast(ptr);
}

static void release_runtime_pipelines(const pnanovdb_compute_t* compute,
                                      pnanovdb_compute_queue_t* queue,
                                      Runtime* ptr)
{
    // invalidate objects, as they did not own their resources
    for (size_t object_idx = 0u; object_idx < ptr->objects.size(); object_idx++)
    {
        auto& object = ptr->objects[object_idx];

        object->buffer_arrays.clear();
        object->buffer_array_versions.clear();
        object->buffer_buffers.clear();
        object->buffer_buffer_sizes.clear();
        object->buffer_buffer_versions.clear();
        object->buffer_paths.clear();
        object->buffer_names.clear();
    }
    ptr->objects.clear();

    // releases both pipelines and the resources they owned
    for (size_t pipeline_idx = 0u; pipeline_idx < ptr->pipelines.size(); pipeline_idx++)
    {
        auto& pipeline = ptr->pipelines[pipeline_idx];

        if (pipeline->pipeline_desc.destroy)
        {
            pipeline->pipeline_desc.destroy(compute, queue, pipeline->instance);
        }
        pipeline->instance = nullptr;
    }
    ptr->pipelines.clear();
}

void destroy_runtime(const pnanovdb_compute_t* compute,
                     pnanovdb_compute_queue_t* queue,
                     pnanovdb_pipeline_runtime_t* runtime)
{
    auto ptr = cast(runtime);

    release_runtime_pipelines(compute, queue, ptr);

    for (pnanovdb_uint32_t idx = 0u; idx < shader_count; idx++)
    {
        compute->destroy_shader_context(compute, queue, ptr->shader_ctx[idx]);
    }

    delete ptr;
}

static bool update_runtime_is_dirty(pnanovdb_pipeline_runtime_t* runtime,
                                    pnanovdb_pipeline_config_t* config,
                                    pnanovdb_compute_token_t* scene,
                                    pnanovdb_pipeline_object_list_t* mapped_object_list)
{
    auto ptr = cast(runtime);

    if (ptr->scene_name != scene)
    {
        return true;
    }
    if (ptr->objects.size() != mapped_object_list->object_count)
    {
        return true;
    }
    for (pnanovdb_uint64_t object_idx = 0u; object_idx < mapped_object_list->object_count; object_idx++)
    {
        if (ptr->objects[object_idx]->object_name != mapped_object_list->objects[object_idx])
        {
            return true;
        }
    }
    bool pipeline_dirty = false;
    for (pnanovdb_uint64_t object_idx = 0u; object_idx < mapped_object_list->object_count; object_idx++)
    {
        RuntimeObject* object_run = ptr->objects[object_idx].get();

        pnanovdb_pipeline_object_mapped_t* mapped_object = nullptr;
        if(!config_map_params(config, scene, mapped_object_list->objects[object_idx], PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_pipeline_object_mapped_t), (void**)&mapped_object))
        {
            continue;
        }
        for (pnanovdb_uint64_t stage_idx = 0u; stage_idx < mapped_object->stage_count; stage_idx++)
        {
            pnanovdb_pipeline_object_mapped_stage_t* stage = &mapped_object->stages[stage_idx];
            pnanovdb_uint64_t pipeline_first_index = object_run->pipeline_first_indices[stage_idx];
            for (pnanovdb_uint64_t pipeline_idx = 0u; pipeline_idx < stage->pipeline_count; pipeline_idx++)
            {
                if (pipeline_first_index + pipeline_idx < ptr->pipelines.size())
                {
                    RuntimePipeline* pipeline_run = ptr->pipelines[pipeline_first_index + pipeline_idx].get();
                    if (pipeline_run->stage_idx != stage_idx ||
                        pipeline_run->pipeline_idx != pipeline_idx ||
                        pipeline_run->pipeline_type != stage->pipelines[pipeline_idx].pipeline_type)
                    {
                        // cannot simply return here because unmap() must be called
                        pipeline_dirty = true;
                    }
                }
                else
                {
                    pipeline_dirty = true;
                }
            }
        }
        config_unmap_params(config, scene, mapped_object_list->objects[object_idx], PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_pipeline_object_mapped_t));
    }
    return pipeline_dirty;
}

static void invalidate_shader_params(const pnanovdb_compute_t* compute,
                                     pnanovdb_pipeline_config_t* config,
                                     pnanovdb_compute_token_t* scene,
                                     pnanovdb_pipeline_object_list_t* mapped_object_list)
{
    for (pnanovdb_uint64_t object_idx = 0u; object_idx < mapped_object_list->object_count; object_idx++)
    {
        pnanovdb_pipeline_object_mapped_t* mapped_object = nullptr;
        if(!config_map_params(config, scene, mapped_object_list->objects[object_idx], PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_pipeline_object_mapped_t), (void**)&mapped_object))
        {
            continue;
        }
        for (pnanovdb_uint64_t stage_idx = 0u; stage_idx < mapped_object->stage_count; stage_idx++)
        {
            pnanovdb_pipeline_object_mapped_stage_t* stage = &mapped_object->stages[stage_idx];
            for (pnanovdb_uint64_t pipeline_idx = 0u; pipeline_idx < stage->pipeline_count; pipeline_idx++)
            {
                pnanovdb_pipeline_object_mapped_pipeline_t* pipe = &stage->pipelines[pipeline_idx];
                if (pipe->params_data && pipe->params_data_type)
                {
                    for (pnanovdb_uint64_t idx = 0u; idx < pipe->params_data_type->child_reflect_data_count; idx++)
                    {
                        auto reflect_data = &pipe->params_data_type->child_reflect_datas[idx];
                        if (pnanovdb_reflect_layout_compare(
                                reflect_data->data_type,
                                PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_pipeline_shader_t)))
                        {
                            pnanovdb_pipeline_shader_t* shader = (pnanovdb_pipeline_shader_t*)(((uint8_t*)pipe->params_data) + reflect_data->data_offset);
                            shader->params_data = nullptr;
                            shader->params_data_type = nullptr;
                            shader->params_metadata = nullptr;
                            shader->params_metadata_type = nullptr;
                        }
                    }
                }
            }
        }
        config_unmap_params(config, scene, mapped_object_list->objects[object_idx], PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_pipeline_object_mapped_t));
    }
}

static void build_new_pipelines(const pnanovdb_compute_t* compute,
                                pnanovdb_compute_queue_t* queue,
                                pnanovdb_pipeline_runtime_t* runtime,
                                pnanovdb_pipeline_config_t* config,
                                pnanovdb_compute_token_t* scene,
                                pnanovdb_pipeline_object_list_t* mapped_object_list)
{
    auto ptr = cast(runtime);
    auto config_impl = cast(config);

    ptr->scene_name = scene;
    for (pnanovdb_uint64_t object_idx = 0u; object_idx < mapped_object_list->object_count; object_idx++)
    {
        pnanovdb_pipeline_object_mapped_t* mapped_object = nullptr;
        if(!config_map_params(config, scene, mapped_object_list->objects[object_idx], PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_pipeline_object_mapped_t), (void**)&mapped_object))
        {
            continue;
        }

        std::unique_ptr<RuntimeObject> object_run = std::make_unique<RuntimeObject>();
        object_run->object_name = mapped_object_list->objects[object_idx];
        for (pnanovdb_uint64_t stage_idx = 0u; stage_idx < mapped_object->stage_count; stage_idx++)
        {
            object_run->pipeline_first_indices.push_back(ptr->pipelines.size());
            pnanovdb_pipeline_object_mapped_stage_t* stage = &mapped_object->stages[stage_idx];
            for (pnanovdb_uint64_t pipeline_idx = 0u; pipeline_idx < stage->pipeline_count; pipeline_idx++)
            {
                std::unique_ptr<RuntimePipeline> pipeline_run = std::make_unique<RuntimePipeline>();
                pipeline_run->stage_idx = stage_idx;
                pipeline_run->pipeline_idx = pipeline_idx;
                pipeline_run->pipeline_type = stage->pipelines[pipeline_idx].pipeline_type;

                pnanovdb_pipeline_type_t pipeline_type = pipeline_run->pipeline_type;
                RegistryPipeline* pipeline_reg = config_impl->registry->pipelines[pipeline_type].get();

                pipeline_run->pipeline_desc = pipeline_reg->desc;
                if (pipeline_run->pipeline_desc.create)
                {
                    pipeline_run->instance = pipeline_run->pipeline_desc.create(compute, queue);
                }

                ptr->pipelines.push_back(std::move(pipeline_run));
            }
        }
        ptr->objects.push_back(std::move(object_run));
        config_unmap_params(config, scene, mapped_object_list->objects[object_idx], PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_pipeline_object_mapped_t));
    }
}


void update_runtime_main(const pnanovdb_compute_t* compute,
                         pnanovdb_compute_queue_t* queue,
                         pnanovdb_pipeline_runtime_t* runtime,
                         pnanovdb_pipeline_config_t* config,
                         pnanovdb_pipeline_runtime_target_t* target_in)
{
    auto ptr = cast(runtime);

    // get camera info
    pnanovdb_compute_token_t* scene = config_get_active_scene(config);
    pnanovdb_compute_token_t* camera_name = config_get_active_camera(config, scene);
    pnanovdb_camera_t camera_data = {};
    pnanovdb_camera_init(&camera_data);
    pnanovdb_camera_state_default(&camera_data.state, PNANOVDB_TRUE);
    pnanovdb_camera_config_default(&camera_data.config);
    {
        pnanovdb_pipeline_object_mapped_t* mapped_object = nullptr;
        if(config_map_params(config, scene, camera_name, PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_pipeline_object_mapped_t), (void**)&mapped_object))
        {
            if (mapped_object->camera)
            {
                camera_data = *mapped_object->camera;
            }
            config_unmap_params(config, scene, camera_name, PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_pipeline_object_mapped_t));
        }
    }
    pnanovdb_camera_mat_t view = {};
    pnanovdb_camera_get_view(&camera_data, &view);
    pnanovdb_camera_mat_t view_inv = pnanovdb_camera_mat_inverse(view);
    pnanovdb_camera_mat_t projection = {};
    pnanovdb_camera_get_projection(&camera_data, &projection, (float)target_in->width, (float)target_in->height);
    pnanovdb_camera_mat_t projection_inv = pnanovdb_camera_mat_inverse(projection);

    pnanovdb_pipeline_object_list_t* mapped_object_list = nullptr;
    if(!config_map_params(config, scene, nullptr, PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_pipeline_object_list_t), (void**)&mapped_object_list))
    {
        return;
    }

    // allow pipelines to request required buffer names
    for (pnanovdb_uint64_t object_idx = 0u; object_idx < mapped_object_list->object_count; object_idx++)
    {
        pnanovdb_pipeline_object_mapped_t* mapped_object = nullptr;
        if(!config_map_params(config, scene, mapped_object_list->objects[object_idx], PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_pipeline_object_mapped_t), (void**)&mapped_object))
        {
            continue;
        }
        std::vector<pnanovdb_compute_token_t*> new_names;
        for (pnanovdb_uint64_t stage_idx = 0u; stage_idx < mapped_object->stage_count; stage_idx++)
        {
            pnanovdb_pipeline_object_mapped_stage_t* stage = &mapped_object->stages[stage_idx];
            pnanovdb_uint64_t pipeline_count = stage->pipeline_count;
            for (pnanovdb_uint64_t pipeline_idx = 0u; pipeline_idx < pipeline_count; pipeline_idx++)
            {
                pnanovdb_pipeline_object_mapped_pipeline_t* pipe = &stage->pipelines[pipeline_idx];
                pnanovdb_pipeline_type_t pipeline_type = pipe->pipeline_type;
                auto config_impl = cast(config);
                RegistryPipeline* pipeline_reg = config_impl->registry->pipelines[pipeline_type].get();
                if (pipeline_reg->desc.get_requirements)
                {
                    pnanovdb_pipeline_requirements_params_t requirements_params = {};
                    requirements_params.params_data = pipe->params_data;
                    requirements_params.params_data_type = pipe->params_data_type;
                    pnanovdb_pipeline_requirements_output_t requirements_output = {};
                    const char* names_stack[16u] = {};
                    pnanovdb_uint64_t name_count = 16u;
                    requirements_output.p_name_count = &name_count;
                    requirements_output.names = names_stack;
                    pnanovdb_bool_t got_all = pipeline_reg->desc.get_requirements(&requirements_params, &requirements_output);
                    std::vector<const char*> names_heap;
                    if (!got_all)
                    {
                        requirements_output.names = nullptr;
                        pipeline_reg->desc.get_requirements(&requirements_params, &requirements_output);
                        names_heap.resize(*requirements_output.p_name_count);
                        requirements_output.names = names_heap.data();
                        pipeline_reg->desc.get_requirements(&requirements_params, &requirements_output);
                    }
                    for (pnanovdb_uint64_t name_idx = 0u; name_idx < *requirements_output.p_name_count; name_idx++)
                    {
                        const char* name = requirements_output.names[name_idx];
                        bool did_find = false;
                        for (pnanovdb_uint64_t cmp_idx = 0u; cmp_idx < mapped_object->buffer_count; cmp_idx++)
                        {
                            if (pnanovdb_reflect_string_compare(mapped_object->buffer_names[cmp_idx]->str, name) == 0)
                            {
                                did_find = true;
                            }
                        }
                        if (!did_find)
                        {
                            new_names.push_back(compute->get_token(name));
                        }
                    }
                }
            }
        }
        config_unmap_params(config, scene, mapped_object_list->objects[object_idx], PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_pipeline_object_mapped_t));
        for (size_t idx = 0u; idx < new_names.size(); idx++)
        {
            config_set_buffer_array(config, scene, mapped_object_list->objects[object_idx],
                                   new_names[idx], nullptr);
        }
    }

    // check if cached runtime is valid, rebuild as needed
    if (update_runtime_is_dirty(runtime, config, scene, mapped_object_list))
    {
        invalidate_shader_params(compute, config, scene, mapped_object_list);
        release_runtime_pipelines(compute, queue, ptr);
        build_new_pipelines(compute, queue, runtime, config, scene, mapped_object_list);
    }

    for (pnanovdb_uint64_t object_idx = 0u; object_idx < mapped_object_list->object_count; object_idx++)
    {
        pnanovdb_pipeline_object_mapped_t* mapped_object = nullptr;
        if(!config_map_params(config, scene, mapped_object_list->objects[object_idx], PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_pipeline_object_mapped_t), (void**)&mapped_object))
        {
            continue;
        }
        RuntimeObject* object_run = ptr->objects[object_idx].get();
        // initialize object state to config based input
        object_run->buffer_arrays.resize(mapped_object->buffer_count);
        object_run->buffer_array_versions.resize(mapped_object->buffer_count);
        object_run->buffer_buffers.resize(mapped_object->buffer_count);
        object_run->buffer_buffer_sizes.resize(mapped_object->buffer_count);
        object_run->buffer_buffer_versions.resize(mapped_object->buffer_count);
        object_run->buffer_paths.resize(mapped_object->buffer_count);
        object_run->buffer_names.resize(mapped_object->buffer_count);
        for (pnanovdb_uint64_t buffer_idx = 0u; buffer_idx < mapped_object->buffer_count; buffer_idx++)
        {
            object_run->buffer_arrays[buffer_idx] = mapped_object->buffer_arrays[buffer_idx];
            object_run->buffer_array_versions[buffer_idx] = mapped_object->buffer_array_versions[buffer_idx];
            object_run->buffer_buffers[buffer_idx] = nullptr; // Today not a form of config
            object_run->buffer_buffer_versions[buffer_idx] = 0u;
            object_run->buffer_paths[buffer_idx] = mapped_object->buffer_paths[buffer_idx];
            object_run->buffer_names[buffer_idx] = mapped_object->buffer_names[buffer_idx];
        }
        config_unmap_params(config, scene, mapped_object_list->objects[object_idx], PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_pipeline_object_mapped_t));
    }

    for (pnanovdb_uint64_t object_idx = 0u; object_idx < mapped_object_list->object_count; object_idx++)
    {
        pnanovdb_pipeline_object_mapped_t* mapped_object = nullptr;
        if(!config_map_params(config, scene, mapped_object_list->objects[object_idx], PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_pipeline_object_mapped_t), (void**)&mapped_object))
        {
            continue;
        }
        RuntimeObject* object_run = ptr->objects[object_idx].get();
        pnanovdb_uint64_t pipeline_count = mapped_object->stages[pnanovdb_pipeline_stage_load].pipeline_count;
        for (pnanovdb_uint64_t pipeline_idx = 0u; pipeline_idx < pipeline_count; pipeline_idx++)
        {
            pnanovdb_pipeline_object_mapped_stage_t* stage = &mapped_object->stages[pnanovdb_pipeline_stage_load];
            pnanovdb_uint64_t pipeline_first_index = object_run->pipeline_first_indices[pnanovdb_pipeline_stage_load];
            RuntimePipeline* pipeline_run = ptr->pipelines[pipeline_first_index + pipeline_idx].get();
            auto& pipeline_config = stage->pipelines[pipeline_idx];
            if (pipeline_run->pipeline_desc.load)
            {
                pnanovdb_pipeline_load_input_t load_input = {};
                load_input.buffer_count = object_run->buffer_paths.size();
                load_input.buffer_paths = object_run->buffer_paths.data();
                load_input.buffer_arrays = object_run->buffer_arrays.data();
                load_input.buffer_array_versions = object_run->buffer_array_versions.data();
                load_input.buffer_names = object_run->buffer_names.data();
                load_input.params_data_type = pipeline_config.params_data_type;
                load_input.params_data = pipeline_config.params_data;
                pnanovdb_pipeline_output_t output = {};
                output.buffer_count = object_run->buffer_buffers.size();
                output.buffer_buffers = object_run->buffer_buffers.data();
                output.buffer_sizes = object_run->buffer_buffer_sizes.data();
                output.buffer_versions = object_run->buffer_buffer_versions.data();
                output.buffer_names = object_run->buffer_names.data();
                pipeline_run->pipeline_desc.load(
                    pipeline_run->instance,
                    compute,
                    queue,
                    &load_input,
                    &output);
            }
        }
        config_unmap_params(config, scene, mapped_object_list->objects[object_idx], PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_pipeline_object_mapped_t));
    }

    for (pnanovdb_uint64_t object_idx = 0u; object_idx < mapped_object_list->object_count; object_idx++)
    {
        pnanovdb_pipeline_object_mapped_t* mapped_object = nullptr;
        if(!config_map_params(config, scene, mapped_object_list->objects[object_idx], PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_pipeline_object_mapped_t), (void**)&mapped_object))
        {
            continue;
        }
        RuntimeObject* object_run = ptr->objects[object_idx].get();
        pnanovdb_uint64_t pipeline_count = mapped_object->stages[pnanovdb_pipeline_stage_process].pipeline_count;
        for (pnanovdb_uint64_t pipeline_idx = 0u; pipeline_idx < pipeline_count; pipeline_idx++)
        {
            pnanovdb_pipeline_object_mapped_stage_t* stage = &mapped_object->stages[pnanovdb_pipeline_stage_process];
            pnanovdb_uint64_t pipeline_first_index = object_run->pipeline_first_indices[pnanovdb_pipeline_stage_process];
            RuntimePipeline* pipeline_run = ptr->pipelines[pipeline_first_index + pipeline_idx].get();
            auto& pipeline_config = stage->pipelines[pipeline_idx];
            if (pipeline_run->pipeline_desc.process)
            {
                // make copy of named buffers to avoid read/write collision
                object_run->buffer_buffers_tmp = object_run->buffer_buffers;
                object_run->buffer_buffer_sizes_tmp = object_run->buffer_buffer_sizes;
                object_run->buffer_buffer_versions_tmp = object_run->buffer_buffer_versions;
                pnanovdb_pipeline_input_t input = {};
                input.buffer_count = object_run->buffer_buffers.size();
                input.buffer_buffers = object_run->buffer_buffers.data();
                input.buffer_sizes = object_run->buffer_buffer_sizes.data();
                input.buffer_versions = object_run->buffer_buffer_versions.data();
                input.buffer_names = object_run->buffer_names.data();
                input.params_data_type = pipeline_config.params_data_type;
                input.params_data = pipeline_config.params_data;
                pnanovdb_pipeline_output_t output = {};
                output.buffer_count = object_run->buffer_buffers_tmp.size();
                output.buffer_buffers = object_run->buffer_buffers_tmp.data();
                output.buffer_sizes = object_run->buffer_buffer_sizes_tmp.data();
                output.buffer_versions = object_run->buffer_buffer_versions_tmp.data();
                output.buffer_names = object_run->buffer_names.data();
                pipeline_run->pipeline_desc.process(
                    pipeline_run->instance,
                    compute,
                    queue,
                    &input,
                    &output);
                // commit output to official state
                object_run->buffer_buffers = object_run->buffer_buffers_tmp;
                object_run->buffer_buffer_sizes = object_run->buffer_buffer_sizes_tmp;
                object_run->buffer_buffer_versions = object_run->buffer_buffer_versions_tmp;
            }
        }
        config_unmap_params(config, scene, mapped_object_list->objects[object_idx], PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_pipeline_object_mapped_t));
    }

    pnanovdb_compute_interface_t* compute_interface = compute->device_interface.get_compute_interface(queue);
    pnanovdb_compute_context_t* context = compute->device_interface.get_compute_context(queue);
    pnanovdb_compute_texture_t* tmp_color = nullptr;
    {
        pnanovdb_compute_resource_t resources[1u] = {};
        resources[0u].texture_transient = compute_interface->register_texture_as_transient(context, target_in->color);

        compute->dispatch_shader(compute_interface, context, ptr->shader_ctx[clear_color_slang], resources,
                                 (target_in->width + 7u) / 8u, (target_in->height + 7u) / 8u, 1u, "clear_color");

        pnanovdb_compute_texture_desc_t tex_desc = {};
        tex_desc.texture_type = PNANOVDB_COMPUTE_TEXTURE_TYPE_2D;
        tex_desc.usage = PNANOVDB_COMPUTE_TEXTURE_USAGE_TEXTURE | PNANOVDB_COMPUTE_TEXTURE_USAGE_RW_TEXTURE;
        tex_desc.format = PNANOVDB_COMPUTE_FORMAT_R8G8B8A8_UNORM;
        tex_desc.width = target_in->width;
        tex_desc.height = target_in->height;
        tex_desc.depth = 1u;
        tex_desc.mip_levels = 1u;
        tmp_color = compute_interface->create_texture(context, &tex_desc);
    }

    bool did_update_color = false;
    for (pnanovdb_uint64_t object_idx = 0u; object_idx < mapped_object_list->object_count; object_idx++)
    {
        pnanovdb_pipeline_object_mapped_t* mapped_object = nullptr;
        if(!config_map_params(config, scene, mapped_object_list->objects[object_idx], PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_pipeline_object_mapped_t), (void**)&mapped_object))
        {
            continue;
        }
        RuntimeObject* object_run = ptr->objects[object_idx].get();
        pnanovdb_uint64_t pipeline_count = mapped_object->stages[pnanovdb_pipeline_stage_render].pipeline_count;
        for (pnanovdb_uint64_t pipeline_idx = 0u; pipeline_idx < pipeline_count; pipeline_idx++)
        {
            pnanovdb_pipeline_object_mapped_stage_t* stage = &mapped_object->stages[pnanovdb_pipeline_stage_render];
            pnanovdb_uint64_t pipeline_first_index = object_run->pipeline_first_indices[pnanovdb_pipeline_stage_render];
            RuntimePipeline* pipeline_run = ptr->pipelines[pipeline_first_index + pipeline_idx].get();
            auto& pipeline_config = stage->pipelines[pipeline_idx];
            if (pipeline_run->pipeline_desc.render && mapped_object->is_visible && *mapped_object->is_visible)
            {
                {
                    pnanovdb_compute_resource_t resources[1u] = {};
                    resources[0u].texture_transient = compute_interface->register_texture_as_transient(context, tmp_color);

                    compute->dispatch_shader(compute_interface, context, ptr->shader_ctx[clear_color_slang], resources,
                                            (target_in->width + 7u) / 8u, (target_in->height + 7u) / 8u, 1u, "clear_color");
                }

                pnanovdb_pipeline_input_t input = {};
                input.buffer_count = object_run->buffer_buffers.size();
                input.buffer_buffers = object_run->buffer_buffers.data();
                input.buffer_sizes = object_run->buffer_buffer_sizes.data();
                input.buffer_versions = object_run->buffer_buffer_versions.data();
                input.buffer_names = object_run->buffer_names.data();
                input.params_data_type = pipeline_config.params_data_type;
                input.params_data = pipeline_config.params_data;
                pnanovdb_pipeline_render_target_t target = {};
                target.color = tmp_color;
                target.depth = target_in->depth;
                target.width = target_in->width;
                target.height = target_in->height;
                target.view = view;
                target.projection = projection;
                target.view_inv = view_inv;
                target.projection_inv = projection_inv;
                pipeline_run->pipeline_desc.render(
                    pipeline_run->instance,
                    compute,
                    queue,
                    &input,
                    &target);

                {
                    pnanovdb_compute_resource_t resources[2u] = {};
                    resources[0u].texture_transient = compute_interface->register_texture_as_transient(context, tmp_color);
                    resources[1u].texture_transient = compute_interface->register_texture_as_transient(context, target_in->color);

                    compute->dispatch_shader(compute_interface, context, ptr->shader_ctx[composite_color_slang], resources,
                                            (target_in->width + 7u) / 8u, (target_in->height + 7u) / 8u, 1u, "composite_color");
                }

                did_update_color = true;
            }
        }
        config_unmap_params(config, scene, mapped_object_list->objects[object_idx], PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_pipeline_object_mapped_t));
    }
    config_unmap_params(config, scene, nullptr, PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_pipeline_object_list_t));

    if (!did_update_color)
    {
        pnanovdb_compute_resource_t resources[1u] = {};
        resources[0u].texture_transient = compute_interface->register_texture_as_transient(context, target_in->color);

        compute->dispatch_shader(compute_interface, context, ptr->shader_ctx[checkerboard_color_slang], resources,
                                 (target_in->width + 7u) / 8u, (target_in->height + 7u) / 8u, 1u, "checkerboard_color");
    }

    compute_interface->destroy_texture(context, tmp_color);
}

void update_runtime_background(const pnanovdb_compute_t* compute,
                               pnanovdb_compute_queue_t* queue,
                               pnanovdb_pipeline_runtime_t* runtime,
                               pnanovdb_pipeline_config_t* config)
{
    auto ptr = cast(runtime);
}

}

pnanovdb_pipeline_t* pnanovdb_get_pipeline()
{
    static pnanovdb_pipeline_t iface = { PNANOVDB_REFLECT_INTERFACE_INIT(pnanovdb_pipeline_t) };

    iface.create_registry = create_registry;
    iface.destroy_registry = destroy_registry;
    iface.register_pipeline = register_pipeline;
    iface.get_pipeline_type = get_pipeline_type;
    iface.get_pipeline_metadesc = get_pipeline_metadesc;
    iface.create_config = create_config;
    iface.destroy_config = destroy_config;
    iface.config_add = config_add;
    iface.config_duplicate = config_duplicate;
    iface.config_rename = config_rename;
    iface.config_remove = config_remove;
    iface.config_set_pipeline = config_set_pipeline;
    iface.config_mark_pipeline_dirty = config_mark_pipeline_dirty;
    iface.config_set_buffer_array = config_set_buffer_array;
    iface.config_set_buffer_path = config_set_buffer_path;
    iface.config_set_camera = config_set_camera;
    iface.config_set_camera_view = config_set_camera_view;
    iface.config_map_params = config_map_params;
    iface.config_unmap_params = config_unmap_params;
    iface.config_set_active_camera = config_set_active_camera;
    iface.config_get_active_camera = config_get_active_camera;
    iface.config_set_active_scene = config_set_active_scene;
    iface.config_get_active_scene = config_get_active_scene;
    iface.create_runtime = create_runtime;
    iface.destroy_runtime = destroy_runtime;
    iface.update_runtime_main = update_runtime_main;
    iface.update_runtime_background = update_runtime_background;

    return &iface;
}
