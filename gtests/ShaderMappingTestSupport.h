// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <nanovdb_editor/putil/Editor.h>
#include "editor/EditorSceneManager.h"

#include <cstring>
#include <functional>

namespace pnanovdb_editor_test
{

inline bool map_shader_defaults(pnanovdb_editor_t& editor,
                                 const pnanovdb_compute_t& compute,
                                 pnanovdb_editor_token_t* scene,
                                 pnanovdb_editor_token_t* name,
                                 const char* shader_name,
                                 const std::function<void(pnanovdb_uint8_t*)>& edit = {})
{
    auto* defaults = pnanovdb_editor::EditorSceneManager::create_isolated_shader_params(
        &compute, shader_name, nullptr, PNANOVDB_COMPUTE_CONSTANT_BUFFER_MAX_SIZE);
    if (!defaults)
    {
        return false;
    }
    auto* mapped = static_cast<pnanovdb_editor_shader_t*>(editor.map_params(
        &editor, scene, name, PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_editor_shader_t)));
    if (!mapped)
    {
        compute.destroy_array(defaults);
        return false;
    }
    mapped->shader_name = editor.get_token(shader_name);
    std::memcpy(mapped->shader_params, defaults->data, sizeof(mapped->shader_params));
    compute.destroy_array(defaults);
    if (edit)
    {
        edit(mapped->shader_params);
    }
    editor.unmap_params(&editor, scene, name);
    return true;
}

} // namespace pnanovdb_editor_test
