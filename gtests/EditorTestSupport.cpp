// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

#include "EditorTestSupport.h"

#include "editor/Editor.h"
#include "editor/EditorScene.h"
#include "editor/EditorSceneManager.h"

namespace pnanovdb_editor_test
{

pnanovdb_bool_t set_object_shader_params(pnanovdb_editor_t* editor,
                                         pnanovdb_editor_token_t* scene,
                                         pnanovdb_editor_token_t* name,
                                         void* shader_params,
                                         const pnanovdb_reflect_data_type_t* data_type)
{
    if (!editor || !editor->impl || !editor->impl->scene_manager || !scene || !name)
    {
        return PNANOVDB_FALSE;
    }
    pnanovdb_bool_t ok = PNANOVDB_FALSE;
    editor->impl->scene_manager->with_object(scene, name,
                                             [&](pnanovdb_editor::SceneObject* obj)
                                             {
                                                 if (!obj)
                                                 {
                                                     return;
                                                 }
                                                 obj->shader_params() = shader_params;
                                                 obj->shader_params_data_type() = data_type;
                                                 ok = PNANOVDB_TRUE;
                                             });
    return ok;
}

size_t snapshot_object_shader_params(pnanovdb_editor_t* editor,
                                     pnanovdb_editor_token_t* scene,
                                     pnanovdb_editor_token_t* name,
                                     void* out_buf,
                                     size_t out_buf_size)
{
    if (!editor || !editor->impl || !editor->impl->scene_manager || !scene || !name || !out_buf || out_buf_size == 0)
    {
        return 0u;
    }
    pnanovdb_editor::snapshot_object_shader_params_readonly(
        *editor->impl->scene_manager, scene, name, out_buf_size, out_buf_size, nullptr, nullptr, out_buf);
    return out_buf_size;
}

void* get_object_shader_params_ptr(pnanovdb_editor_t* editor, pnanovdb_editor_token_t* scene, pnanovdb_editor_token_t* name)
{
    if (!editor || !editor->impl || !editor->impl->scene_manager || !scene || !name)
    {
        return nullptr;
    }
    void* ptr = nullptr;
    editor->impl->scene_manager->with_object(scene, name,
                                             [&](pnanovdb_editor::SceneObject* obj)
                                             {
                                                 if (obj)
                                                 {
                                                     ptr = obj->shader_params();
                                                 }
                                             });
    return ptr;
}

pnanovdb_bool_t get_object_process_dirty(pnanovdb_editor_t* editor,
                                         pnanovdb_editor_token_t* scene,
                                         pnanovdb_editor_token_t* name)
{
    if (!editor || !editor->impl || !editor->impl->scene_manager || !scene || !name)
    {
        return PNANOVDB_FALSE;
    }
    bool dirty = false;
    editor->impl->scene_manager->with_object(scene, name,
                                             [&](pnanovdb_editor::SceneObject* obj)
                                             {
                                                 if (obj)
                                                 {
                                                     dirty = obj->process_dirty();
                                                 }
                                             });
    return dirty ? PNANOVDB_TRUE : PNANOVDB_FALSE;
}

size_t get_object_pipeline_params_size(pnanovdb_editor_t* editor,
                                       pnanovdb_editor_token_t* scene,
                                       pnanovdb_editor_token_t* name,
                                       pnanovdb_pipeline_stage_t stage)
{
    if (!editor || !editor->impl || !editor->impl->scene_manager || !scene || !name ||
        stage >= pnanovdb_pipeline_stage_count)
    {
        return 0u;
    }
    size_t size = 0u;
    editor->impl->scene_manager->with_object(scene, name,
                                             [&](pnanovdb_editor::SceneObject* obj)
                                             {
                                                 if (obj)
                                                 {
                                                     size = static_cast<size_t>(obj->pipeline.stages[stage].params.size);
                                                 }
                                             });
    return size;
}

const void* get_object_pipeline_params_data(pnanovdb_editor_t* editor,
                                            pnanovdb_editor_token_t* scene,
                                            pnanovdb_editor_token_t* name,
                                            pnanovdb_pipeline_stage_t stage)
{
    if (!editor || !editor->impl || !editor->impl->scene_manager || !scene || !name ||
        stage >= pnanovdb_pipeline_stage_count)
    {
        return nullptr;
    }
    const void* ptr = nullptr;
    editor->impl->scene_manager->with_object(scene, name,
                                             [&](pnanovdb_editor::SceneObject* obj)
                                             {
                                                 if (obj)
                                                 {
                                                     ptr = obj->pipeline.stages[stage].params.data;
                                                 }
                                             });
    return ptr;
}

size_t get_object_pipeline_shader_override_count(pnanovdb_editor_t* editor,
                                                 pnanovdb_editor_token_t* scene,
                                                 pnanovdb_editor_token_t* name,
                                                 pnanovdb_pipeline_stage_t stage)
{
    if (!editor || !editor->impl || !editor->impl->scene_manager || !scene || !name ||
        stage >= pnanovdb_pipeline_stage_count)
    {
        return 0u;
    }
    size_t count = 0u;
    editor->impl->scene_manager->with_object(scene, name,
                                             [&](pnanovdb_editor::SceneObject* obj)
                                             {
                                                 if (obj)
                                                 {
                                                     count = obj->pipeline.stages[stage].shader_overrides.size();
                                                 }
                                             });
    return count;
}

pnanovdb_bool_t append_blank_shader_override(pnanovdb_editor_t* editor,
                                             pnanovdb_editor_token_t* scene,
                                             pnanovdb_editor_token_t* name,
                                             pnanovdb_pipeline_stage_t stage)
{
    if (!editor || !editor->impl || !editor->impl->scene_manager || !scene || !name ||
        stage >= pnanovdb_pipeline_stage_count)
    {
        return PNANOVDB_FALSE;
    }
    pnanovdb_bool_t ok = PNANOVDB_FALSE;
    editor->impl->scene_manager->with_object(scene, name,
                                             [&](pnanovdb_editor::SceneObject* obj)
                                             {
                                                 if (!obj)
                                                     return;
                                                 obj->pipeline.stages[stage].shader_overrides.emplace_back();
                                                 ok = PNANOVDB_TRUE;
                                             });
    return ok;
}

} // namespace pnanovdb_editor_test
