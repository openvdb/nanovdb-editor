// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/editor/EditorSceneManager.cpp

    \author Petra Hapalova

    \brief  Implementation of scene management system for tracking multiple objects.
*/

#include "EditorSceneManager.h"
#include "Console.h"
#include "EditorToken.h"
#include "Pipeline.h"
#include "SceneView.h"
#include "raster/Raster.h"

#include <cstring>
#include <initializer_list>
#include <limits>
#include <set>
#include <string>

#ifdef _DEBUG
#    define SCENEMANAGER_LOG(...) printf(__VA_ARGS__)
#else
#    define SCENEMANAGER_LOG(...) (void)0
#endif

namespace pnanovdb_editor
{

namespace
{
std::shared_ptr<pnanovdb_compute_array_t> make_compute_array_owner(pnanovdb_compute_array_t* array,
                                                                   const pnanovdb_compute_t* compute,
                                                                   std::shared_ptr<pnanovdb_compute_array_t> existing_owner,
                                                                   const char* log_name);
} // namespace

bool ensure_scene_with_registered_cameras(EditorSceneManager& scene_manager,
                                          SceneView& scene_view,
                                          pnanovdb_editor_token_t* scene_token,
                                          bool create_default_camera)
{
    if (!scene_token)
    {
        return false;
    }

    if (!scene_view.get_scene_data(scene_token) && create_default_camera)
    {
        pnanovdb_editor_token_t* camera_token = EditorToken::getInstance().getToken("Camera");
        bool render_name_conflict = false;
        scene_manager.with_object(scene_token, camera_token,
                                  [&](SceneObject* obj)
                                  { render_name_conflict = obj && obj->type != SceneObjectType::Camera; });
        if (render_name_conflict)
        {
            Console::getInstance().addLog(
                Console::LogLevel::Error,
                "Cannot create scene '%s': its default camera name is already used by a render object",
                scene_token->str ? scene_token->str : "?");
            return false;
        }
    }

    SceneViewData* scene = scene_view.get_or_create_scene(scene_token, create_default_camera);
    if (!scene)
    {
        return false;
    }

    for (const auto& [camera_id, context] : scene->cameras)
    {
        if (!context.camera_view)
        {
            continue;
        }
        pnanovdb_editor_token_t* camera_token = EditorToken::getInstance().getTokenById(camera_id);
        if (!camera_token)
        {
            continue;
        }

        bool already_registered = false;
        bool render_name_conflict = false;
        std::shared_ptr<pnanovdb_camera_view_t> registered_owner;
        scene_manager.with_object(scene_token, camera_token,
                                  [&](SceneObject* obj)
                                  {
                                      if (obj && obj->type == SceneObjectType::Camera)
                                      {
                                          registered_owner = obj->resources.camera_view_owner;
                                          already_registered = static_cast<bool>(registered_owner);
                                      }
                                      render_name_conflict = obj && obj->type != SceneObjectType::Camera;
                                  });
        if (render_name_conflict)
        {
            Console::getInstance().addLog(Console::LogLevel::Error,
                                          "Cannot register camera '%s': that name is already used by a render object",
                                          camera_token->str ? camera_token->str : "?");
            return false;
        }
        if (!already_registered && !scene_manager.register_camera(scene_token, camera_token, context.camera_view))
        {
            return false;
        }
        if (registered_owner && registered_owner.get() != context.camera_view.get())
        {
            scene_view.sync_camera_owner(scene_token, camera_token, registered_owner);
        }
    }
    return true;
}

bool normalize_scene_viewport_camera(EditorSceneManager& scene_manager,
                                     SceneView& scene_view,
                                     pnanovdb_editor_token_t* scene_token)
{
    if (!scene_token || !ensure_scene_with_registered_cameras(scene_manager, scene_view, scene_token, false))
    {
        return false;
    }

    SceneViewData* scene = scene_view.get_or_create_scene(scene_token, false);
    if (!scene)
    {
        return false;
    }

    const auto is_usable = [](const CameraViewContext& context)
    {
        return context.camera_view && context.camera_view->num_cameras > 0 && context.camera_view->configs &&
               context.camera_view->states;
    };

    auto viewport = scene->cameras.find(scene->viewport_camera_token_id);
    if (viewport != scene->cameras.end() && is_usable(viewport->second))
    {
        return true;
    }

    for (const auto& [camera_id, context] : scene->cameras)
    {
        if (!is_usable(context))
        {
            continue;
        }
        pnanovdb_editor_token_t* camera_token = EditorToken::getInstance().getTokenById(camera_id);
        if (camera_token)
        {
            scene_view.set_viewport_camera(scene_token, camera_token);
            return true;
        }
    }

    static constexpr unsigned int k_max_suffix_attempts = 10000u;
    for (unsigned int suffix = 0; suffix < k_max_suffix_attempts; ++suffix)
    {
        const std::string name = suffix == 0 ? "Camera" : "Camera " + std::to_string(suffix);
        pnanovdb_editor_token_t* camera_token = EditorToken::getInstance().getToken(name.c_str());
        if (!camera_token || scene->cameras.count(camera_token->id) != 0)
        {
            continue;
        }

        bool occupied = false;
        scene_manager.with_object(scene_token, camera_token, [&](SceneObject* obj) { occupied = obj != nullptr; });
        if (occupied)
        {
            continue;
        }

        camera_token = scene_view.add_new_camera(scene_token, name.c_str());
        auto added = camera_token ? scene->cameras.find(camera_token->id) : scene->cameras.end();
        if (!camera_token || added == scene->cameras.end() || !is_usable(added->second) ||
            !scene_manager.register_camera(scene_token, camera_token, added->second.camera_view))
        {
            if (camera_token)
            {
                scene_view.remove(scene_token, camera_token);
            }
            continue;
        }

        scene_view.set_viewport_camera(scene_token, camera_token);
        added->second.camera_view->is_visible = PNANOVDB_FALSE;
        added->second.camera_view->configs[0].far_plane = std::numeric_limits<float>::infinity();
        return true;
    }
    return false;
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
        if (obj.type == SceneObjectType::NanoVDB && tokens_equal(obj.shader_name(), shader_name_token))
        {
            // Destroy old params array owner first (if different)
            if (obj.params.shader_params_array_owner)
            {
                obj.params.shader_params_array_owner.reset();
            }

            // Recreate from JSON defaults for this shader
            pnanovdb_compute_array_t* params_array = shader_params.get_compute_array_for_shader(shader_name, compute);
            if (!params_array)
            {
                params_array = create_params_array(compute, nullptr, PNANOVDB_COMPUTE_CONSTANT_BUFFER_MAX_SIZE);
            }

            obj.params.shader_params_array = params_array;
            obj.shader_params() = params_array ? params_array->data : nullptr;
            obj.params.shader_params_array_owner = make_compute_array_owner(params_array, compute, {}, "params array");
        }
    }
}

