// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

#include "EditorParamMapRegistry.h"

#include "CustomSceneParams.h"
#include "Editor.h"
#include "EditorSceneManager.h"

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace pnanovdb_editor
{

namespace
{

// Per-kind state carries whatever the registry must keep alive AND any existing
// synchronization on the held object that must remain held for the map window.
//
//  ShaderParamsState:
//      Captures shader_params_array_owner so the buffer survives a SceneObject
//      remove/replace. The pointer the caller writes through is obj->shader_params(),
//      which lives inside the array's data buffer. For objects whose buffer was
//      installed without a shared_ptr owner (test stubs, externally-owned buffers)
//      array_owner is null and the entry is a frame marker only.
//
//  ShaderNameState:
//      Tiny POD storage; no internal mutex (no concurrent UI reader exists for
//      shader-name dispatch — render reads it under the worker mutex).
//
//  CustomSceneParamsState:
//      Holds the params shared_ptr AND a unique_lock over the params' own
//      data_mutex, because CustomSceneParams::render() reads m_data from the UI
//      thread, which is not gated by the worker mutex.
struct ShaderParamsState
{
    std::shared_ptr<pnanovdb_compute_array_t> array_owner;
};

struct ShaderNameState
{
    std::shared_ptr<ShaderNameStorage> storage;
};

struct CustomSceneParamsState
{
    std::shared_ptr<CustomSceneParams> params;
    std::unique_lock<std::mutex> data_lock;
};

// monostate is the default-constructed state on first acquire, before
// init_if_first replaces it with the appropriate alternative.
using ParamMapState = std::variant<std::monostate, ShaderParamsState, ShaderNameState, CustomSceneParamsState>;

} // namespace

// Per-editor instance. Owns the refcounted entry table and provides per-thread
// LIFO of in-flight frames. The thread storage is stored in a function-local
// thread_local map keyed by `this`, so each registry sees only its own frames
// even when several editors run on the same thread.
class ParamMapRegistry
{
public:
    template <typename InitFn>
    ParamMapState& acquire(const ParamMapKey& key, InitFn&& init_if_first)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        Entry& e = m_entries[key];
        if (e.ref_count == 0)
        {
            init_if_first(e.state);
        }
        ++e.ref_count;
        return e.state;
    }

    template <typename TeardownFn>
    void release(const ParamMapKey& key, TeardownFn&& teardown_if_last)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_entries.find(key);
        if (it == m_entries.end() || it->second.ref_count == 0)
        {
            return;
        }
        if (--it->second.ref_count == 0)
        {
            teardown_if_last(it->second.state);
            m_entries.erase(it);
        }
    }

    size_t ref_count(const ParamMapKey& key) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_entries.find(key);
        return (it == m_entries.end()) ? 0u : it->second.ref_count;
    }

    void stack_push(ParamMapFrame frame)
    {
        thread_stack().push_back(frame);
    }

    bool stack_try_pop(ParamMapFrame& out_frame)
    {
        auto& stk = thread_stack();
        if (stk.empty())
        {
            return false;
        }
        out_frame = stk.back();
        stk.pop_back();
        return true;
    }

    size_t stack_depth() const
    {
        auto& m = thread_stack_map();
        auto it = m.find(this);
        return (it == m.end()) ? 0u : it->second.size();
    }

private:
    struct Entry
    {
        ParamMapState state;
        size_t ref_count = 0;
    };
    mutable std::mutex m_mutex;
    std::map<ParamMapKey, Entry> m_entries;

    // Per-thread, per-instance LIFO. The map itself is thread_local (lives in
    // each thread's storage) and indexed by `this`, so different registry
    // instances never see each other's frames on the same thread.
    static std::unordered_map<const ParamMapRegistry*, std::vector<ParamMapFrame>>& thread_stack_map()
    {
        thread_local std::unordered_map<const ParamMapRegistry*, std::vector<ParamMapFrame>> stacks;
        return stacks;
    }

    std::vector<ParamMapFrame>& thread_stack()
    {
        return thread_stack_map()[this];
    }
};

namespace
{

// Resolves the registry handle from the editor; returns nullptr if the editor
// is not initialised. Callers must defend against the nullptr.
ParamMapRegistry* registry_for(pnanovdb_editor_t* editor)
{
    return (editor && editor->impl) ? editor->impl->param_map_registry : nullptr;
}

} // namespace

ParamMapRegistry* create_param_map_registry()
{
    return new ParamMapRegistry();
}

void destroy_param_map_registry(ParamMapRegistry* registry)
{
    delete registry;
}

