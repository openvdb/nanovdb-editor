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
#include "Console.h"

#include <cmath>
#include <cctype>
#include <string>
#include <vector>

#ifndef M_PI_2
#    define M_PI_2 1.57079632679489661923
#endif

namespace pnanovdb_editor
{
using namespace imgui_instance_user;

const float EPSILON = 1e-6f;

static void renderPipelineProcessParams(EditorSceneManager* scene_manager,
                                        pnanovdb_editor_token_t* scene_token,
                                        pnanovdb_editor_token_t* name_token,
                                        pnanovdb_pipeline_type_t process_pipeline,
                                        const char* suffix,
                                        imgui_instance_user::Instance* ptr)
{
    const auto* desc = pnanovdb_pipeline_get_descriptor(process_pipeline);
    if (!desc || desc->param_field_count == 0 || !desc->param_fields)
        return;

    void* params_data = nullptr;
    size_t params_size = 0;
    scene_manager->with_object(
        scene_token, name_token,
        [&](pnanovdb_editor::SceneObject* scene_obj)
        {
            if (!scene_obj)
                return;
            auto& pp = scene_obj->pipeline.process().params;
            if ((!pp.data || pp.size < desc->params_size) && desc->init_params)
            {
                free(pp.data);
                pp = {};
                desc->init_params(&pp);
            }
            params_data = pp.data;
            params_size = pp.size;
        });

    if (!params_data || params_size == 0)
        return;

    // Snapshot current source params so "New" can keep source object unchanged.
    std::vector<unsigned char> original_params;
    original_params.resize(params_size);
    std::memcpy(original_params.data(), params_data, params_size);

    constexpr pnanovdb_uint32_t MAX_FIELDS = 16;
    pnanovdb_uint32_t field_count = desc->param_field_count;
    if (field_count > MAX_FIELDS)
        field_count = MAX_FIELDS;

    float field_values[MAX_FIELDS];
    for (pnanovdb_uint32_t i = 0; i < field_count; ++i)
    {
        const auto& field = desc->param_fields[i];
        if (field.type == PNANOVDB_REFLECT_TYPE_FLOAT && field.offset + sizeof(float) <= params_size)
            field_values[i] = *(const float*)((const char*)params_data + field.offset);
        else
            field_values[i] = field.default_value;
    }

    bool any_committed = false;
    char id_buf[128];

    for (pnanovdb_uint32_t i = 0; i < field_count; ++i)
    {
        const auto& field = desc->param_fields[i];
        if (field.type != PNANOVDB_REFLECT_TYPE_FLOAT)
            continue;

        snprintf(id_buf, sizeof(id_buf), "%s%s", field.name, suffix);

        ImGui::SetNextItemWidth(100.0f);
        ImGui::InputFloat(id_buf, &field_values[i], field.step, field.step * 10.0f, "%.1f");
        if (ImGui::IsItemHovered() && field.tooltip)
            ImGui::SetTooltip("%s", field.tooltip);

        if (field_values[i] < field.min_value)
            field_values[i] = field.min_value;
        if (field_values[i] > field.max_value)
            field_values[i] = field.max_value;

        if (ImGui::IsItemEdited())
        {
            float val = field_values[i];
            pnanovdb_uint64_t off = field.offset;
            scene_manager->with_object(
                scene_token, name_token,
                [val, off](pnanovdb_editor::SceneObject* scene_obj)
                {
                    if (scene_obj && scene_obj->pipeline.process().params.data)
                        *(float*)((char*)scene_obj->pipeline.process().params.data + off) = val;
                });
        }

        if (ImGui::IsItemDeactivatedAfterEdit())
            any_committed = true;
    }

    // Apply and New buttons
    char apply_id[64], new_id[64];
    snprintf(apply_id, sizeof(apply_id), "Apply%s", suffix);
    snprintf(new_id, sizeof(new_id), "New%s_new", suffix);

    ImGui::SameLine();
    bool apply_clicked = ImGui::Button(apply_id);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Re-run %s with current parameters", desc->name);
    ImGui::SameLine();
    bool new_clicked = ImGui::Button(new_id);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Create a new object with these parameters\n(keeps original unchanged)");

    if (new_clicked)
    {
        // Build variant name: source name + abbreviated field name initials + int value
        std::string name_suffix = "_";
        bool at_word_start = true;
        for (const char* p = desc->param_fields[0].name; *p; ++p)
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
        name_suffix += std::to_string((int)field_values[0]);

        // Ensure we never overwrite an existing object when creating a variant.
        std::string base_name = std::string(name_token->str) + name_suffix;
        std::string new_name = base_name;
        int suffix_index = 1;
        for (;;)
        {
            bool exists = false;
            auto* candidate_token = pnanovdb_editor::EditorToken::getInstance().getToken(new_name.c_str());
            scene_manager->with_object(
                scene_token, candidate_token,
                [&](pnanovdb_editor::SceneObject* existing_obj)
                {
                    exists = (existing_obj != nullptr);
                });

            if (!exists)
            {
                break;
            }

            new_name = base_name + "_" + std::to_string(suffix_index++);
        }

        bool created = pnanovdb_editor::pipeline_create_variant(
            scene_manager, scene_token, name_token, new_name.c_str());

        // Revert source object params to preserve "source unchanged" semantics for New.
        scene_manager->with_object(
            scene_token, name_token,
            [&](pnanovdb_editor::SceneObject* scene_obj)
            {
                if (!scene_obj)
                    return;

                auto& pp = scene_obj->pipeline.process().params;
                if (pp.data && pp.size >= original_params.size() && !original_params.empty())
                {
                    std::memcpy(pp.data, original_params.data(), original_params.size());
                }

                // New should not trigger source object re-processing.
                scene_obj->pipeline.process().dirty = false;
            });

        if (created)
        {
            auto* new_token = pnanovdb_editor::EditorToken::getInstance().getToken(new_name.c_str());
            ptr->editor_scene->add_nanovdb_placeholder(scene_token, new_token);
            ptr->editor_scene->select_render_view(scene_token, new_token);
            pnanovdb_editor::Console::getInstance().addLog(
                "Creating '%s' from '%s' (%s=%.0f)...",
                new_name.c_str(),
                name_token->str ? name_token->str : "?",
                desc->param_fields[0].name, field_values[0]);
        }
    }
    else if (apply_clicked)
    {
        scene_manager->with_object(
            scene_token, name_token,
            [desc](pnanovdb_editor::SceneObject* scene_obj)
            {
                if (scene_obj)
                {
                    scene_obj->pipeline.process().dirty = true;
                    pnanovdb_editor::Console::getInstance().addLog(
                        "Re-running %s...", desc->name);
                }
            });
    }
}

static bool showPipelineSelector(const char* label, pnanovdb_pipeline_type_t* pipeline, pnanovdb_pipeline_stage_t stage)
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

