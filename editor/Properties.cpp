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

#include <cmath>

#ifndef M_PI_2
#    define M_PI_2 1.57079632679489661923
#endif

namespace pnanovdb_editor
{
using namespace imgui_instance_user;

const float EPSILON = 1e-6f;

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

    bool isViewportCamera = (strcmp(selected_name, VIEWPORT_CAMERA) == 0);
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
        if (ImGui::Button("Set Viewport Camera"))
        {
            pnanovdb_vec3_t& up = state.eye_up;
            if (state.eye_up.x != ptr->render_settings->camera_state.eye_up.x ||
                state.eye_up.y != ptr->render_settings->camera_state.eye_up.y ||
                state.eye_up.z != ptr->render_settings->camera_state.eye_up.z)
            {
                pnanovdb_vec3_t& dir = state.eye_direction;
                pnanovdb_vec3_t right = { dir.y * up.z - dir.z * up.y, dir.z * up.x - dir.x * up.z,
                                          dir.x * up.y - dir.y * up.x };
                up.x = -(right.y * dir.z - right.z * dir.y);
                up.y = -(right.z * dir.x - right.x * dir.z);
                up.z = -(right.x * dir.y - right.y * dir.x);
                float len = sqrtf(up.x * up.x + up.y * up.y + up.z * up.z);
                if (len > EPSILON)
                {
                    up.x /= len;
                    up.y /= len;
                    up.z /= len;
                }
            }
            ptr->render_settings->camera_state = state;
            ptr->render_settings->camera_state.eye_up = up;
            ptr->render_settings->camera_config.is_orthographic = camera->configs[cameraIdx].is_orthographic;
            ptr->render_settings->sync_camera = PNANOVDB_TRUE;
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

        if (selection.type == pnanovdb_editor::ViewType::Root)
        {
        }
        else if (selection.type == pnanovdb_editor::ViewType::GaussianScenes)
        {
            auto* scene_manager = ptr->editor_scene->get_scene_manager();
            if (scene_manager)
            {
                std::string properties_shader_name;
                auto* scene_token = ptr->editor_scene->get_current_scene_token();
                scene_manager->with_object(scene_token, selection.name_token,
                                           [&](pnanovdb_editor::SceneObject* scene_obj)
                                           {
                                               if (scene_obj && !scene_obj->shader_name.empty())
                                               {
                                                   properties_shader_name = scene_obj->shader_name;
                                               }
                                           });

                if (!properties_shader_name.empty())
                {
                    ImGui::Separator();
                    scene_manager->shader_params.render(properties_shader_name.c_str());
                }
                else
                {
                    ImGui::TextDisabled("No shader assigned");
                }
            }
        }
        else if (selection.type == pnanovdb_editor::ViewType::NanoVDBs)
        {
            // Use with_object() to safely access shader_name while holding mutex
            auto* scene_manager = ptr->editor_scene->get_scene_manager();
            if (scene_manager)
            {
                std::string properties_shader_name;
                auto* scene_token = ptr->editor_scene->get_current_scene_token();
                scene_manager->with_object(scene_token, selection.name_token,
                                           [&](pnanovdb_editor::SceneObject* scene_obj)
                                           {
                                               if (scene_obj && !scene_obj->shader_name.empty())
                                               {
                                                   properties_shader_name = scene_obj->shader_name;
                                               }
                                           });

                // Shader name (editable)
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
                else
                {
                    ImGui::TextDisabled("No shader assigned");
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
