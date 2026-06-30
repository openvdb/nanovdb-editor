// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/editor/Properties.cpp

    \author Petra Hapalova

    \brief  Properties window for displaying and managing scene item properties
*/

#include "Properties.h"
#include "ImguiInstance.h"
#include "EditorScene.h"
#include "EditorSceneManager.h"
#include "EditorToken.h"
#include "Pipeline.h"
#include "ShaderParams.h"
#include "PipelineParams.h"
#include "ParamWidget.h"
#include "Console.h"

#include "nanovdb_editor/putil/Reflect.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#ifndef M_PI_2
#    define M_PI_2 1.57079632679489661923
#endif

namespace pnanovdb_editor
{
using namespace imgui_instance_user;

const float EPSILON = 1e-6f;

static constexpr float k_property_field_width = 140.0f;
static constexpr float k_property_combo_width = 200.0f;

struct PipelinePreset
{
    const char* label;
    pnanovdb_pipeline_type_t load;
    pnanovdb_pipeline_type_t process;
    pnanovdb_pipeline_type_t render;
    bool set_load;
    pnanovdb_uint32_t source_kinds;
};

static constexpr pnanovdb_uint32_t sourceKindBit(SceneObjectSourceKind kind)
{
    return 1u << static_cast<pnanovdb_uint32_t>(kind);
}

static constexpr pnanovdb_uint32_t k_gaussian_data_sources =
    sourceKindBit(SceneObjectSourceKind::GaussianData) | sourceKindBit(SceneObjectSourceKind::GaussianFile);
static constexpr pnanovdb_uint32_t k_voxelbvh_gaussian_sources =
    sourceKindBit(SceneObjectSourceKind::GaussianFile) | sourceKindBit(SceneObjectSourceKind::GaussianArrays);
static const PipelinePreset k_pipeline_presets[] = {
    { "Gaussian Splat", pnanovdb_pipeline_type_gaussian_load, pnanovdb_pipeline_type_noop,
      pnanovdb_pipeline_type_gaussian_splat, true, k_gaussian_data_sources },
    { "VoxelBVH from Gaussians", pnanovdb_pipeline_type_gaussian_load, pnanovdb_pipeline_type_voxelbvh_build,
      pnanovdb_pipeline_type_voxelbvh_gaussians_render, true, k_voxelbvh_gaussian_sources },
    { "Mesh VoxelBVH (triangles)", pnanovdb_pipeline_type_mesh_load, pnanovdb_pipeline_type_voxelbvh_build,
      pnanovdb_pipeline_type_voxelbvh_triangles_render, true, sourceKindBit(SceneObjectSourceKind::MeshTriangles) },
    { "Mesh VoxelBVH (lines)", pnanovdb_pipeline_type_mesh_load, pnanovdb_pipeline_type_voxelbvh_build,
      pnanovdb_pipeline_type_voxelbvh_lines_render, true, sourceKindBit(SceneObjectSourceKind::MeshLines) },
    { "VoxelBVH + RGBA8", pnanovdb_pipeline_type_noop, pnanovdb_pipeline_type_voxelbvh_rgba8_chain,
      pnanovdb_pipeline_type_nanovdb_render, false, sourceKindBit(SceneObjectSourceKind::MeshTriangles) },
    { "NanoVDB", pnanovdb_pipeline_type_noop, pnanovdb_pipeline_type_noop, pnanovdb_pipeline_type_nanovdb_render, true,
      sourceKindBit(SceneObjectSourceKind::NanoVDB) },
};

static void applyPreset(EditorScene* editor_scene,
                        EditorSceneManager* scene_manager,
                        pnanovdb_editor_token_t* scene_token,
                        pnanovdb_editor_token_t* name_token,
                        const PipelinePreset& preset,
                        PropertiesPanelState& st)
{
    pnanovdb_editor_t* editor = editor_scene ? editor_scene->get_editor() : nullptr;
    if (!editor)
        return;

    if (preset.set_load)
        editor->set_pipeline(editor, scene_token, name_token, pnanovdb_pipeline_stage_load, preset.load);
    editor->set_pipeline(editor, scene_token, name_token, pnanovdb_pipeline_stage_process, preset.process);
    editor->set_pipeline(editor, scene_token, name_token, pnanovdb_pipeline_stage_render, preset.render);

    if (scene_manager)
        scene_manager->with_object(scene_token, name_token,
                                   [](SceneObject* o)
                                   {
                                       if (o)
                                           scene_object_invalidate_process_from(o, 0);
                                   });
    editor->mark_pipeline_dirty(editor, scene_token, name_token);

    if (const char* s = pnanovdb_pipeline_get_shader_name(preset.render))
        editor_scene->set_selected_object_shader_name(s);

    if (preset.process != pnanovdb_pipeline_type_noop)
    {
        st.stage = (int)pnanovdb_pipeline_stage_process;
        st.step = 0;
    }
    else
    {
        st.stage = (int)pnanovdb_pipeline_stage_render;
        st.step = 0;
    }

    Console::getInstance().addLog("Applied preset '%s'", preset.label);
}

static void processNewVariant(imgui_instance_user::Instance* ptr,
                              EditorSceneManager* scene_manager,
                              pnanovdb_editor_token_t* scene_token,
                              pnanovdb_editor_token_t* name_token,
                              pnanovdb_pipeline_type_t process_pipeline)
{
    const auto* desc = pnanovdb_pipeline_get_descriptor(process_pipeline);
    if (!desc || !desc->params_data_type || desc->params_data_type->child_reflect_data_count == 0)
        return;

    std::vector<unsigned char> snapshot;
    size_t params_size = 0;
    scene_manager->with_object(scene_token, name_token,
                               [&](SceneObject* scene_obj)
                               {
                                   if (!scene_obj)
                                       return;
                                   auto& pp = scene_obj->process_params();
                                   if (pp.data && pp.size > 0)
                                   {
                                       params_size = pp.size;
                                       snapshot.resize(pp.size);
                                       std::memcpy(snapshot.data(), pp.data, pp.size);
                                   }
                               });
    if (snapshot.empty() || params_size == 0)
        return;

    std::string primary_name;
    double primary_value_d = 0.0;
    scene_manager->pipeline_params.primary_field(
        desc->params_data_type, desc->params_hints, snapshot.data(), params_size, primary_name, primary_value_d);
    const float primary_value = (float)primary_value_d;

    // Build variant name: source name + abbreviated field-name initials + int value.
    std::string name_suffix = "_";
    bool at_word_start = true;
    for (const char* p = primary_name.c_str(); *p; ++p)
    {
        if (isalpha((unsigned char)*p))
        {
            if (at_word_start)
            {
                name_suffix += (char)tolower((unsigned char)*p);
                at_word_start = false;
            }
        }
        else
        {
            at_word_start = true;
        }
    }
    name_suffix += std::to_string((int)primary_value);

    // Ensure we never overwrite an existing object when creating a variant.
    std::string base_name = std::string(name_token->str) + name_suffix;
    std::string new_name = base_name;
    int suffix_index = 1;
    for (;;)
    {
        bool exists = false;
        auto* candidate_token = EditorToken::getInstance().getToken(new_name.c_str());
        scene_manager->with_object(
            scene_token, candidate_token, [&](SceneObject* existing_obj) { exists = (existing_obj != nullptr); });
        if (!exists)
            break;
        new_name = base_name + "_" + std::to_string(suffix_index++);
    }

    bool created = pipeline_create_variant(scene_manager, scene_token, name_token, new_name.c_str());

    if (created)
    {
        auto* new_token = EditorToken::getInstance().getToken(new_name.c_str());
        ptr->editor_scene->add_nanovdb_placeholder(scene_token, new_token);
        ptr->editor_scene->select_render_view(scene_token, new_token);
        Console::getInstance().addLog("Creating '%s' from '%s' (%s=%.0f)...", new_name.c_str(),
                                      name_token->str ? name_token->str : "?", primary_name.c_str(), primary_value);
    }
}

static bool showPipelineSelector(const char* label,
                                 pnanovdb_pipeline_type_t* pipeline,
                                 pnanovdb_pipeline_stage_t stage,
                                 const std::vector<pnanovdb_pipeline_type_t>* allowed = nullptr)
{
    // Build filtered list: noop is always available, plus types matching the target stage
    pnanovdb_pipeline_type_t types[pnanovdb_pipeline_type_count];
    const char* names[pnanovdb_pipeline_type_count];
    int count = 0;
    int current_idx = 0;

    for (int i = 0; i < pnanovdb_pipeline_type_count; ++i)
    {
        auto type = static_cast<pnanovdb_pipeline_type_t>(i);
        const auto* desc = pnanovdb_pipeline_get_descriptor(type);
        if (!desc)
            continue;

        if (type != pnanovdb_pipeline_type_noop && desc->stage != stage)
            continue;

        // Optional compatibility filter: when provided, only list renders that can actually
        // consume the object's current output. noop (disable stage) and the currently selected
        // type are always kept so the combo can display them.
        if (allowed && type != pnanovdb_pipeline_type_noop && type != *pipeline &&
            std::find(allowed->begin(), allowed->end(), type) == allowed->end())
            continue;

        // Chain templates get their own selector (see showChainSelector); never list them here.
        if (desc->chain_step_count > 0)
            continue;

        if (type == *pipeline)
            current_idx = count;

        types[count] = type;
        names[count] = desc->name;
        count++;
    }

    // Render the label before the combo (ImGui draws BeginCombo's label on the right by default).
    const char* sep = strstr(label, "##");
    const std::string display = sep ? std::string(label, sep - label) : std::string(label);
    const std::string combo_id = std::string("##combo") + label;
    if (!display.empty())
    {
        ImGui::TextUnformatted(display.c_str());
        ImGui::SameLine();
    }

    bool changed = false;
    ImGui::SetNextItemWidth(k_property_combo_width);
    if (ImGui::BeginCombo(combo_id.c_str(), (current_idx < count) ? names[current_idx] : "Unknown"))
    {
        for (int i = 0; i < count; ++i)
        {
            bool is_selected = (types[i] == *pipeline);
            if (ImGui::Selectable(names[i], is_selected))
            {
                *pipeline = types[i];
                changed = true;
            }
            if (is_selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    return changed;
}

static void renderProcessStepParams(EditorSceneManager* scene_manager,
                                    pnanovdb_editor_t* editor,
                                    pnanovdb_editor_token_t* scene_token,
                                    pnanovdb_editor_token_t* name_token,
                                    size_t step_index,
                                    pnanovdb_pipeline_type_t step_type,
                                    const char* suffix,
                                    PropertiesPanelState& state)
{
    const auto* desc = pnanovdb_pipeline_get_descriptor(step_type);
    if (!desc || !desc->params_data_type || desc->params_data_type->child_reflect_data_count == 0)
        return;

    size_t params_size = 0;
    const uint64_t scene_id = scene_token ? scene_token->id : 0;
    const uint64_t object_id = name_token ? name_token->id : 0;
    const bool same_step = state.params_scene == scene_id && state.params_object == object_id &&
                           state.params_step == (int)step_index && state.params_type == (int)step_type;
    if (!same_step)
    {
        state.params_scene = scene_id;
        state.params_object = object_id;
        state.params_step = (int)step_index;
        state.params_type = (int)step_type;
        state.params_editing = false;
    }
    scene_manager->with_object(scene_token, name_token,
                               [&](SceneObject* o)
                               {
                                   if (!o || step_index >= o->pipeline.process_count())
                                       return;
                                   auto& pp = o->pipeline.process_step(step_index).params;
                                   if ((!pp.data || pp.size < desc->params_size) && desc->init_params)
                                   {
                                       free(pp.data);
                                       pp = {};
                                       desc->init_params(&pp);
                                   }
                                   if (pp.data && pp.size > 0)
                                   {
                                       params_size = pp.size;
                                       if (!state.params_editing || state.params_snapshot.size() != pp.size)
                                       {
                                           state.params_snapshot.resize(pp.size);
                                           std::memcpy(state.params_snapshot.data(), pp.data, pp.size);
                                       }
                                   }
                               });
    if (state.params_snapshot.empty() || params_size == 0)
        return;

    char step_suffix[64];
    snprintf(step_suffix, sizeof(step_suffix), "##step%zu%s", step_index, suffix);

    const PipelineParams::EditResult edit = scene_manager->pipeline_params.render(
        desc->params_data_type, desc->params_hints, state.params_snapshot.data(), params_size, step_suffix);

    state.params_editing |= edit.any_edited;

    const bool commit = edit.any_committed || (state.params_editing && !edit.any_active && !edit.any_edited);
    if (commit && editor)
    {
        pnanovdb_pipeline_params_t* pp =
            editor->map_process_step_params(editor, scene_token, name_token, (pnanovdb_uint32_t)step_index);
        if (pp)
        {
            if (pp->data && pp->size >= params_size)
                std::memcpy(pp->data, state.params_snapshot.data(), params_size);
        }
        editor->unmap_process_step_params(editor, scene_token, name_token, (pnanovdb_uint32_t)step_index);
        state.params_editing = false;
    }
}

// Restore the selected object's shader params to their JSON-declared defaults
static void showShaderParamsResetButton(EditorSceneManager* scene_manager,
                                        const pnanovdb_compute_t* compute,
                                        const char* shader_name)
{
    if (!scene_manager || !shader_name || !*shader_name)
    {
        return;
    }

    if (ImGui::Button("Reset parameters"))
    {
        if (scene_manager->reset_shader_params_to_defaults(compute, shader_name))
        {
            Console::getInstance().addLog("Shader params for '%s' reset to defaults", shader_name);
        }
        else
        {
            Console::getInstance().addLog("Failed to reset shader params for '%s'", shader_name);
        }
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Restore JSON-declared defaults for '%s'\nand refresh every object using it", shader_name);
    }
}

static bool loadPipelineUsesSourceFile(pnanovdb_pipeline_type_t load_pipeline)
{
    return load_pipeline == pnanovdb_pipeline_type_mesh_load || load_pipeline == pnanovdb_pipeline_type_gaussian_load ||
           load_pipeline == pnanovdb_pipeline_type_noop;
}

static const char* pipelineDisplayName(pnanovdb_pipeline_type_t type);

static void showSourceInfo(Instance* ptr,
                           EditorScene* editor_scene,
                           pnanovdb_editor_token_t* scene_token,
                           pnanovdb_editor_token_t* name_token,
                           const char* suffix,
                           const std::string& source_filepath,
                           pnanovdb_pipeline_type_t load_pipeline)
{
    pnanovdb_editor_t* editor = editor_scene ? editor_scene->get_editor() : nullptr;

    ImGui::Text("Load:");
    ImGui::SameLine();

    pnanovdb_pipeline_type_t selected_load = load_pipeline;
    char load_id[80];
    snprintf(load_id, sizeof(load_id), "##loadsel%s", suffix);
    if (showPipelineSelector(load_id, &selected_load, pnanovdb_pipeline_stage_load) && editor)
    {
        editor->set_pipeline(editor, scene_token, name_token, pnanovdb_pipeline_stage_load, selected_load);
        editor->mark_pipeline_dirty(editor, scene_token, name_token);
    }

    ImGui::TextDisabled("Source:");
    ImGui::SameLine();

    const bool can_browse = ptr && loadPipelineUsesSourceFile(load_pipeline);
    const float button_w = ImGui::GetFrameHeight();
    const float avail = ImGui::GetContentRegionAvail().x;

    char source_id[32];
    snprintf(source_id, sizeof(source_id), "##source%s", suffix);
    char source_buf[1024];
    strncpy(source_buf, source_filepath.c_str(), sizeof(source_buf) - 1);
    source_buf[sizeof(source_buf) - 1] = '\0';

    ImGui::SetNextItemWidth(avail - button_w - ImGui::GetStyle().ItemSpacing.x);
    ImGui::InputText(source_id, source_buf, sizeof(source_buf), ImGuiInputTextFlags_ReadOnly);
    if (ImGui::IsItemHovered() && !source_filepath.empty())
        ImGui::SetTooltip("%s", source_filepath.c_str());

    ImGui::SameLine();
    char browse_id[32];
    snprintf(browse_id, sizeof(browse_id), "...##srcbtn%s", suffix);
    ImGui::BeginDisabled(!can_browse);
    if (ImGui::Button(browse_id, ImVec2(button_w, 0.f)))
    {
        ptr->source_load_pipeline = load_pipeline;
        ptr->source_scene_token = scene_token;
        ptr->source_name_token = name_token;
        ptr->pending.find_source_file = true;
    }
    ImGui::EndDisabled();
}

static std::vector<pnanovdb_pipeline_type_t> compatiblePipelineTypes(EditorSceneManager* scene_manager,
                                                                     pnanovdb_editor_token_t* scene_token,
                                                                     pnanovdb_editor_token_t* name_token,
                                                                     pnanovdb_pipeline_stage_t stage,
                                                                     size_t step,
                                                                     bool* constrained)
{
    if (constrained)
        *constrained = false;
    std::vector<pnanovdb_pipeline_type_t> allowed;
    if (!scene_manager)
        return allowed;

    pnanovdb_uint32_t upstream = pnanovdb_pipeline_data_kind_none;
    SceneObjectSourceKind source_kind = SceneObjectSourceKind::None;
    scene_manager->with_object(scene_token, name_token,
                               [&](SceneObject* o)
                               {
                                   if (!o)
                                       return;
                                   source_kind = scene_object_source_kind(o);
                                   if (stage == pnanovdb_pipeline_stage_render && step == SIZE_MAX)
                                   {
                                       upstream = scene_object_renderable_data_kind(o);
                                   }
                                   else
                                   {
                                       const size_t s = (step == SIZE_MAX) ? o->pipeline.process_count() : step;
                                       upstream = scene_object_upstream_data_kind(o, s);
                                   }
                               });
    if (upstream == pnanovdb_pipeline_data_kind_none)
        return allowed;
    if (constrained)
        *constrained = true;

    for (int i = 0; i < pnanovdb_pipeline_type_count; ++i)
    {
        const auto type = static_cast<pnanovdb_pipeline_type_t>(i);
        const auto* desc = pnanovdb_pipeline_get_descriptor(type);
        const bool supports_source =
            stage != pnanovdb_pipeline_stage_process || process_pipeline_supports_source(source_kind, type);
        if (desc && desc->stage == stage && desc->chain_step_count == 0 && supports_source &&
            (desc->inputs == 0u || (desc->inputs & upstream)))
            allowed.push_back(type);
    }
    return allowed;
}

static void showSaveStageButton(const char* button_id,
                                EditorScene* editor_scene,
                                pnanovdb_editor_token_t* scene_token,
                                pnanovdb_editor_token_t* name_token,
                                size_t step_index)
{
    if (ImGui::Button(button_id) && editor_scene)
    {
        const char* base = (name_token && name_token->str) ? name_token->str : "object";
        std::string path = base + std::string("_p") + std::to_string(step_index + 1) + ".nvdb";
        editor_scene->save_nanovdb_file_stage(scene_token, name_token, (int)step_index, path.c_str());
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Save this step's retained NanoVDB output to <name>_p<step>.nvdb");
}

// Helper to show common visibility and pipeline UI for scene objects
static const char* pipelineDisplayName(pnanovdb_pipeline_type_t type)
{
    const auto* d = pnanovdb_pipeline_get_descriptor(type);
    return (d && d->name) ? d->name : "None";
}

static SceneObjectSourceKind objectSourceKind(EditorSceneManager* scene_manager,
                                              pnanovdb_editor_token_t* scene_token,
                                              pnanovdb_editor_token_t* name_token)
{
    SceneObjectSourceKind kind = SceneObjectSourceKind::None;
    if (!scene_manager)
        return kind;
    scene_manager->with_object(scene_token, name_token,
                               [&](SceneObject* o)
                               {
                                   if (!o)
                                       return;
                                   kind = scene_object_source_kind(o);
                               });
    return kind;
}

// Section 1: whole-pipeline preset picker
static void showPresetDropdown(EditorScene* editor_scene,
                               EditorSceneManager* scene_manager,
                               pnanovdb_editor_token_t* scene_token,
                               pnanovdb_editor_token_t* name_token,
                               const char* suffix,
                               PropertiesPanelState& st)
{
    pnanovdb_editor_t* editor = editor_scene ? editor_scene->get_editor() : nullptr;

    // Only offer presets compatible with the object's data; if unclassifiable, show everything
    const SceneObjectSourceKind source_kind = objectSourceKind(scene_manager, scene_token, name_token);

    ImGui::TextDisabled("Pipeline");
    char combo_id[48];
    snprintf(combo_id, sizeof(combo_id), "##preset%s", suffix);
    ImGui::SetNextItemWidth(k_property_combo_width);
    if (ImGui::BeginCombo(combo_id, "Apply preset..."))
    {
        for (const auto& preset : k_pipeline_presets)
        {
            const bool compatible =
                source_kind == SceneObjectSourceKind::None || (preset.source_kinds & sourceKindBit(source_kind)) != 0u;
            if (!compatible)
                continue;
            if (ImGui::Selectable(preset.label, false))
                applyPreset(editor_scene, scene_manager, scene_token, name_token, preset, st);
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    char reset_id[48];
    snprintf(reset_id, sizeof(reset_id), "Reset##resetall%s", suffix);
    if (ImGui::Button(reset_id) && editor)
    {
        editor->set_pipeline(editor, scene_token, name_token, pnanovdb_pipeline_stage_load, pnanovdb_pipeline_type_noop);
        editor->set_pipeline(
            editor, scene_token, name_token, pnanovdb_pipeline_stage_process, pnanovdb_pipeline_type_noop);
        editor->set_pipeline(
            editor, scene_token, name_token, pnanovdb_pipeline_stage_render, pnanovdb_pipeline_type_noop);
        scene_manager->with_object(scene_token, name_token,
                                   [](SceneObject* o)
                                   {
                                       if (o)
                                           scene_object_invalidate_process_from(o, 0);
                                   });
        editor->mark_pipeline_dirty(editor, scene_token, name_token);
        st.stage = (int)pnanovdb_pipeline_stage_process;
        st.step = 0;
    }
}

// Section 2: two-level Load / Process / Render tree
static void showStageTree(EditorScene* editor_scene,
                          EditorSceneManager* scene_manager,
                          pnanovdb_editor_token_t* scene_token,
                          pnanovdb_editor_token_t* name_token,
                          const char* suffix,
                          PropertiesPanelState& st,
                          pnanovdb_pipeline_type_t load_pipeline,
                          pnanovdb_pipeline_type_t render_pipeline)
{
    pnanovdb_editor_t* editor = editor_scene ? editor_scene->get_editor() : nullptr;

    auto leaf = [&](pnanovdb_pipeline_stage_t stage, int step, const std::string& text)
    {
        const bool selected = (st.stage == (int)stage && st.step == step);
        ImGui::Indent();
        char id[64];
        snprintf(id, sizeof(id), "##leaf%d_%d%s", (int)stage, step, suffix);
        const std::string label = text + id;
        if (ImGui::Selectable(label.c_str(), selected))
        {
            st.stage = (int)stage;
            st.step = step;
        }
        ImGui::Unindent();
    };

    const ImGuiTreeNodeFlags parent_flags = ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth;

    char load_node[48];
    snprintf(load_node, sizeof(load_node), "Load##loadnode%s", suffix);
    if (ImGui::TreeNodeEx(load_node, parent_flags))
    {
        leaf(pnanovdb_pipeline_stage_load, 0, pipelineDisplayName(load_pipeline));
        ImGui::TreePop();
    }

    char proc_node[48];
    snprintf(proc_node, sizeof(proc_node), "Process##procnode%s", suffix);
    if (ImGui::TreeNodeEx(proc_node, parent_flags))
    {
        pnanovdb_uint32_t count = 1;
        if (editor)
            count = editor->get_process_step_count(editor, scene_token, name_token);
        else
            scene_manager->with_object(scene_token, name_token,
                                       [&](SceneObject* o)
                                       {
                                           if (o)
                                               count = (pnanovdb_uint32_t)o->pipeline.process_count();
                                       });
        for (pnanovdb_uint32_t i = 0; i < count; ++i)
        {
            pnanovdb_pipeline_type_t t = pnanovdb_pipeline_type_noop;
            if (editor)
                t = editor->get_process_step(editor, scene_token, name_token, i);
            else
                scene_manager->with_object(scene_token, name_token,
                                           [&](SceneObject* o)
                                           {
                                               if (o && i < o->pipeline.process_count())
                                                   t = o->pipeline.process_step(i).type;
                                           });
            std::string text = pipelineDisplayName(t);
            if (count > 1)
                text = std::to_string((int)i + 1) + ". " + text;
            leaf(pnanovdb_pipeline_stage_process, (int)i, text);
        }
        ImGui::TreePop();
    }

    char rend_node[48];
    snprintf(rend_node, sizeof(rend_node), "Render##rendnode%s", suffix);
    if (ImGui::TreeNodeEx(rend_node, parent_flags))
    {
        leaf(pnanovdb_pipeline_stage_render, 0, pipelineDisplayName(render_pipeline));
        ImGui::TreePop();
    }
}

// Section 3: contextual action bar for the selected stage/step
static void showActionBar(Instance* ptr,
                          EditorScene* editor_scene,
                          EditorSceneManager* scene_manager,
                          pnanovdb_editor_token_t* scene_token,
                          pnanovdb_editor_token_t* name_token,
                          const char* suffix,
                          PropertiesPanelState& st,
                          pnanovdb_pipeline_type_t load_pipeline,
                          pnanovdb_pipeline_type_t process_pipeline,
                          pnanovdb_pipeline_type_t render_pipeline,
                          const std::string& source_filepath,
                          std::string& shader_name,
                          bool allow_shader_edit)
{
    pnanovdb_editor_t* editor = editor_scene ? editor_scene->get_editor() : nullptr;

    const pnanovdb_pipeline_stage_t sel = (pnanovdb_pipeline_stage_t)st.stage;

    if (sel == pnanovdb_pipeline_stage_load)
    {
        showSourceInfo(ptr, editor_scene, scene_token, name_token, suffix, source_filepath, load_pipeline);
    }
    else if (sel == pnanovdb_pipeline_stage_render)
    {
        ImGui::Text("Render:");
        ImGui::SameLine();

        bool constrained = false;
        const std::vector<pnanovdb_pipeline_type_t> allowed = compatiblePipelineTypes(
            scene_manager, scene_token, name_token, pnanovdb_pipeline_stage_render, SIZE_MAX, &constrained);
        pnanovdb_pipeline_type_t new_render = render_pipeline;
        char rsel[64];
        snprintf(rsel, sizeof(rsel), "##rendsel%s", suffix);
        if (showPipelineSelector(rsel, &new_render, pnanovdb_pipeline_stage_render, constrained ? &allowed : nullptr) &&
            editor)
        {
            const pnanovdb_pipeline_type_t prev =
                editor->get_pipeline(editor, scene_token, name_token, pnanovdb_pipeline_stage_render);
            editor->set_pipeline(editor, scene_token, name_token, pnanovdb_pipeline_stage_render, new_render);
            if (new_render != prev)
            {
                if (const char* s = pnanovdb_pipeline_get_shader_name(new_render))
                {
                    if (editor_scene)
                        editor_scene->set_selected_object_shader_name(s);
                    shader_name = s;
                }
            }
        }

        if (allow_shader_edit)
        {
            ImGui::TextDisabled("Shader:");
            ImGui::SameLine();
            char shader_buf[256];
            strncpy(shader_buf, shader_name.c_str(), sizeof(shader_buf) - 1);
            shader_buf[sizeof(shader_buf) - 1] = '\0';
            char shader_id[32];
            snprintf(shader_id, sizeof(shader_id), "##shader%s", suffix);
            char browse_id[32];
            snprintf(browse_id, sizeof(browse_id), "...##shaderbrowse%s", suffix);
            const float browse_w = ImGui::CalcTextSize("...").x + ImGui::GetStyle().FramePadding.x * 2.0f;
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - browse_w - ImGui::GetStyle().ItemSpacing.x);
            if (ImGui::InputText(shader_id, shader_buf, sizeof(shader_buf), ImGuiInputTextFlags_EnterReturnsTrue))
            {
                std::string new_shader = shader_buf;
                if (editor_scene)
                    editor_scene->set_selected_object_shader_name(new_shader);
                shader_name = new_shader;
                ptr->pending.update_shader = true;
            }
            ImGui::SameLine();
            if (ImGui::Button(browse_id))
            {
                ptr->pending.find_shader_file = true;
            }
        }

        if (!shader_name.empty())
            showShaderParamsResetButton(scene_manager, ptr->compute, shader_name.c_str());
    }
    else // process
    {
        const int step = st.step;

        auto with_this_object = [&](void (*fn)(SceneObject*))
        {
            scene_manager->with_object(scene_token, name_token,
                                       [fn](SceneObject* o)
                                       {
                                           if (o)
                                               fn(o);
                                       });
        };
        pnanovdb_pipeline_type_t step_type =
            editor ? editor->get_process_step(editor, scene_token, name_token, (pnanovdb_uint32_t)step) :
                     process_pipeline;
        ImGui::Text("Process %d:", step + 1);
        ImGui::SameLine();

        bool constrained = false;
        const std::vector<pnanovdb_pipeline_type_t> allowed = compatiblePipelineTypes(
            scene_manager, scene_token, name_token, pnanovdb_pipeline_stage_process, (size_t)step, &constrained);
        pnanovdb_pipeline_type_t new_type = step_type;
        char psel[64];
        snprintf(psel, sizeof(psel), "##procsel%d%s", step, suffix);
        if (showPipelineSelector(psel, &new_type, pnanovdb_pipeline_stage_process, constrained ? &allowed : nullptr) &&
            editor)
        {
            if (step == 0)
                editor->set_pipeline(editor, scene_token, name_token, pnanovdb_pipeline_stage_process, new_type);
            else
                editor->set_process_step(editor, scene_token, name_token, (pnanovdb_uint32_t)step, new_type);
        }

        const bool process_active = pipeline_async_process_running_for(scene_manager, scene_token, name_token);
        const bool runtime_cancelling = pipeline_async_process_cancelling_for(scene_manager, scene_token, name_token);
        const bool cancel_available = pipeline_async_process_cancel_available_for(scene_manager, scene_token, name_token);

        // Run: re-run this step and everything downstream of it
        char run_id[32];
        snprintf(run_id, sizeof(run_id), "Run##run%d%s", step, suffix);
        if (process_active || runtime_cancelling)
            ImGui::BeginDisabled();
        if (ImGui::Button(run_id))
        {
            with_this_object(scene_object_clear_process_cancel_state);
            // Re-run only this step and the steps after it
            scene_manager->with_object(scene_token, name_token,
                                       [step](SceneObject* o)
                                       {
                                           if (!o)
                                               return;
                                           scene_object_invalidate_process_from(o, step);
                                       });
        }
        if (process_active || runtime_cancelling)
            ImGui::EndDisabled();

        if (!process_active && !runtime_cancelling)
        {
            with_this_object(scene_object_clear_process_cancel_state);
        }
        const bool cancel_enabled = process_active && cancel_available && !runtime_cancelling;

        ImGui::SameLine();
        char cancel_id[32];
        snprintf(cancel_id, sizeof(cancel_id), "Cancel##cancel%d%s", step, suffix);
        if (!cancel_enabled)
        {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button(cancel_id))
        {
            with_this_object(scene_object_process_user_cancel);
            if (!pipeline_cancel_async_process(scene_manager, scene_token, name_token) && process_active)
            {
                Console::getInstance().addLog(
                    Console::LogLevel::Warning, "Cancel: no async process worker found for this object (will retry)");
            }
        }
        if (!cancel_enabled)
        {
            ImGui::EndDisabled();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        {
            ImGui::SetTooltip(runtime_cancelling ? "Cancelling the running process task..." :
                              process_active && !cancel_available ? "This process cannot be stopped once it is running" :
                              process_active ? "Stop the running process task and restore the state from before Run" :
                                               "No process task is running");
        }

        // New: create a variant from the primary step's params.
        if (step == 0)
        {
            ImGui::SameLine();
            char new_id[32];
            snprintf(new_id, sizeof(new_id), "New##new%s", suffix);
            if (ImGui::Button(new_id))
                processNewVariant(ptr, scene_manager, scene_token, name_token, step_type);
        }

        ImGui::SameLine();
        char save_id[48];
        snprintf(save_id, sizeof(save_id), "Save NanoVDB##psave%d%s", step, suffix);
        showSaveStageButton(save_id, editor_scene, scene_token, name_token, (size_t)step);

        // Remove an extra chain step (the primary process stage cannot be removed)
        if (step > 0)
        {
            ImGui::SameLine();
            char rm_id[32];
            snprintf(rm_id, sizeof(rm_id), "Remove##rm%d%s", step, suffix);
            if (ImGui::Button(rm_id))
            {
                const size_t step_index = (size_t)step;
                scene_manager->with_object(scene_token, name_token,
                                           [step_index](SceneObject* o)
                                           {
                                               if (!o)
                                                   return;
                                               scene_object_remove_process_step(o, step_index);
                                           });
                st.step = 0;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Remove this process step");
        }

        // Append a new (no-op) chain step and select it
        ImGui::SameLine();
        char add_id[40];
        snprintf(add_id, sizeof(add_id), "Add step##addstep%s", suffix);
        if (ImGui::Button(add_id) && editor)
        {
            const pnanovdb_uint32_t count = editor->get_process_step_count(editor, scene_token, name_token);
            editor->set_process_step(editor, scene_token, name_token, count, pnanovdb_pipeline_type_noop);
            st.step = (int)count;
        }

        // Drop-intermediates toggle is only meaningful for a multi-step chain
        pnanovdb_uint32_t count = 1;
        bool drop_intermediate = false;
        scene_manager->with_object(scene_token, name_token,
                                   [&](SceneObject* o)
                                   {
                                       if (!o)
                                           return;
                                       count = (pnanovdb_uint32_t)o->pipeline.process_count();
                                       drop_intermediate = o->pipeline.drop_intermediate;
                                   });
        if (count > 1)
        {
            char drop_id[48];
            snprintf(drop_id, sizeof(drop_id), "Drop intermediates%s", suffix);
            if (ImGui::Checkbox(drop_id, &drop_intermediate))
            {
                const bool di = drop_intermediate;
                scene_manager->with_object(scene_token, name_token,
                                           [di](SceneObject* o)
                                           {
                                               if (o)
                                                   o->pipeline.drop_intermediate = di;
                                           });
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "Release each step's intermediate output once the next step consumes it\n"
                    "(saves memory, but the intermediate can no longer be saved or inspected)");
        }
    }
}

// Section 4: parameters for the currently selected stage/step in the stage tree
static void showSelectedProperties(Instance* ptr,
                                   EditorScene* editor_scene,
                                   EditorSceneManager* scene_manager,
                                   pnanovdb_editor_token_t* scene_token,
                                   pnanovdb_editor_token_t* name_token,
                                   const char* suffix,
                                   PropertiesPanelState& st,
                                   const std::string& shader_name)
{
    pnanovdb_editor_t* editor = editor_scene ? editor_scene->get_editor() : nullptr;

    ImGui::TextDisabled("Properties");

    switch ((pnanovdb_pipeline_stage_t)st.stage)
    {
    case pnanovdb_pipeline_stage_load:
        ImGui::TextDisabled("No parameters");
        break;
    case pnanovdb_pipeline_stage_process:
    {
        const pnanovdb_pipeline_type_t t =
            editor ? editor->get_process_step(editor, scene_token, name_token, (pnanovdb_uint32_t)st.step) :
                     pnanovdb_pipeline_type_noop;
        renderProcessStepParams(scene_manager, editor, scene_token, name_token, (size_t)st.step, t, suffix, st);
        break;
    }
    case pnanovdb_pipeline_stage_render:
    {
        std::string current = shader_name;
        scene_manager->with_object(scene_token, name_token,
                                   [&](SceneObject* o)
                                   {
                                       if (!o)
                                           return;
                                       if (const char* s = pipeline_get_shader(o))
                                           current = s;
                                   });
        if (!current.empty())
            scene_manager->shader_params.render(current.c_str());
        break;
    }
    default:
        break;
    }
}

void Properties::showPipelinePanel(imgui_instance_user::Instance* ptr,
                                   EditorSceneManager* scene_manager,
                                   EditorScene* editor_scene,
                                   pnanovdb_editor_token_t* scene_token,
                                   pnanovdb_editor_token_t* name_token,
                                   const char* suffix,
                                   bool allow_shader_edit)
{
    std::string shader_name;
    std::string source_filepath;
    pnanovdb_pipeline_type_t load_pipeline = pnanovdb_pipeline_type_noop;
    pnanovdb_pipeline_type_t process_pipeline = pnanovdb_pipeline_type_noop;
    pnanovdb_pipeline_type_t render_pipeline = pnanovdb_pipeline_type_noop;
    bool is_visible = true;
    pnanovdb_uint32_t step_count = 1;

    scene_manager->with_object(scene_token, name_token,
                               [&](SceneObject* o)
                               {
                                   if (!o)
                                       return;
                                   scene_object_sync_render_to_chain(o);
                                   load_pipeline = o->load_pipeline();
                                   process_pipeline = o->process_pipeline();
                                   render_pipeline = o->render_pipeline();
                                   is_visible = o->visible;
                                   source_filepath = o->resources.source_filepath;
                                   step_count = (pnanovdb_uint32_t)o->pipeline.process_count();
                                   const char* shader = pipeline_get_shader(o);
                                   if (shader)
                                       shader_name = shader;
                               });

    const uint64_t key = name_token ? name_token->id : 0ULL;
    PropertiesPanelState& st = m_panel_state[key];
    if (st.stage < 0 || st.stage >= (int)pnanovdb_pipeline_stage_count)
        st.stage = (int)pnanovdb_pipeline_stage_process;
    if (st.stage == (int)pnanovdb_pipeline_stage_process && st.step >= (int)step_count)
        st.step = 0;

    char visible_id[40];
    snprintf(visible_id, sizeof(visible_id), "Visible%s", suffix);
    if (ImGui::Checkbox(visible_id, &is_visible))
    {
        const bool vis = is_visible;
        scene_manager->with_object(scene_token, name_token,
                                   [vis](SceneObject* o)
                                   {
                                       if (o)
                                           o->visible = vis;
                                   });
        if (is_visible && editor_scene)
            editor_scene->select_render_view(scene_token, name_token);
    }
    ImGui::Separator();

    showPresetDropdown(editor_scene, scene_manager, scene_token, name_token, suffix, st);
    ImGui::Separator();
    showStageTree(editor_scene, scene_manager, scene_token, name_token, suffix, st, load_pipeline, render_pipeline);
    ImGui::Separator();
    showActionBar(ptr, editor_scene, scene_manager, scene_token, name_token, suffix, st, load_pipeline,
                  process_pipeline, render_pipeline, source_filepath, shader_name, allow_shader_edit);
    ImGui::Separator();
    showSelectedProperties(ptr, editor_scene, scene_manager, scene_token, name_token, suffix, st, shader_name);
}

void Properties::showCameraViews(imgui_instance_user::Instance* ptr)
{
    if (!ptr->editor_scene)
    {
        return;
    }
    auto selection = ptr->editor_scene->get_properties_selection();
    const char* selected_name = (selection.name_token && selection.name_token->str) ? selection.name_token->str : "";

    pnanovdb_camera_view_t* camera = nullptr;
    ptr->editor_scene->for_each_view(ViewType::Cameras,
                                     [&](uint64_t name_id, const auto& view_data)
                                     {
                                         using ViewT = std::decay_t<decltype(view_data)>;
                                         if constexpr (std::is_same_v<ViewT, CameraViewContext>)
                                         {
                                             if (!camera && selection.name_token && selection.name_token->id == name_id)
                                             {
                                                 camera = view_data.camera_view.get();
                                             }
                                         }
                                     });
    if (!camera)
    {
        return;
    }

    if (camera->num_cameras == 0 || !camera->states || !camera->configs)
    {
        ImGui::TextDisabled("No camera state/configuration available");
        return;
    }

    int cameraIdx = ptr->editor_scene->get_camera_frustum_index(selection.name_token);
    cameraIdx = std::clamp(cameraIdx, 0, static_cast<int>(camera->num_cameras) - 1);
    ptr->editor_scene->set_camera_frustum_index(selection.name_token, cameraIdx);
    pnanovdb_camera_state_t& state = camera->states[cameraIdx];

    // Check if this camera is marked as the viewport camera
    bool isViewportCamera = ptr->editor_scene->is_viewport_camera(selection.name_token);

    if (isViewportCamera)
    {
        // Projection mode: Perspective vs Orthographic
        {
            if (ImGui::RadioButton("Perspective", ptr->render_settings->is_orthographic == PNANOVDB_FALSE))
            {
                ptr->render_settings->is_orthographic = PNANOVDB_FALSE;
                ptr->render_settings->camera_config.is_orthographic = PNANOVDB_FALSE;
            }
            ImGui::SameLine();
            if (ImGui::RadioButton("Orthographic", ptr->render_settings->is_orthographic == PNANOVDB_TRUE))
            {
                ptr->render_settings->is_orthographic = PNANOVDB_TRUE;
                ptr->render_settings->camera_config.is_orthographic = PNANOVDB_TRUE;
            }
        }
#if 0
        // View preset buttons
        {
            auto setLookUp = [&](float up_x, float up_y, float up_z)
            {
                ptr->render_settings->camera_state.eye_up.x = up_x;
                ptr->render_settings->camera_state.eye_up.y = up_y;
                ptr->render_settings->camera_state.eye_up.z = up_z;
            };

            auto setDirection = [&](float dir_x, float dir_y, float dir_z)
            {
                ptr->render_settings->camera_state.eye_direction.x = dir_x;
                ptr->render_settings->camera_state.eye_direction.y = dir_y;
                ptr->render_settings->camera_state.eye_direction.z = dir_z;
                ptr->render_settings->sync_camera = PNANOVDB_TRUE;
            };

            bool isYUp = (ptr->render_settings->is_y_up == PNANOVDB_TRUE);
            float upAxisVal = isYUp ? ptr->render_settings->camera_state.eye_up.y
                                    : ptr->render_settings->camera_state.eye_up.z;
            const float upSign = (upAxisVal < -EPSILON) ? -1.0f : 1.0f;

            if (ImGui::Button("Top"))
            {
                if (isYUp)
                {
                    setDirection(0.0f, -1.0f, 0.0f); // Look down -Y
                    setLookUp(0.0f, 0.0f, upSign * 1.0f); // Screen up is +/-Z (forward)
                }
                else
                {
                    setDirection(0.0f, 0.0f, -1.0f); // Look down -Z
                    setLookUp(0.0f, upSign * 1.0f, 0.0f); // Screen up is +/-Y (forward)
                }
                ptr->render_settings->sync_camera = PNANOVDB_TRUE;
            }

            ImGui::SameLine();
            if (ImGui::Button("Front"))
            {
                if (isYUp)
                {
                    setDirection(0.0f, 0.0f, 1.0f); // Look along +Z (forward)
                    setLookUp(0.0f, upSign * 1.0f, 0.0f);
                }
                else
                {
                    setDirection(0.0f, 1.0f, 0.0f); // Look along +Y (forward)
                    setLookUp(0.0f, 0.0f, upSign * 1.0f);
                }
            }

            ImGui::SameLine();
            if (ImGui::Button("Right"))
            {
                setDirection(1.0f, 0.0f, 0.0f);
                if (isYUp)
                {
                    setLookUp(0.0f, upSign * 1.0f, 0.0f);
                }
                else
                {
                    setLookUp(0.0f, 0.0f, upSign * 1.0f);
                }
            }
        }
#endif
        if (ImGui::Button("Reset"))
        {
            if (ptr->editor_scene)
            {
                pnanovdb_editor_token_t* name_token =
                    pnanovdb_editor::EditorToken::getInstance().getToken(s_render_settings_default);
                const pnanovdb_camera_state_t* saved_state = ptr->editor_scene->get_saved_camera_state(name_token);
                if (saved_state)
                {
                    ptr->render_settings->camera_state = *saved_state;
                }
                else
                {
                    // Fallback to default camera state if not found
                    pnanovdb_camera_state_t default_state = {};
                    pnanovdb_camera_state_default(&default_state, ptr->render_settings->is_y_up);
                    ptr->render_settings->camera_state = default_state;
                }
            }

            pnanovdb_camera_config_t default_config = {};
            pnanovdb_camera_config_default(&default_config);
            ptr->render_settings->camera_config = default_config;

            auto settings_it = ptr->saved_render_settings.find(s_render_settings_default);
            if (settings_it != ptr->saved_render_settings.end())
            {
                ptr->render_settings->is_projection_rh = settings_it->second.is_projection_rh;
                ptr->render_settings->is_orthographic = settings_it->second.is_orthographic;
                ptr->render_settings->is_reverse_z = settings_it->second.is_reverse_z;
            }
            else
            {
                ptr->render_settings->is_projection_rh = default_config.is_projection_rh;
                ptr->render_settings->is_orthographic = default_config.is_orthographic;
                ptr->render_settings->is_reverse_z = default_config.is_reverse_z;
            }

            ptr->render_settings->sync_camera = PNANOVDB_TRUE;
        }
    }
    else
    {
        ImGui::Text("Total Cameras: %d", camera->num_cameras);
        size_t maxIndex = (camera->num_cameras > 0) ? ((int)camera->num_cameras - 1) : 0;
        if (maxIndex > 0)
        {
            int current_index = ptr->editor_scene->get_camera_frustum_index(selection.name_token);
            if (ImGui::SliderInt("Camera Index", &current_index, 0, maxIndex, "%d"))
            {
                ptr->editor_scene->set_camera_frustum_index(selection.name_token, current_index);
            }
        }
        else
        {
            ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x, ImGui::GetFrameHeight()));
        }
    }

    pnanovdb_vec3_t eyePosition = pnanovdb_camera_get_eye_position_from_state(&state);
    float eyePos[3] = { eyePosition.x, eyePosition.y, eyePosition.z };
    if (ImGui::DragFloat3("Origin", eyePos, 0.1f))
    {
        // Keep look-at point fixed, recalculate direction and distance from new eye position
        pnanovdb_vec3_t delta = { state.position.x - eyePos[0], state.position.y - eyePos[1],
                                  state.position.z - eyePos[2] };
        float len2 = delta.x * delta.x + delta.y * delta.y + delta.z * delta.z;
        if (len2 > 0.f)
        {
            float len = pnanovdb_camera_sqrt(len2);
            state.eye_direction.x = delta.x / len;
            state.eye_direction.y = delta.y / len;
            state.eye_direction.z = delta.z / len;
            state.eye_distance_from_position = len;
        }
        else
        {
            state.eye_distance_from_position = 0.f;
        }

        if (isViewportCamera)
        {
            ptr->render_settings->camera_state = state;
            ptr->render_settings->sync_camera = PNANOVDB_TRUE;
        }
    }
    float lookAt[3] = { state.position.x, state.position.y, state.position.z };
    if (ImGui::DragFloat3("Look At", lookAt, 0.1f))
    {
        pnanovdb_vec3_t eye = pnanovdb_camera_get_eye_position_from_state(&state);
        pnanovdb_vec3_t delta = { lookAt[0] - eye.x, lookAt[1] - eye.y, lookAt[2] - eye.z };
        float len2 = delta.x * delta.x + delta.y * delta.y + delta.z * delta.z;
        if (len2 > 0.f)
        {
            float len = pnanovdb_camera_sqrt(len2);
            pnanovdb_vec3_t dir = { delta.x / len, delta.y / len, delta.z / len };
            state.position.x = lookAt[0];
            state.position.y = lookAt[1];
            state.position.z = lookAt[2];
            state.eye_direction = dir;
            state.eye_distance_from_position = len;
        }
        else
        {
            state.position.x = lookAt[0];
            state.position.y = lookAt[1];
            state.position.z = lookAt[2];
            state.eye_distance_from_position = 0.f;
        }

        if (isViewportCamera)
        {
            ptr->render_settings->camera_state = state;
            ptr->render_settings->sync_camera = PNANOVDB_TRUE;
        }
    }
    float upVec[3] = { state.eye_up.x, state.eye_up.y, state.eye_up.z };
    if (ImGui::DragFloat3("Up", upVec, 0.1f))
    {
        state.eye_up.x = upVec[0];
        state.eye_up.y = upVec[1];
        state.eye_up.z = upVec[2];
        float len =
            sqrtf(state.eye_up.x * state.eye_up.x + state.eye_up.y * state.eye_up.y + state.eye_up.z * state.eye_up.z);
        if (len > EPSILON)
        {
            state.eye_up.x /= len;
            state.eye_up.y /= len;
            state.eye_up.z /= len;
        }

        if (isViewportCamera)
        {
            ptr->render_settings->camera_state = state;
            ptr->render_settings->sync_camera = PNANOVDB_TRUE;
        }
    }
    if (!isViewportCamera)
    {
        ImGui::Separator();
        ImGui::DragFloat("Axis Length", &camera->axis_length, 1.f, 0.f, 100.f);
        ImGui::DragFloat("Axis Thickness", &camera->axis_thickness, 0.1f, 0.f, 10.f);
        ImGui::DragFloat("Frustum Line Width", &camera->frustum_line_width, 0.1f, 0.f, 10.f);
        ImGui::DragFloat("Frustum Scale", &camera->frustum_scale, 0.1f, 0.f, 10.f);
        float frustumColor[4] = { (float)camera->frustum_color.x, (float)camera->frustum_color.y,
                                  (float)camera->frustum_color.z, 1.0f };
        if (ImGui::ColorEdit4("Frustum Color", frustumColor))
        {
            camera->frustum_color.x = frustumColor[0];
            camera->frustum_color.y = frustumColor[1];
            camera->frustum_color.z = frustumColor[2];
        }
        ImGui::Separator();
        if (ImGui::DragFloat("Aspect Ratio", &camera->configs[cameraIdx].aspect_ratio, 0.01f, 0.f, 2.f))
        {
            if (isViewportCamera)
            {
                ptr->render_settings->camera_config = camera->configs[cameraIdx];
                ptr->render_settings->sync_camera = PNANOVDB_TRUE;
            }
        }
    }
    if (ImGui::DragFloat("FOV", &camera->configs[cameraIdx].fov_angle_y, 0.01f, 0.f, M_PI_2))
    {
        if (isViewportCamera)
        {
            ptr->render_settings->camera_config = camera->configs[cameraIdx];
            ptr->render_settings->sync_camera = PNANOVDB_TRUE;
        }
    }
    if (ImGui::DragFloat("Near Plane", &camera->configs[cameraIdx].near_plane, 0.1f, 0.01f, 10000.f))
    {
        if (isViewportCamera)
        {
            ptr->render_settings->camera_config = camera->configs[cameraIdx];
            ptr->render_settings->sync_camera = PNANOVDB_TRUE;
        }
    }
    if (ImGui::DragFloat("Far Plane", &camera->configs[cameraIdx].far_plane, 10.f, 1.f, 100000.f))
    {
        if (isViewportCamera)
        {
            ptr->render_settings->camera_config = camera->configs[cameraIdx];
            ptr->render_settings->sync_camera = PNANOVDB_TRUE;
        }
    }
    if (camera->configs[cameraIdx].is_orthographic)
    {
        if (ImGui::DragFloat("Orthographic Y", &camera->configs[cameraIdx].orthographic_y, 0.1f, 0.f, 100000.f))
        {
            if (isViewportCamera)
            {
                ptr->render_settings->camera_config = camera->configs[cameraIdx];
                ptr->render_settings->sync_camera = PNANOVDB_TRUE;
            }
        }
    }
}

void Properties::render(imgui_instance_user::Instance* ptr)
{
    if (ImGui::Begin(PROPERTIES, &ptr->window.show_scene_properties))
    {

        auto selection = ptr->editor_scene->get_properties_selection();
        if (!selection.name_token || !selection.name_token->str || selection.type == pnanovdb_editor::ViewType::None)
        {
            ImGui::TextDisabled("Scene is empty");
            ImGui::End();
            return;
        }
        ImGui::TextDisabled("%s", selection.name_token->str);
        ImGui::Separator();

        if (selection.type == pnanovdb_editor::ViewType::Root)
        {
        }
        else if (selection.type == pnanovdb_editor::ViewType::GaussianScenes)
        {
            auto* scene_manager = ptr->editor_scene->get_scene_manager();
            if (scene_manager)
            {
                char ui_suffix[64];
                snprintf(ui_suffix, sizeof(ui_suffix), "##gs_%llu",
                         (unsigned long long)(selection.name_token ? selection.name_token->id : 0ULL));

                auto* scene_token =
                    selection.scene_token ? selection.scene_token : ptr->editor_scene->get_current_scene_token();
                showPipelinePanel(ptr, scene_manager, ptr->editor_scene, scene_token, selection.name_token, ui_suffix,
                                  /*allow_shader_edit*/ false);
            }
        }
        else if (selection.type == pnanovdb_editor::ViewType::NanoVDBs)
        {
            auto* scene_manager = ptr->editor_scene->get_scene_manager();
            if (scene_manager)
            {
                char ui_suffix[64];
                snprintf(ui_suffix, sizeof(ui_suffix), "##nvdb_%llu",
                         (unsigned long long)(selection.name_token ? selection.name_token->id : 0ULL));

                auto* scene_token =
                    selection.scene_token ? selection.scene_token : ptr->editor_scene->get_current_scene_token();
                showPipelinePanel(ptr, scene_manager, ptr->editor_scene, scene_token, selection.name_token, ui_suffix,
                                  /*allow_shader_edit*/ true);
            }
        }
        else if (selection.type == pnanovdb_editor::ViewType::Cameras)
        {
            showCameraViews(ptr);
        }
    }
    ImGui::End();
}

} // namespace pnanovdb_editor
