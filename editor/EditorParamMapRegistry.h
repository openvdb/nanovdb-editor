// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   editor/EditorParamMapRegistry.h

    \brief
*/

#ifndef NANOVDB_EDITOR_EDITOR_PARAM_MAP_REGISTRY_H_HAS_BEEN_INCLUDED
#define NANOVDB_EDITOR_EDITOR_PARAM_MAP_REGISTRY_H_HAS_BEEN_INCLUDED

#include "nanovdb_editor/putil/Editor.h"

#include <cstdint>
#include <stddef.h>

namespace pnanovdb_editor
{

class ParamMapRegistry;

enum class ParamMapKind : uint8_t
{
    ShaderParams, // regular obj->shader_params() pointer + shader_params_array_owner pinned
    ShaderName, // shader-name pinned for the map window
    CustomSceneParams, // custom-scene-params pinned + data_lock acquired
};

struct ParamMapKey
{
    ParamMapKind kind;
    uint64_t id; // object_key (ShaderParams, ShaderName) or scene->id (CustomSceneParams)

    bool operator<(const ParamMapKey& other) const noexcept
    {
        if (kind != other.kind)
        {
            return static_cast<uint8_t>(kind) < static_cast<uint8_t>(other.kind);
        }
        return id < other.id;
    }
};

// Per-thread record for a successful map_params(): the key identifies which
// registry entry to release on the matching unmap_params(), and
// locked_worker_mutex says whether this call also took
// editor_worker->shader_params_mutex (so unmap knows whether to unlock).
struct ParamMapFrame
{
    ParamMapKey key;
    bool locked_worker_mutex;
};

// Lifecycle: created/destroyed by editor init() / shutdown(); stored on
// pnanovdb_editor_impl_t::param_map_registry.
ParamMapRegistry* create_param_map_registry();
void destroy_param_map_registry(ParamMapRegistry* registry);

// through editor->impl->param_map_registry.
void* begin_custom_scene_params_map(pnanovdb_editor_t* editor,
                                    pnanovdb_editor_token_t* scene,
                                    const pnanovdb_reflect_data_type_t* data_type,
                                    ParamMapKey* out_key);

pnanovdb_editor_shader_name_t* begin_shader_name_map(pnanovdb_editor_t* editor,
                                                     pnanovdb_editor_token_t* scene,
                                                     pnanovdb_editor_token_t* name,
                                                     ParamMapKey* out_key);

void* begin_shader_params_map(pnanovdb_editor_t* editor,
                              pnanovdb_editor_token_t* scene,
                              pnanovdb_editor_token_t* name,
                              const pnanovdb_reflect_data_type_t* data_type,
                              ParamMapKey* out_key);

void release_param_map(pnanovdb_editor_t* editor, const ParamMapKey& key);

void param_map_stack_push(pnanovdb_editor_t* editor, ParamMapFrame frame);
bool param_map_stack_try_pop(pnanovdb_editor_t* editor, ParamMapFrame& out_frame); // false when empty

PNANOVDB_API size_t shader_name_map_ref_count(pnanovdb_editor_t* editor,
                                              pnanovdb_editor_token_t* scene,
                                              pnanovdb_editor_token_t* name);

PNANOVDB_API size_t param_map_stack_depth(pnanovdb_editor_t* editor);

} // namespace pnanovdb_editor

#endif