bool EditorSceneManager::reset_shader_params_to_defaults(const pnanovdb_compute_t* compute, const char* shader_name)
{
    if (!shader_name || !*shader_name)
    {
        return false;
    }
    if (!shader_params.resetToDefaults(shader_name))
    {
        return false;
    }
    if (compute)
    {
        refresh_params_for_shader(compute, shader_name);
    }
    return true;
}

bool EditorSceneManager::reset_group_params_to_defaults(const pnanovdb_compute_t* compute, const char* group_file_path)
{
    if (!group_file_path || !*group_file_path)
    {
        return false;
    }
    if (!shader_params.loadGroup(group_file_path, false))
    {
        return false;
    }
    if (!shader_params.resetGroupToDefaults(group_file_path))
    {
        return false;
    }
    if (compute)
    {
        std::set<std::string> seen;
        shader_params.forEachGroupShader(
            [&](const std::string& shader_name)
            {
                if (seen.insert(shader_name).second)
                {
                    refresh_params_for_shader(compute, shader_name.c_str());
                }
            });
    }
    return true;
}

size_t capture_shader_default_params(EditorSceneManager& scene_manager,
                                     const pnanovdb_compute_t* compute,
                                     const char* shader_name,
                                     size_t buf_size,
                                     void* out_buf)
{
    if (!compute || !shader_name || !out_buf || buf_size == 0)
    {
        return 0u;
    }
    pnanovdb_compute_array_t* arr = scene_manager.create_initialized_shader_params(
        compute, shader_name, nullptr, PNANOVDB_COMPUTE_CONSTANT_BUFFER_MAX_SIZE);
    if (!arr)
    {
        return 0u;
    }
    const size_t arr_bytes = arr->element_size * arr->element_count;
    const size_t copy_size = (arr_bytes < buf_size) ? arr_bytes : buf_size;
    if (arr->data && copy_size > 0)
    {
        std::memcpy(out_buf, arr->data, copy_size);
    }
    compute->destroy_array(arr);
    return copy_size;
}

bool EditorSceneManager::refresh_params_for_object(const pnanovdb_compute_t* compute,
                                                   pnanovdb_editor_token_t* scene,
                                                   pnanovdb_editor_token_t* name)
{
    if (!compute || !scene || !name)
    {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_objects.find(make_key(scene, name));
    if (it == m_objects.end())
    {
        return false;
    }
    return refresh_params_for_object(compute, it->second);
}

bool EditorSceneManager::refresh_params_for_object(const pnanovdb_compute_t* compute, SceneObject& obj)
{
    if (!compute)
    {
        return false;
    }

    if (obj.type != SceneObjectType::NanoVDB)
    {
        return false;
    }

    const char* shader_name = token_to_string(obj.shader_name());
    if (!shader_name || !*shader_name)
    {
        return false;
    }

    if (obj.params.shader_params_array_owner)
    {
        obj.params.shader_params_array_owner.reset();
    }

    pnanovdb_compute_array_t* params_array = shader_params.get_compute_array_for_shader(shader_name, compute);
    if (!params_array)
    {
        params_array = create_params_array(compute, nullptr, PNANOVDB_COMPUTE_CONSTANT_BUFFER_MAX_SIZE);
    }

    obj.params.shader_params_array = params_array;
    obj.shader_params() = params_array ? params_array->data : nullptr;
    obj.params.shader_params_array_owner = make_compute_array_owner(params_array, compute, {}, "params array");
    return true;
}

bool EditorSceneManager::add_nanovdb(pnanovdb_editor_token_t* scene,
                                     pnanovdb_editor_token_t* name,
                                     pnanovdb_compute_array_t* array,
                                     pnanovdb_compute_array_t* params_array,
                                     const pnanovdb_compute_t* compute,
                                     pnanovdb_editor_token_t* shader_name,
                                     std::shared_ptr<pnanovdb_raster_gaussian_data_t>* old_gaussian_owner_out)
{
    // Use default pipelines for NanoVDB
    return add_with_lock(scene, name, nullptr,
                         [&]
                         {
                             return add_nanovdb_impl(scene, name, array, params_array, compute, shader_name,
                                                     pnanovdb_pipeline_type_noop, pnanovdb_pipeline_type_nanovdb_render,
                                                     old_gaussian_owner_out);
                         });
}

bool EditorSceneManager::add_nanovdb(pnanovdb_editor_token_t* scene,
                                     pnanovdb_editor_token_t* name,
                                     pnanovdb_compute_array_t* array,
                                     pnanovdb_compute_array_t* params_array,
                                     const pnanovdb_compute_t* compute,
                                     pnanovdb_editor_token_t* shader_name,
                                     pnanovdb_pipeline_type_t process_pipeline,
                                     pnanovdb_pipeline_type_t render_pipeline,
                                     std::shared_ptr<pnanovdb_raster_gaussian_data_t>* old_gaussian_owner_out)
{
    // Atomic: acquire mutex once for both object creation and pipeline config
    return add_with_lock(scene, name, nullptr,
                         [&]
                         {
                             return add_nanovdb_impl(scene, name, array, params_array, compute, shader_name,
                                                     process_pipeline, render_pipeline, old_gaussian_owner_out);
                         });
}

bool EditorSceneManager::load_reservation_matches(uint64_t key, uint64_t lifetime_id) const
{
    auto it = m_objects.find(key);
    return it != m_objects.end() && it->second.lifetime_id == lifetime_id;
}

bool EditorSceneManager::render_insertion_allowed(uint64_t key, pnanovdb_editor_token_t* name) const
{
    auto it = m_objects.find(key);
    if (it == m_objects.end() || it->second.type != SceneObjectType::Camera)
        return true;
    Console::getInstance().addLog(Console::LogLevel::Error,
                                  "Cannot add render object '%s': that name is already used by a camera",
                                  name && name->str ? name->str : "?");
    return false;
}

bool EditorSceneManager::reserve_load_target(pnanovdb_editor_token_t* scene,
                                             pnanovdb_editor_token_t* name,
                                             uint64_t* lifetime_id,
                                             bool replace_existing,
                                             bool* replacing)
{
    if (!scene || !name || !lifetime_id)
        return false;

    std::lock_guard<std::mutex> lock(m_mutex);
    const uint64_t key = make_key(scene, name);
    auto existing = m_objects.find(key);
    if (existing != m_objects.end())
    {
        if (!replace_existing || existing->second.type == SceneObjectType::Camera ||
            existing->second.type == SceneObjectType::Uninitialized)
            return false;
        *lifetime_id = existing->second.lifetime_id;
        if (replacing)
            *replacing = true;
        return *lifetime_id != 0;
    }

    SceneObject& obj = reset_object_locked(key, scene, name, SceneObjectType::Uninitialized);
    *lifetime_id = obj.lifetime_id;
    if (replacing)
        *replacing = false;
    return true;
}

void EditorSceneManager::cancel_load_target(pnanovdb_editor_token_t* scene,
                                            pnanovdb_editor_token_t* name,
                                            uint64_t lifetime_id)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const uint64_t key = make_key(scene, name);
    auto it = m_objects.find(key);
    if (it != m_objects.end() && it->second.type == SceneObjectType::Uninitialized &&
        it->second.lifetime_id == lifetime_id)
    {
        forget_object_lifetime(it->second);
        m_objects.erase(key);
    }
}