        // Include noop (always) or types matching the target stage
        if (type != pnanovdb_pipeline_type_noop && desc->stage != stage)
            continue;

        if (type == *pipeline)
            current_idx = count;

        types[count] = type;
        names[count] = desc->name;
        count++;
    }

    bool changed = false;
    if (ImGui::BeginCombo(label, (current_idx < count) ? names[current_idx] : "Unknown"))
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

// Helper to show common visibility and pipeline UI for scene objects
static void showVisibilityAndPipelineUI(EditorSceneManager* scene_manager,
                                        pnanovdb_editor_token_t* scene_token,
                                        pnanovdb_editor_token_t* name_token,
                                        const char* suffix,
                                        bool& is_visible,
                                        pnanovdb_pipeline_type_t& process_pipeline,
                                        pnanovdb_pipeline_type_t& render_pipeline,
                                        EditorScene* editor_scene = nullptr)
{
    char visible_id[32], process_id[32], render_id[32];
    snprintf(visible_id, sizeof(visible_id), "Visible%s", suffix);
    snprintf(process_id, sizeof(process_id), "Process%s", suffix);
    snprintf(render_id, sizeof(render_id), "Render%s", suffix);

    if (ImGui::Checkbox(visible_id, &is_visible))
    {
        scene_manager->with_object(scene_token, name_token,
                                   [is_visible](SceneObject* scene_obj)
                                   {
                                       if (scene_obj)
                                           scene_obj->visible = is_visible;
                                   });

        // When making an object visible, auto-switch render view to it
        if (is_visible && editor_scene)
        {
            editor_scene->select_render_view(scene_token, name_token);
        }
    }

    ImGui::TextDisabled("Pipeline:");
    bool pipeline_changed = false;
    pipeline_changed |= showPipelineSelector(process_id, &process_pipeline, pnanovdb_pipeline_stage_process);
    pipeline_changed |= showPipelineSelector(render_id, &render_pipeline, pnanovdb_pipeline_stage_render);

    if (pipeline_changed)
    {
        scene_manager->with_object(scene_token, name_token,
                                   [process_pipeline, render_pipeline](SceneObject* scene_obj)
                                   {
                                       if (scene_obj)
                                       {
                                           // Reinitialize process params when type changes so
                                           // they match the new pipeline
                                           if (scene_obj->pipeline.process().type != process_pipeline)
                                           {
                                               pnanovdb_pipeline_get_default_params(
                                                   process_pipeline, &scene_obj->pipeline.process().params);
                                           }
                                           scene_obj->pipeline.process().type = process_pipeline;
                                           scene_obj->pipeline.render().type = render_pipeline;
                                           scene_obj->pipeline.process().dirty = true;
                                       }
                                   });
    }
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

    int cameraIdx = ptr->editor_scene->get_camera_frustum_index(selection.name_token);
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

                std::string properties_shader_name;
                pnanovdb_pipeline_type_t process_pipeline = pnanovdb_pipeline_type_noop;
                pnanovdb_pipeline_type_t render_pipeline = pnanovdb_pipeline_type_raster2d;
                bool is_visible = true;
                auto* scene_token = ptr->editor_scene->get_current_scene_token();

                scene_manager->with_object(scene_token, selection.name_token,
                                           [&](pnanovdb_editor::SceneObject* scene_obj)
                                           {
                                               if (scene_obj)
                                               {
                                                   process_pipeline = scene_obj->pipeline.process().type;
                                                   render_pipeline = scene_obj->pipeline.render().type;
                                                   is_visible = scene_obj->visible;
                                                   const char* shader = pipeline_get_shader(scene_obj);
                                                   if (shader) properties_shader_name = shader;
                                               }
                                           });

                showVisibilityAndPipelineUI(scene_manager, scene_token, selection.name_token, ui_suffix,
                                            is_visible, process_pipeline, render_pipeline, ptr->editor_scene);

                renderPipelineProcessParams(scene_manager, scene_token, selection.name_token,
                                            process_pipeline, ui_suffix, ptr);

                ImGui::Separator();

                if (!properties_shader_name.empty())
                {
                    scene_manager->shader_params.render(properties_shader_name.c_str());
                }
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

                std::string properties_shader_name;
                pnanovdb_pipeline_type_t process_pipeline = pnanovdb_pipeline_type_noop;
                pnanovdb_pipeline_type_t render_pipeline = pnanovdb_pipeline_type_nanovdb_render;
                bool is_visible = true;
                auto* scene_token = ptr->editor_scene->get_current_scene_token();

                scene_manager->with_object(scene_token, selection.name_token,
                                           [&](pnanovdb_editor::SceneObject* scene_obj)
                                           {
                                               if (scene_obj)
                                               {
                                                   process_pipeline = scene_obj->pipeline.process().type;
                                                   render_pipeline = scene_obj->pipeline.render().type;
                                                   is_visible = scene_obj->visible;
                                                   const char* shader = pipeline_get_shader(scene_obj);
                                                   if (shader) properties_shader_name = shader;
                                               }
                                           });

                showVisibilityAndPipelineUI(scene_manager, scene_token, selection.name_token, ui_suffix,
                                            is_visible, process_pipeline, render_pipeline, ptr->editor_scene);

                renderPipelineProcessParams(scene_manager, scene_token, selection.name_token,
                                            process_pipeline, ui_suffix, ptr);

                ImGui::Separator();

                ImGui::TextDisabled("Shader:");
                ImGui::SameLine();
                char shader_buf[256];
                strncpy(shader_buf, properties_shader_name.c_str(), sizeof(shader_buf) - 1);
                shader_buf[sizeof(shader_buf) - 1] = '\0';
                if (ImGui::InputText("##shader", shader_buf, sizeof(shader_buf), ImGuiInputTextFlags_EnterReturnsTrue))
                {
                    std::string new_shader = shader_buf;
                    ptr->editor_scene->set_selected_object_shader_name(new_shader);
                    properties_shader_name = new_shader;
                    ptr->pending.update_shader = true;
                }

                if (!properties_shader_name.empty())
                {
                    ImGui::Separator();
                    scene_manager->shader_params.render(properties_shader_name.c_str());
                }
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