void* begin_shader_params_map(pnanovdb_editor_t* editor,
                              pnanovdb_editor_token_t* scene,
                              pnanovdb_editor_token_t* name,
                              const pnanovdb_reflect_data_type_t* data_type,
                              ParamMapKey* out_key)
{
    ParamMapRegistry* registry = registry_for(editor);
    if (!registry || !editor->impl->scene_manager || !scene || !name || !data_type || !out_key)
    {
        return nullptr;
    }

    void* result = nullptr;
    std::shared_ptr<pnanovdb_compute_array_t> array_owner;
    editor->impl->scene_manager->with_object(
        scene, name,
        [&](SceneObject* obj)
        {
            if (obj && obj->shader_params() && obj->shader_params_data_type() &&
                pnanovdb_reflect_layout_compare(obj->shader_params_data_type(), data_type))
            {
                result = obj->shader_params();
                array_owner = obj->params.shader_params_array_owner;
            }
        });
    if (!result)
    {
        return nullptr;
    }

    const ParamMapKey key{ ParamMapKind::ShaderParams, EditorSceneManager::make_key(scene, name) };
    registry->acquire(key, [&](ParamMapState& s) { s = ShaderParamsState{ std::move(array_owner) }; });
    *out_key = key;
    return result;
}

void* begin_custom_scene_params_map(pnanovdb_editor_t* editor,
                                    pnanovdb_editor_token_t* scene,
                                    const pnanovdb_reflect_data_type_t* data_type,
                                    ParamMapKey* out_key)
{
    ParamMapRegistry* registry = registry_for(editor);
    if (!registry || !editor->impl->scene_manager || !scene || !data_type || !out_key)
    {
        return nullptr;
    }

    std::shared_ptr<CustomSceneParams> params = editor->impl->scene_manager->get_custom_scene_params(scene);
    if (!params || !params->dataType() || !pnanovdb_reflect_layout_compare(params->dataType(), data_type))
    {
        return nullptr;
    }

    const ParamMapKey key{ ParamMapKind::CustomSceneParams, scene->id };
    ParamMapState& state = registry->acquire(key,
                                             [&](ParamMapState& s)
                                             {
                                                 CustomSceneParamsState payload;
                                                 payload.params = std::move(params);
                                                 payload.data_lock =
                                                     std::unique_lock<std::mutex>(payload.params->dataMutex());
                                                 s = std::move(payload);
                                             });
    *out_key = key;
    return std::get<CustomSceneParamsState>(state).params->data();
}

pnanovdb_editor_shader_name_t* begin_shader_name_map(pnanovdb_editor_t* editor,
                                                     pnanovdb_editor_token_t* scene,
                                                     pnanovdb_editor_token_t* name,
                                                     ParamMapKey* out_key)
{
    ParamMapRegistry* registry = registry_for(editor);
    if (!registry || !editor->impl->scene_manager || !out_key)
    {
        return nullptr;
    }

    const uint64_t object_key = EditorSceneManager::make_key(scene, name);
    std::shared_ptr<ShaderNameStorage> storage;
    editor->impl->scene_manager->with_object(scene, name,
                                             [&](SceneObject* obj)
                                             {
                                                 if (obj)
                                                 {
                                                     storage = obj->params.shader_name_storage;
                                                 }
                                             });

    if (!storage || storage->object_key != object_key)
    {
        return nullptr;
    }

    const ParamMapKey key{ ParamMapKind::ShaderName, object_key };
    ParamMapState& state = registry->acquire(key, [&](ParamMapState& s) { s = ShaderNameState{ std::move(storage) }; });
    *out_key = key;
    return &std::get<ShaderNameState>(state).storage->value;
}

void release_param_map(pnanovdb_editor_t* editor, const ParamMapKey& key)
{
    ParamMapRegistry* registry = registry_for(editor);
    if (!registry)
    {
        return;
    }
    registry->release(key,
                      [](ParamMapState& s)
                      {
                          if (auto* p = std::get_if<CustomSceneParamsState>(&s))
                          {
                              p->data_lock.unlock();
                          }
                      });
}

void param_map_stack_push(pnanovdb_editor_t* editor, ParamMapFrame frame)
{
    if (ParamMapRegistry* registry = registry_for(editor))
    {
        registry->stack_push(frame);
    }
}

bool param_map_stack_try_pop(pnanovdb_editor_t* editor, ParamMapFrame& out_frame)
{
    ParamMapRegistry* registry = registry_for(editor);
    return registry ? registry->stack_try_pop(out_frame) : false;
}

// Used in gtests

PNANOVDB_API size_t shader_name_map_ref_count(pnanovdb_editor_t* editor,
                                              pnanovdb_editor_token_t* scene,
                                              pnanovdb_editor_token_t* name)
{
    ParamMapRegistry* registry = registry_for(editor);
    if (!registry || !scene || !name)
    {
        return 0u;
    }
    const uint64_t object_key = EditorSceneManager::make_key(scene, name);
    return registry->ref_count({ ParamMapKind::ShaderName, object_key });
}

PNANOVDB_API size_t param_map_stack_depth(pnanovdb_editor_t* editor)
{
    ParamMapRegistry* registry = registry_for(editor);
    return registry ? registry->stack_depth() : 0u;
}

} // namespace pnanovdb_editor