bool EditorSceneManager::commit_reserved_nanovdb(pnanovdb_editor_token_t* scene,
                                                 pnanovdb_editor_token_t* name,
                                                 uint64_t lifetime_id,
                                                 pnanovdb_compute_array_t* array,
                                                 pnanovdb_compute_array_t* params_array,
                                                 const pnanovdb_compute_t* compute,
                                                 pnanovdb_editor_token_t* shader_name,
                                                 pnanovdb_pipeline_type_t process_pipeline,
                                                 pnanovdb_pipeline_type_t render_pipeline,
                                                 std::shared_ptr<pnanovdb_raster_gaussian_data_t>* old_gaussian_owner_out)
{
    return add_with_lock(scene, name, &lifetime_id,
                         [&]
                         {
                             return add_nanovdb_impl(scene, name, array, params_array, compute, shader_name,
                                                     process_pipeline, render_pipeline, old_gaussian_owner_out);
                         });
}

namespace
{
void apply_default_stage(PipelineStage& slot, pnanovdb_pipeline_type_t type)
{
    if (slot.configured)
    {
        return;
    }
    slot.type = type;
    pnanovdb_pipeline_get_default_params(type, &slot.params);
    slot.bump_revision();
}

std::shared_ptr<pnanovdb_compute_array_t> find_array_owner(const std::map<uint64_t, SceneObject>& objects,
                                                           pnanovdb_compute_array_t* array)
{
    if (!array)
    {
        return {};
    }
    for (const auto& entry : objects)
    {
        const SceneObject& obj = entry.second;
        const auto matches = [array](const std::shared_ptr<pnanovdb_compute_array_t>& owner)
        { return owner && owner.get() == array; };
        if (matches(obj.resources.nanovdb_array_owner))
            return obj.resources.nanovdb_array_owner;
        if (matches(obj.resources.converted_nanovdb_owner))
            return obj.resources.converted_nanovdb_owner;
        if (matches(obj.params.shader_params_array_owner))
            return obj.params.shader_params_array_owner;
        for (const auto& named : obj.resources.named_array_owners)
        {
            if (matches(named.second))
                return named.second;
        }
        for (size_t stage_index = 0; stage_index < pnanovdb_pipeline_stage_count; ++stage_index)
        {
            for (const auto& output : obj.pipeline.stages[stage_index].output.array_owners)
            {
                if (matches(output.second))
                    return output.second;
            }
        }
        for (const PipelineStage& stage : obj.pipeline.extra_process)
        {
            for (const auto& output : stage.output.array_owners)
            {
                if (matches(output.second))
                    return output.second;
            }
        }
    }
    return {};
}

std::shared_ptr<pnanovdb_compute_array_t> find_array_owner(const std::map<uint64_t, SceneObject>& objects,
                                                           const std::map<uint64_t, SceneObject>& retained_objects,
                                                           pnanovdb_compute_array_t* array)
{
    std::shared_ptr<pnanovdb_compute_array_t> owner = find_array_owner(objects, array);
    return owner ? owner : find_array_owner(retained_objects, array);
}

std::shared_ptr<pnanovdb_raster_gaussian_data_t> find_gaussian_owner(const std::map<uint64_t, SceneObject>& objects,
                                                                     pnanovdb_raster_gaussian_data_t* gaussian)
{
    if (!gaussian)
    {
        return {};
    }
    for (const auto& entry : objects)
    {
        const SceneObject& obj = entry.second;
        if (obj.resources.gaussian_data_owner && obj.resources.gaussian_data_owner.get() == gaussian)
        {
            return obj.resources.gaussian_data_owner;
        }
        for (size_t stage_index = 0; stage_index < pnanovdb_pipeline_stage_count; ++stage_index)
        {
            const StageOutput& output = obj.pipeline.stages[stage_index].output;
            if (output.gaussian_owner && output.gaussian_owner.get() == gaussian)
            {
                return output.gaussian_owner;
            }
        }
        for (const PipelineStage& stage : obj.pipeline.extra_process)
        {
            if (stage.output.gaussian_owner && stage.output.gaussian_owner.get() == gaussian)
            {
                return stage.output.gaussian_owner;
            }
        }
    }
    return {};
}

std::shared_ptr<pnanovdb_raster_gaussian_data_t> find_gaussian_owner(const std::map<uint64_t, SceneObject>& objects,
                                                                     const std::map<uint64_t, SceneObject>& retained_objects,
                                                                     pnanovdb_raster_gaussian_data_t* gaussian)
{
    std::shared_ptr<pnanovdb_raster_gaussian_data_t> owner = find_gaussian_owner(objects, gaussian);
    return owner ? owner : find_gaussian_owner(retained_objects, gaussian);
}

std::shared_ptr<pnanovdb_compute_array_t> make_compute_array_owner(pnanovdb_compute_array_t* array,
                                                                   const pnanovdb_compute_t* compute,
                                                                   std::shared_ptr<pnanovdb_compute_array_t> existing_owner,
                                                                   const char* log_name)
{
    if (existing_owner || !array || !compute || !compute->destroy_array)
    {
        return existing_owner;
    }
    return std::shared_ptr<pnanovdb_compute_array_t>(
        array,
        [destroy_fn = compute->destroy_array, log_name](pnanovdb_compute_array_t* ptr)
        {
            if (ptr)
            {
                SCENEMANAGER_LOG("[SceneManager] Destroying %s %p (data=%p)\n", log_name ? log_name : "array",
                                 (void*)ptr, (void*)ptr->data);
                destroy_fn(ptr);
            }
        });
}

std::shared_ptr<pnanovdb_raster_gaussian_data_t> make_gaussian_owner(
    pnanovdb_raster_gaussian_data_t* gaussian_data,
    const pnanovdb_compute_t* compute,
    const pnanovdb_raster_t* raster,
    pnanovdb_compute_queue_t* queue,
    std::shared_ptr<pnanovdb_raster_gaussian_data_t> existing_owner)
{
    if (existing_owner || !gaussian_data || !raster || !raster->destroy_gaussian_data || !compute || !queue)
    {
        return existing_owner;
    }
    return std::shared_ptr<pnanovdb_raster_gaussian_data_t>(
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
}

std::shared_ptr<pnanovdb_compute_array_t> reuse_or_make_compute_array_owner(
    pnanovdb_compute_array_t* array,
    std::shared_ptr<pnanovdb_compute_array_t> owner,
    const pnanovdb_compute_t* compute,
    std::initializer_list<std::pair<pnanovdb_compute_array_t*, std::shared_ptr<pnanovdb_compute_array_t>>> aliases,
    const char* log_name)
{
    if (owner || !array)
    {
        return owner;
    }
    for (const auto& [alias_array, alias_owner] : aliases)
    {
        if (array == alias_array && alias_owner)
        {
            return alias_owner;
        }
    }
    return make_compute_array_owner(array, compute, {}, log_name);
}

void bind_named_array(SceneObject& obj,
                      const char* name,
                      pnanovdb_compute_array_t* array,
                      const std::shared_ptr<pnanovdb_compute_array_t>& owner)
{
    if (!array)
    {
        return;
    }
    obj.named_arrays()[name] = array;
    if (owner)
    {
        obj.resources.named_array_owners[name] = owner;
    }
}

void bind_gaussian_cpu_arrays(SceneObject& obj, pnanovdb_raster_gaussian_data_t* gaussian_data)
{
    obj.named_arrays().clear();
    obj.resources.named_array_owners.clear();
    if (!gaussian_data)
    {
        return;
    }

    auto* gd = pnanovdb_raster::cast(gaussian_data);
    if (!gd)
    {
        return;
    }

    const auto& owner = obj.resources.gaussian_data_owner;
    auto alias_owner = [&owner](pnanovdb_compute_array_t* array) -> std::shared_ptr<pnanovdb_compute_array_t>
    { return owner && array ? std::shared_ptr<pnanovdb_compute_array_t>(owner, array) : nullptr; };

    for (const auto& binding : pnanovdb_raster::s_gaussian_cpu_array_bindings)
    {
        pnanovdb_compute_array_t* array = gd->*binding.member;
        bind_named_array(obj, binding.name, array, alias_owner(array));
    }
    obj.resources.file_backed_named_arrays = obj.resources.named_arrays;
}

void preserve_replaced_gaussian_owner(const SceneObject& obj,
                                      std::shared_ptr<pnanovdb_raster_gaussian_data_t>* old_gaussian_owner_out)
{
    if (old_gaussian_owner_out && obj.resources.gaussian_data_owner)
        *old_gaussian_owner_out = obj.resources.gaussian_data_owner;
}

void preserve_replaced_gaussian_owner(const std::map<uint64_t, SceneObject>& objects,
                                      uint64_t key,
                                      std::shared_ptr<pnanovdb_raster_gaussian_data_t>* old_gaussian_owner_out)
{
    auto it = objects.find(key);
    if (it != objects.end())
    {
        preserve_replaced_gaussian_owner(it->second, old_gaussian_owner_out);
    }
}

struct ReplacementState
{
    bool existed = false;
    bool visible = true;
};

ReplacementState capture_replacement_state(const std::map<uint64_t, SceneObject>& objects,
                                           uint64_t key,
                                           std::shared_ptr<pnanovdb_raster_gaussian_data_t>* old_gaussian_owner_out)
{
    auto it = objects.find(key);
    if (it == objects.end())
    {
        return {};
    }

    preserve_replaced_gaussian_owner(it->second, old_gaussian_owner_out);
    return { true, it->second.visible };
}

void restore_replacement_state(SceneObject& obj, const ReplacementState& state)
{
    if (state.existed)
    {
        obj.visible = state.visible;
    }
}

void release_unowned_arrays(
    const pnanovdb_compute_t* compute,
    std::initializer_list<std::pair<pnanovdb_compute_array_t*, std::shared_ptr<pnanovdb_compute_array_t>>> arrays)
{
    if (!compute || !compute->destroy_array)
    {
        return;
    }

    std::set<pnanovdb_compute_array_t*> released;
    for (const auto& [array, owner] : arrays)
    {
        if (array && !owner && released.insert(array).second)
        {
            compute->destroy_array(array);
        }
    }
}
} // namespace

bool EditorSceneManager::add_nanovdb_impl(pnanovdb_editor_token_t* scene,
                                          pnanovdb_editor_token_t* name,
                                          pnanovdb_compute_array_t* array,
                                          pnanovdb_compute_array_t* params_array,
                                          const pnanovdb_compute_t* compute,
                                          pnanovdb_editor_token_t* shader_name,
                                          pnanovdb_pipeline_type_t process_pipeline,
                                          pnanovdb_pipeline_type_t render_pipeline,
                                          std::shared_ptr<pnanovdb_raster_gaussian_data_t>* old_gaussian_owner_out)
{
    // NOTE: Caller must hold m_mutex
    uint64_t key = make_key(scene, name);

    std::shared_ptr<pnanovdb_compute_array_t> array_owner =
        find_array_owner(m_objects, m_file_replacement_backups, array);
    std::shared_ptr<pnanovdb_compute_array_t> params_owner =
        find_array_owner(m_objects, m_file_replacement_backups, params_array);
    if (!render_insertion_allowed(key, name))
    {
        release_unowned_arrays(compute, { { array, array_owner }, { params_array, params_owner } });
        return false;
    }

    const ReplacementState replacement = capture_replacement_state(m_objects, key, old_gaussian_owner_out);

    SceneObject& obj = reset_object_locked(key, scene, name, SceneObjectType::NanoVDB);
    obj.nanovdb_array() = array;
    obj.resources.nanovdb_array_owner = make_compute_array_owner(array, compute, std::move(array_owner), "nanovdb array");
    obj.params.shader_params_array = params_array;
    obj.params.shader_params_array_owner =
        make_compute_array_owner(params_array, compute, std::move(params_owner), "params array");
    obj.shader_params() = params_array ? params_array->data : nullptr;
    obj.shader_params_data_type() = nullptr;
    obj.shader_name() = shader_name;

    apply_default_stage(obj.pipeline.process(), process_pipeline);
    apply_default_stage(obj.pipeline.render(), render_pipeline);
    obj.mark_process_dirty();
    restore_replacement_state(obj, replacement);

    obj.pipeline.load().output.set_array(k_stage_output_nanovdb, array, obj.resources.nanovdb_array_owner);
    for (size_t i = 0; i < obj.pipeline.process_count(); ++i)
    {
        obj.pipeline.process_step(i).output.clear();
    }

    return true;
}

bool EditorSceneManager::add_gaussian_data(pnanovdb_editor_token_t* scene,
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
    // Use default pipelines for Gaussian data: gaussian_splat (no process, 2D splatting)
    return add_with_lock(scene, name, nullptr,
                         [&]
                         {
                             return add_gaussian_data_impl(scene, name, gaussian_data, params_array,
                                                           shader_params_data_type, compute, raster, queue, shader_name,
                                                           pnanovdb_pipeline_type_noop,
                                                           pnanovdb_pipeline_type_gaussian_splat, old_owner_out);
                         });
}

bool EditorSceneManager::add_gaussian_data(pnanovdb_editor_token_t* scene,
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
                                           std::shared_ptr<pnanovdb_raster_gaussian_data_t>* old_owner_out)
{
    // Atomic: acquire mutex once for both object creation and pipeline config
    return add_with_lock(scene, name, nullptr,
                         [&]
                         {
                             return add_gaussian_data_impl(scene, name, gaussian_data, params_array,
                                                           shader_params_data_type, compute, raster, queue, shader_name,
                                                           process_pipeline, render_pipeline, old_owner_out);
                         });
}

bool EditorSceneManager::commit_reserved_gaussian_data(pnanovdb_editor_token_t* scene,
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
                                                       std::shared_ptr<pnanovdb_raster_gaussian_data_t>* old_owner_out)
{
    return add_with_lock(scene, name, &lifetime_id,
                         [&]
                         {
                             return add_gaussian_data_impl(scene, name, gaussian_data, params_array,
                                                           shader_params_data_type, compute, raster, queue, shader_name,
                                                           process_pipeline, render_pipeline, old_owner_out);
                         });
}

bool EditorSceneManager::add_gaussian_data_impl(pnanovdb_editor_token_t* scene,
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
                                                std::shared_ptr<pnanovdb_raster_gaussian_data_t>* old_owner_out)
{
    // NOTE: Caller must hold m_mutex
    uint64_t key = make_key(scene, name);

    std::shared_ptr<pnanovdb_raster_gaussian_data_t> gaussian_owner =
        find_gaussian_owner(m_objects, m_file_replacement_backups, gaussian_data);
    std::shared_ptr<pnanovdb_compute_array_t> params_owner =
        find_array_owner(m_objects, m_file_replacement_backups, params_array);
    if (!render_insertion_allowed(key, name))
    {
        if (gaussian_data && !gaussian_owner && raster && raster->destroy_gaussian_data && compute && queue)
            raster->destroy_gaussian_data(compute, queue, gaussian_data);
        release_unowned_arrays(compute, { { params_array, params_owner } });
        return false;
    }
    const ReplacementState replacement = capture_replacement_state(m_objects, key, old_owner_out);

    SceneObject& obj = reset_object_locked(key, scene, name, SceneObjectType::GaussianData);
    obj.gaussian_data() = gaussian_data;
    obj.resources.gaussian_data_owner =
        make_gaussian_owner(gaussian_data, compute, raster, queue, std::move(gaussian_owner));
    obj.params.shader_params_array = params_array;
    obj.params.shader_params_array_owner =
        make_compute_array_owner(params_array, compute, std::move(params_owner), "params array");
    obj.shader_params() = params_array ? params_array->data : nullptr;
    obj.shader_params_data_type() = shader_params_data_type;
    obj.shader_name() = EditorToken::getInstance().getToken(shader_name);

    apply_default_stage(obj.pipeline.process(), process_pipeline);
    apply_default_stage(obj.pipeline.render(), render_pipeline);
    obj.mark_process_dirty();
    restore_replacement_state(obj, replacement);

    bind_gaussian_cpu_arrays(obj, gaussian_data);

    obj.pipeline.load().output.gaussian = gaussian_data;
    obj.pipeline.load().output.gaussian_owner = obj.resources.gaussian_data_owner;
    return true;
}

bool EditorSceneManager::add_camera(pnanovdb_editor_token_t* scene,
                                    pnanovdb_editor_token_t* name,
                                    pnanovdb_camera_view_t* camera_view)
{
    if (!camera_view)
    {
        return false;
    }

    pnanovdb_camera_view_t* camera_copy = new pnanovdb_camera_view_t();

    // Copy all scalar fields
    camera_copy->name = camera_view->name;
    camera_copy->axis_length = camera_view->axis_length;
    camera_copy->axis_thickness = camera_view->axis_thickness;
    camera_copy->frustum_line_width = camera_view->frustum_line_width;
    camera_copy->frustum_scale = camera_view->frustum_scale;
    camera_copy->frustum_color = camera_view->frustum_color;
    camera_copy->is_visible = camera_view->is_visible;

    const bool has_complete_entries = camera_view->num_cameras > 0 && camera_view->states && camera_view->configs;
    camera_copy->num_cameras = has_complete_entries ? camera_view->num_cameras : 1u;
    camera_copy->states = new pnanovdb_camera_state_t[camera_copy->num_cameras];
    camera_copy->configs = new pnanovdb_camera_config_t[camera_copy->num_cameras];
    if (has_complete_entries)
    {
        std::memcpy(camera_copy->states, camera_view->states, sizeof(pnanovdb_camera_state_t) * camera_copy->num_cameras);
        std::memcpy(
            camera_copy->configs, camera_view->configs, sizeof(pnanovdb_camera_config_t) * camera_copy->num_cameras);
    }
    else
    {
        pnanovdb_camera_state_default(&camera_copy->states[0], PNANOVDB_TRUE);
        pnanovdb_camera_config_default(&camera_copy->configs[0]);
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    uint64_t key = make_key(scene, name);

    auto it = m_objects.find(key);
    SceneObject* existing = it != m_objects.end() ? &it->second : nullptr;
    if (existing && existing->type != SceneObjectType::Uninitialized && existing->type != SceneObjectType::Camera)
    {
        delete[] camera_copy->states;
        delete[] camera_copy->configs;
        delete camera_copy;
        Console::getInstance().addLog(Console::LogLevel::Error,
                                      "Cannot add camera '%s': that name is already used by a render object",
                                      name && name->str ? name->str : "?");
        return false;
    }
    SceneObject& obj = reset_object_locked(key, scene, name, SceneObjectType::Camera);

    obj.camera_view() = camera_copy;
    obj.resources.camera_view_owner = std::shared_ptr<pnanovdb_camera_view_t>(camera_copy,
                                                                              [](pnanovdb_camera_view_t* ptr)
                                                                              {
                                                                                  if (ptr)
                                                                                  {
                                                                                      delete[] ptr->states;
                                                                                      delete[] ptr->configs;
                                                                                      delete ptr;
                                                                                  }
                                                                              });
    return true;
}

namespace
{
void configure_array_object_pipelines(SceneObject& obj,
                                      uint64_t key,
                                      pnanovdb_editor_token_t* scene,
                                      pnanovdb_editor_token_t* name,
                                      const pnanovdb_compute_t* compute,
                                      pnanovdb_pipeline_type_t process_pipeline,
                                      pnanovdb_pipeline_type_t render_pipeline,
                                      EditorSceneManager& manager)
{
    obj.type = SceneObjectType::Array;
    obj.scene_token = scene;
    obj.name_token = name;

    obj.ensure_shader_name_storage().object_key = key;

    apply_default_stage(obj.pipeline.process(), process_pipeline);
    apply_default_stage(obj.pipeline.render(), render_pipeline);
    obj.mark_process_dirty();

    obj.params.shader_params_array = nullptr;
    obj.params.shader_params_array_owner.reset();
    obj.shader_params() = nullptr;
    obj.shader_params_data_type() = nullptr;

    const pnanovdb_pipeline_descriptor_t* render_desc = pnanovdb_pipeline_get_descriptor(obj.render_pipeline());
    const char* render_shader_name = (render_desc && render_desc->shader_count > 0u && render_desc->shaders) ?
                                         render_desc->shaders[0].shader_name :
                                         nullptr;
    if (render_shader_name)
    {
        obj.shader_name() = EditorToken::getInstance().getToken(render_shader_name);

        pnanovdb_compute_array_t* params_array = manager.create_initialized_shader_params(
            compute, render_shader_name, nullptr, PNANOVDB_COMPUTE_CONSTANT_BUFFER_MAX_SIZE);
        if (params_array)
        {
            obj.params.shader_params_array = params_array;
            obj.shader_params() = params_array->data;
            obj.params.shader_params_array_owner = make_compute_array_owner(params_array, compute, {}, "params array");
        }
    }
    else
    {
        obj.shader_name() = nullptr;
    }
}
} // namespace

bool EditorSceneManager::add_mesh(pnanovdb_editor_token_t* scene,
                                  pnanovdb_editor_token_t* name,
                                  pnanovdb_compute_array_t* indices,
                                  pnanovdb_compute_array_t* positions,
                                  pnanovdb_compute_array_t* colors,
                                  const pnanovdb_compute_t* compute,
                                  pnanovdb_pipeline_type_t process_pipeline,
                                  pnanovdb_pipeline_type_t render_pipeline,
                                  std::shared_ptr<pnanovdb_raster_gaussian_data_t>* old_gaussian_owner_out)
{
    if (!indices || !positions || !compute)
    {
        return false;
    }

    return add_with_lock(scene, name, nullptr,
                         [&]
                         {
                             return add_mesh_impl(scene, name, indices, positions, colors, compute, process_pipeline,
                                                  render_pipeline, old_gaussian_owner_out);
                         });
}

bool EditorSceneManager::commit_reserved_mesh(pnanovdb_editor_token_t* scene,
                                              pnanovdb_editor_token_t* name,
                                              uint64_t lifetime_id,
                                              pnanovdb_compute_array_t* indices,
                                              pnanovdb_compute_array_t* positions,
                                              pnanovdb_compute_array_t* colors,
                                              const pnanovdb_compute_t* compute,
                                              pnanovdb_pipeline_type_t process_pipeline,
                                              pnanovdb_pipeline_type_t render_pipeline,
                                              std::shared_ptr<pnanovdb_raster_gaussian_data_t>* old_gaussian_owner_out)
{
    if (!indices || !positions || !compute)
        return false;

    return add_with_lock(scene, name, &lifetime_id,
                         [&]
                         {
                             return add_mesh_impl(scene, name, indices, positions, colors, compute, process_pipeline,
                                                  render_pipeline, old_gaussian_owner_out);
                         });
}

bool EditorSceneManager::add_mesh_impl(pnanovdb_editor_token_t* scene,
                                       pnanovdb_editor_token_t* name,
                                       pnanovdb_compute_array_t* indices,
                                       pnanovdb_compute_array_t* positions,
                                       pnanovdb_compute_array_t* colors,
                                       const pnanovdb_compute_t* compute,
                                       pnanovdb_pipeline_type_t process_pipeline,
                                       pnanovdb_pipeline_type_t render_pipeline,
                                       std::shared_ptr<pnanovdb_raster_gaussian_data_t>* old_gaussian_owner_out)
{
    uint64_t key = make_key(scene, name);

    std::shared_ptr<pnanovdb_compute_array_t> indices_owner =
        find_array_owner(m_objects, m_file_replacement_backups, indices);
    std::shared_ptr<pnanovdb_compute_array_t> positions_owner =
        find_array_owner(m_objects, m_file_replacement_backups, positions);
    std::shared_ptr<pnanovdb_compute_array_t> colors_owner =
        find_array_owner(m_objects, m_file_replacement_backups, colors);
    if (!render_insertion_allowed(key, name))
    {
        release_unowned_arrays(
            compute, { { indices, indices_owner }, { positions, positions_owner }, { colors, colors_owner } });
        return false;
    }

    preserve_replaced_gaussian_owner(m_objects, key, old_gaussian_owner_out);
    SceneObject& obj = reset_object_locked(key, scene, name, SceneObjectType::Array);
    configure_array_object_pipelines(obj, key, scene, name, compute, process_pipeline, render_pipeline, *this);

    indices_owner = reuse_or_make_compute_array_owner(indices, indices_owner, compute, {}, "mesh indices");
    positions_owner = reuse_or_make_compute_array_owner(
        positions, positions_owner, compute, { { indices, indices_owner } }, "mesh positions");
    colors_owner = reuse_or_make_compute_array_owner(
        colors, colors_owner, compute, { { indices, indices_owner }, { positions, positions_owner } }, "mesh colors");

    obj.named_arrays().clear();
    obj.resources.named_array_owners.clear();
    bind_named_array(obj, "indices", indices, indices_owner);
    bind_named_array(obj, "positions", positions, positions_owner);
    bind_named_array(obj, "colors", colors, colors_owner);
    obj.resources.file_backed_named_arrays = obj.resources.named_arrays;
    return true;
}

bool EditorSceneManager::add_file_object(pnanovdb_editor_token_t* scene,
                                         pnanovdb_editor_token_t* name,
                                         const pnanovdb_compute_t* compute,
                                         pnanovdb_pipeline_type_t process_pipeline,
                                         pnanovdb_pipeline_type_t render_pipeline,
                                         bool replace_existing,
                                         std::shared_ptr<pnanovdb_raster_gaussian_data_t>* old_gaussian_owner_out)
{
    if (!compute)
    {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    uint64_t key = make_key(scene, name);

    if (!render_insertion_allowed(key, name))
        return false;

    auto existing = m_objects.find(key);
    if (existing != m_objects.end() && !replace_existing)
    {
        return false;
    }

    preserve_replaced_gaussian_owner(m_objects, key, old_gaussian_owner_out);
    SceneObject& obj = reset_object_locked(key, scene, name, SceneObjectType::Array);
    configure_array_object_pipelines(obj, key, scene, name, compute, process_pipeline, render_pipeline, *this);
    return true;
}

bool EditorSceneManager::stage_file_object_replacement(pnanovdb_editor_token_t* scene,
                                                       pnanovdb_editor_token_t* name,
                                                       uint64_t lifetime_id,
                                                       const pnanovdb_compute_t* compute,
                                                       pnanovdb_pipeline_type_t process_pipeline,
                                                       pnanovdb_pipeline_type_t render_pipeline)
{
    if (!scene || !name || !compute || lifetime_id == 0)
        return false;

    std::lock_guard<std::mutex> lock(m_mutex);
    const uint64_t key = make_key(scene, name);
    auto it = m_objects.find(key);
    if (it == m_objects.end() || it->second.lifetime_id != lifetime_id || it->second.type == SceneObjectType::Camera ||
        it->second.type == SceneObjectType::Uninitialized ||
        m_file_replacement_backups.find(lifetime_id) != m_file_replacement_backups.end())
    {
        return false;
    }

    const bool previous_visibility = it->second.visible;
    m_file_replacement_backups.emplace(lifetime_id, std::move(it->second));
    it->second = SceneObject{};
    it->second.lifetime_id = lifetime_id;
    configure_array_object_pipelines(it->second, key, scene, name, compute, process_pipeline, render_pipeline, *this);
    it->second.visible = previous_visibility;
    return true;
}

bool EditorSceneManager::finish_file_object_replacement(
    uint64_t lifetime_id, bool success, std::shared_ptr<pnanovdb_raster_gaussian_data_t>* old_gaussian_owner_out)
{
    if (lifetime_id == 0)
        return false;

    std::lock_guard<std::mutex> lock(m_mutex);
    auto backup_it = m_file_replacement_backups.find(lifetime_id);
    if (backup_it == m_file_replacement_backups.end())
        return false;

    auto object_it = m_objects.end();
    auto index_it = m_lifetime_to_key.find(lifetime_id);
    if (index_it != m_lifetime_to_key.end())
    {
        auto candidate = m_objects.find(index_it->second);
        if (candidate != m_objects.end() && candidate->second.lifetime_id == lifetime_id)
        {
            object_it = candidate;
        }
    }
    if (object_it == m_objects.end())
    {
        m_file_replacement_backups.erase(backup_it);
        return false;
    }

    if (success)
    {
        preserve_replaced_gaussian_owner(backup_it->second, old_gaussian_owner_out);
        begin_object_lifetime(object_it->second, object_it->first);
        m_file_replacement_backups.erase(backup_it);
        return true;
    }

    pnanovdb_editor_token_t* current_scene = object_it->second.scene_token;
    pnanovdb_editor_token_t* current_name = object_it->second.name_token;
    SceneObject restored = std::move(backup_it->second);
    restored.scene_token = current_scene;
    restored.name_token = current_name;
    restored.ensure_shader_name_storage().object_key = object_it->first;
    object_it->second = std::move(restored);
    m_file_replacement_backups.erase(backup_it);
    return true;
}

bool EditorSceneManager::has_file_object_replacement_in_progress() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return !m_file_replacement_backups.empty();
}

bool EditorSceneManager::register_camera(pnanovdb_editor_token_t* scene,
                                         pnanovdb_editor_token_t* name,
                                         std::shared_ptr<pnanovdb_camera_view_t> camera_view_owner)
{
    if (!camera_view_owner)
    {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    uint64_t key = make_key(scene, name);

    auto existing = m_objects.find(key);
    if (existing != m_objects.end() && existing->second.type != SceneObjectType::Uninitialized &&
        existing->second.type != SceneObjectType::Camera)
    {
        Console::getInstance().addLog(Console::LogLevel::Error,
                                      "Cannot register camera '%s': that name is "
                                      "already used by a render object",
                                      name && name->str ? name->str : "?");
        return false;
    }
    SceneObject& obj = reset_object_locked(key, scene, name, SceneObjectType::Camera);
    obj.camera_view() = camera_view_owner.get();
    obj.resources.camera_view_owner = camera_view_owner; // Share ownership, no copy
    return true;
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
        if (it->second.resources.nanovdb_array_owner)
            SCENEMANAGER_LOG("[SceneManager]   - nanovdb_array %p, ref_count %ld\n",
                             (void*)it->second.resources.nanovdb_array_owner.get(),
                             it->second.resources.nanovdb_array_owner.use_count());
        if (it->second.params.shader_params_array_owner)
            SCENEMANAGER_LOG("[SceneManager]   - params_array %p, ref_count %ld\n",
                             (void*)it->second.params.shader_params_array_owner.get(),
                             it->second.params.shader_params_array_owner.use_count());
        if (it->second.resources.gaussian_data_owner)
            SCENEMANAGER_LOG("[SceneManager]   - gaussian_data %p, ref_count %ld\n",
                             (void*)it->second.resources.gaussian_data_owner.get(),
                             it->second.resources.gaussian_data_owner.use_count());
    }

    if (it != m_objects.end())
    {
        const uint64_t lifetime_id = it->second.lifetime_id;
        forget_object_lifetime(it->second);
        m_objects.erase(it);
        m_file_replacement_backups.erase(lifetime_id);
        SCENEMANAGER_LOG("[SceneManager] Remove result: success\n");
        return true;
    }
    SCENEMANAGER_LOG("[SceneManager] Remove result: not found\n");
    return false;
}

uint64_t EditorSceneManager::object_lifetime(pnanovdb_editor_token_t* scene, pnanovdb_editor_token_t* name) const
{
    if (!scene || !name)
        return 0;
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_objects.find(make_key(scene, name));
    return it != m_objects.end() ? it->second.lifetime_id : 0;
}

bool EditorSceneManager::remove_if_lifetime(pnanovdb_editor_token_t* scene,
                                            pnanovdb_editor_token_t* name,
                                            uint64_t lifetime_id)
{
    if (!scene || !name || lifetime_id == 0)
        return false;

    std::lock_guard<std::mutex> lock(m_mutex);
    const uint64_t key = make_key(scene, name);
    auto it = m_objects.find(key);
    if (it == m_objects.end() || it->second.lifetime_id != lifetime_id)
        return false;
    forget_object_lifetime(it->second);
    m_objects.erase(it);
    m_file_replacement_backups.erase(lifetime_id);
    return true;
}

bool EditorSceneManager::rename_scene(pnanovdb_editor_token_t* old_scene, pnanovdb_editor_token_t* new_scene)
{
    if (!old_scene || !new_scene)
    {
        return false;
    }
    if (old_scene->id == new_scene->id)
    {
        return true;
    }

    std::lock_guard<std::mutex> lock(m_mutex);

    std::vector<std::pair<uint64_t, uint64_t>> key_moves;
    key_moves.reserve(m_objects.size());

    for (const auto& [old_key, obj] : m_objects)
    {
        if (!obj.scene_token || obj.scene_token->id != old_scene->id)
        {
            continue;
        }

        uint64_t new_key = make_key(new_scene, obj.name_token);
        auto existing = m_objects.find(new_key);
        if (existing != m_objects.end() && existing->first != old_key)
        {
            // Abort to avoid clobbering an existing object in the destination scene.
            return false;
        }
        key_moves.emplace_back(old_key, new_key);
    }

    for (const auto& [old_key, new_key] : key_moves)
    {
        auto node = m_objects.extract(old_key);
        if (!node.empty())
        {
            node.key() = new_key;
            node.mapped().scene_token = new_scene;
            node.mapped().ensure_shader_name_storage().object_key = new_key;
            reindex_object_lifetime(node.mapped(), new_key);
            m_objects.insert(std::move(node));
        }
    }

    auto scene_params = m_scene_custom_params.find(old_scene->id);
    if (scene_params != m_scene_custom_params.end())
    {
        m_scene_custom_params[new_scene->id] = std::move(scene_params->second);
        m_scene_custom_params.erase(scene_params);
    }

    return true;
}

bool EditorSceneManager::rename_object(pnanovdb_editor_token_t* scene,
                                       pnanovdb_editor_token_t* old_name,
                                       pnanovdb_editor_token_t* new_name)
{
    if (!scene || !old_name || !new_name)
    {
        return false;
    }
    if (old_name->id == new_name->id)
    {
        return true;
    }

    std::lock_guard<std::mutex> lock(m_mutex);

    const uint64_t old_key = make_key(scene, old_name);
    const uint64_t new_key = make_key(scene, new_name);
    if (m_objects.find(new_key) != m_objects.end())
    {
        return false;
    }

    auto node = m_objects.extract(old_key);
    if (node.empty())
    {
        return false;
    }
    node.key() = new_key;
    node.mapped().name_token = new_name;
    node.mapped().ensure_shader_name_storage().object_key = new_key;
    reindex_object_lifetime(node.mapped(), new_key);
    m_objects.insert(std::move(node));
    return true;
}

SceneObject* EditorSceneManager::get(pnanovdb_editor_token_t* scene, pnanovdb_editor_token_t* name)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    uint64_t key = make_key(scene, name);
    auto it = m_objects.find(key);
    return (it != m_objects.end()) ? &it->second : nullptr;
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
    m_lifetime_to_key.clear();
    m_file_replacement_backups.clear();
    m_scene_custom_params.clear();
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
    obj.params.shader_params_array_owner.reset();

    obj.params.shader_params_array = params_array;
    obj.shader_params() = params_array ? params_array->data : nullptr;
    obj.params.shader_params_array_owner = make_compute_array_owner(params_array, compute, {}, "params array");
}

bool EditorSceneManager::set_custom_scene_params(pnanovdb_editor_token_t* scene,
                                                 pnanovdb_editor_token_t* json,
                                                 std::string* error_message)
{
    if (!scene || !json || !json->str || json->str[0] == '\0')
    {
        if (error_message)
        {
            *error_message = "scene and json token string are required";
        }
        return false;
    }

    auto custom_params = std::make_shared<CustomSceneParams>();
    if (!custom_params->loadFromJsonString(json->str, "<custom scene params>", error_message))
    {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    m_scene_custom_params[scene->id] = std::move(custom_params);
    return true;
}

std::shared_ptr<CustomSceneParams> EditorSceneManager::get_custom_scene_params(pnanovdb_editor_token_t* scene)
{
    if (!scene)
    {
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_scene_custom_params.find(scene->id);
    if (it == m_scene_custom_params.end())
    {
        return nullptr;
    }
    return it->second;
}

} // namespace pnanovdb_editor
