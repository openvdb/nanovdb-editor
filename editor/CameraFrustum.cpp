// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/editor/CameraFrustum.cpp

    \author Petra Hapalova

    \brief  Frustum rendering functions for the viewport
*/

#include "ImguiInstance.h"
#include "CameraFrustum.h"
#include "EditorScene.h"

#include "nanovdb_editor/putil/Camera.h"

#include <cmath>
#include <limits>

#ifndef M_PI_2
#    define M_PI_2 1.57079632679489661923
#endif

namespace pnanovdb_editor
{
const float EPSILON = 1e-6f;

struct CameraBasisVectors
{
    pnanovdb_vec3_t right;
    pnanovdb_vec3_t up;
    pnanovdb_vec3_t forward;
};

static ImVec2 projectToScreen(const pnanovdb_vec3_t& worldPos,
                              pnanovdb_camera_t* viewingCamera,
                              float screenWidth,
                              float screenHeight)
{
    pnanovdb_camera_mat_t view, proj;
    pnanovdb_camera_get_view(viewingCamera, &view);
    pnanovdb_camera_get_projection(viewingCamera, &proj, screenWidth, screenHeight);
    pnanovdb_camera_mat_t viewProj = pnanovdb_camera_mat_mul(view, proj);

    pnanovdb_vec4_t worldPos4 = { worldPos.x, worldPos.y, worldPos.z, 1.f };
    pnanovdb_vec4_t clipPos = pnanovdb_camera_vec4_transform(worldPos4, viewProj);

    if (clipPos.w > EPSILON)
    {
        float x = clipPos.x / clipPos.w;
        float y = clipPos.y / clipPos.w;

        // Convert from NDC to screen coordinates
        float screenX = (x + 1.f) * 0.5f * screenWidth;
        float screenY = (1.f - y) * 0.5f * screenHeight; // Flip Y coordinate

        return ImVec2(screenX, screenY);
    }

    return ImVec2(-1.f, -1.f); // Invalid position
}

static void calculateFrustumCorners(pnanovdb_camera_state_t& camera_state,
                                    pnanovdb_camera_config_t& camera_config,
                                    float aspectRatio,
                                    pnanovdb_vec3_t corners[8],
                                    CameraBasisVectors* basisVectors = nullptr,
                                    float frustumScale = 1.f)
{
    pnanovdb_vec3_t eyePosition = pnanovdb_camera_get_eye_position_from_state(&camera_state);

    // Calculate camera basis vectors
    pnanovdb_vec3_t forward = camera_state.eye_direction;
    pnanovdb_vec3_t up = camera_state.eye_up;
    pnanovdb_vec3_t right = { forward.y * up.z - forward.z * up.y, forward.z * up.x - forward.x * up.z,
                              forward.x * up.y - forward.y * up.x };

    // Normalize right vector
    float rightLength = sqrt(right.x * right.x + right.y * right.y + right.z * right.z);
    if (rightLength > EPSILON)
    {
        right.x /= rightLength;
        right.y /= rightLength;
        right.z /= rightLength;
    }
    else
    {
        right = { 1.f, 0.f, 0.f };
    }

    up.x = right.y * forward.z - right.z * forward.y;
    up.y = right.z * forward.x - right.x * forward.z;
    up.z = right.x * forward.y - right.y * forward.x;

    // Normalize the recalculated up vector
    float upLength = sqrt(up.x * up.x + up.y * up.y + up.z * up.z);
    if (upLength > EPSILON)
    {
        up.x /= upLength;
        up.y /= upLength;
        up.z /= upLength;
    }
    else
    {
        up = { 0.f, 1.f, 0.f };
    }

    if (basisVectors)
    {
        basisVectors->right = right;
        basisVectors->up = up;
        basisVectors->forward = forward;
    }

    // Handle reverse Z by swapping near/far planes
    float nearPlane = camera_config.is_reverse_z ? camera_config.far_plane : camera_config.near_plane;
    float farPlane = camera_config.is_reverse_z ? camera_config.near_plane : camera_config.far_plane;

    nearPlane = std::max(nearPlane, EPSILON);
    farPlane = std::min(farPlane, 10000000.f);

    // Calculate frustum dimensions
    float frustumNear = nearPlane * frustumScale;
    float frustumFar = farPlane * frustumScale;

    if (camera_config.is_orthographic)
    {
        frustumNear = 0.f; // place near plane at the camera position
        frustumFar = std::max(frustumNear, frustumFar);
    }

    float nearHeight, nearWidth, farHeight, farWidth;

    if (camera_config.is_orthographic)
    {
        float orthoHeight = camera_config.orthographic_y;
        float orthoWidth = orthoHeight * aspectRatio;
        nearHeight = farHeight = orthoHeight * frustumScale;
        nearWidth = farWidth = orthoWidth * frustumScale;
    }
    else
    {
        float tanHalfFov = tan(camera_config.fov_angle_y * 0.5f);
        nearHeight = frustumNear * tanHalfFov * frustumScale;
        nearWidth = nearHeight * aspectRatio;
        farHeight = frustumFar * tanHalfFov * frustumScale;
        farWidth = farHeight * aspectRatio;
    }

    // Calculate near plane corners
    pnanovdb_vec3_t nearCenter = { eyePosition.x + forward.x * frustumNear, eyePosition.y + forward.y * frustumNear,
                                   eyePosition.z + forward.z * frustumNear };

    corners[0] = { nearCenter.x - right.x * nearWidth * 0.5f - up.x * nearHeight * 0.5f,
                   nearCenter.y - right.y * nearWidth * 0.5f - up.y * nearHeight * 0.5f,
                   nearCenter.z - right.z * nearWidth * 0.5f - up.z * nearHeight * 0.5f };

    corners[1] = { nearCenter.x + right.x * nearWidth * 0.5f - up.x * nearHeight * 0.5f,
                   nearCenter.y + right.y * nearWidth * 0.5f - up.y * nearHeight * 0.5f,
                   nearCenter.z + right.z * nearWidth * 0.5f - up.z * nearHeight * 0.5f };

    corners[2] = { nearCenter.x + right.x * nearWidth * 0.5f + up.x * nearHeight * 0.5f,
                   nearCenter.y + right.y * nearWidth * 0.5f + up.y * nearHeight * 0.5f,
                   nearCenter.z + right.z * nearWidth * 0.5f + up.z * nearHeight * 0.5f };

    corners[3] = { nearCenter.x - right.x * nearWidth * 0.5f + up.x * nearHeight * 0.5f,
                   nearCenter.y - right.y * nearWidth * 0.5f + up.y * nearHeight * 0.5f,
                   nearCenter.z - right.z * nearWidth * 0.5f + up.z * nearHeight * 0.5f };

    // Calculate far plane corners
    pnanovdb_vec3_t farCenter = { eyePosition.x + forward.x * frustumFar, eyePosition.y + forward.y * frustumFar,
                                  eyePosition.z + forward.z * frustumFar };

    corners[4] = { farCenter.x - right.x * farWidth * 0.5f - up.x * farHeight * 0.5f,
                   farCenter.y - right.y * farWidth * 0.5f - up.y * farHeight * 0.5f,
                   farCenter.z - right.z * farWidth * 0.5f - up.z * farHeight * 0.5f };

    corners[5] = { farCenter.x + right.x * farWidth * 0.5f - up.x * farHeight * 0.5f,
                   farCenter.y + right.y * farWidth * 0.5f - up.y * farHeight * 0.5f,
                   farCenter.z + right.z * farWidth * 0.5f - up.z * farHeight * 0.5f };

    corners[6] = { farCenter.x + right.x * farWidth * 0.5f + up.x * farHeight * 0.5f,
                   farCenter.y + right.y * farWidth * 0.5f + up.y * farHeight * 0.5f,
                   farCenter.z + right.z * farWidth * 0.5f + up.z * farHeight * 0.5f };

    corners[7] = { farCenter.x - right.x * farWidth * 0.5f + up.x * farHeight * 0.5f,
                   farCenter.y - right.y * farWidth * 0.5f + up.y * farHeight * 0.5f,
                   farCenter.z - right.z * farWidth * 0.5f + up.z * farHeight * 0.5f };
}

static void drawCameraFrustum(imgui_instance_user::Instance* ptr,
                              ImVec2 windowPos,
                              ImVec2 windowSize,
                              pnanovdb_camera_view_t& camera,
                              int cameraIdx,
                              float alpha = 1.f)
{
    ImDrawList* drawList = ImGui::GetWindowDrawList();

    auto isPointFinite = [](const ImVec2& p) { return std::isfinite(p.x) && std::isfinite(p.y); };
    auto isPointInsideWindow = [&](const ImVec2& p)
    {
        return (p.x >= windowPos.x && p.x <= windowPos.x + windowSize.x && p.y >= windowPos.y &&
                p.y <= windowPos.y + windowSize.y);
    };
    auto isValidScreenPoint = [&](const ImVec2& p) { return isPointFinite(p) && isPointInsideWindow(p); };

    pnanovdb_camera_t viewingCamera = {};
    viewingCamera.state = ptr->render_settings->camera_state;
    viewingCamera.config = ptr->render_settings->camera_config;

    // Use a temporary perspective projection for overlay when both are ortho,
    // so the frustum box depth is visible on screen
    if (viewingCamera.config.is_orthographic && camera.configs[cameraIdx].is_orthographic)
    {
        viewingCamera.config.is_orthographic = PNANOVDB_FALSE;
        viewingCamera.config.fov_angle_y = 0.785398163f; // ~45 deg
        viewingCamera.config.near_plane = 0.1f;
        viewingCamera.config.far_plane = 10000.f;
    }

    float aspectRatio = camera.configs[cameraIdx].aspect_ratio;
    aspectRatio = aspectRatio <= 0.f ? windowSize.x / windowSize.y : aspectRatio;
    pnanovdb_vec3_t frustumCorners[8];
    CameraBasisVectors basisVectors;
    calculateFrustumCorners(camera.states[cameraIdx], camera.configs[cameraIdx], aspectRatio, frustumCorners,
                            &basisVectors, camera.frustum_scale);

    ImVec2 screenCorners[8];
    for (int i = 0; i < 8; i++)
    {
        screenCorners[i] = projectToScreen(frustumCorners[i], &viewingCamera, windowSize.x, windowSize.y);
        screenCorners[i].x += windowPos.x;
        screenCorners[i].y += windowPos.y;
    }

    ImU32 lineColor = IM_COL32(ImU32(camera.frustum_color.x * 255.f), ImU32(camera.frustum_color.y * 255.f),
                               ImU32(camera.frustum_color.z * 255.f), ImU32(alpha * 255.f));
    float frustumLineThickness = camera.frustum_scale * camera.frustum_line_width;

    auto drawEdge = [&](int a, int b)
    {
        if (screenCorners[a] == screenCorners[b] || !isValidScreenPoint(screenCorners[a]) ||
            !isValidScreenPoint(screenCorners[b]))
        {
            return;
        }
        drawList->AddLine(screenCorners[a], screenCorners[b], lineColor, frustumLineThickness);
    };

    // Near plane
    drawEdge(0, 1);
    drawEdge(1, 2);
    drawEdge(2, 3);
    drawEdge(3, 0);

    // Far plane
    drawEdge(4, 5);
    drawEdge(5, 6);
    drawEdge(6, 7);
    drawEdge(7, 4);

    // Sides
    drawEdge(0, 4);
    drawEdge(1, 5);
    drawEdge(2, 6);
    drawEdge(3, 7);

    pnanovdb_vec3_t eyePosition = pnanovdb_camera_get_eye_position_from_state(&camera.states[cameraIdx]);

    ImVec2 cameraScreenPos = projectToScreen(eyePosition, &viewingCamera, windowSize.x, windowSize.y);
    cameraScreenPos.x += windowPos.x;
    cameraScreenPos.y += windowPos.y;

    if (!isValidScreenPoint(cameraScreenPos))
    {
        return;
    }

    // XYZ axes in RGB
    pnanovdb_vec3_t forward = basisVectors.forward;
    pnanovdb_vec3_t up = basisVectors.up;
    pnanovdb_vec3_t right = basisVectors.right;

    pnanovdb_vec3_t xAxisEnd = { eyePosition.x + right.x * camera.axis_length,
                                 eyePosition.y + right.y * camera.axis_length,
                                 eyePosition.z + right.z * camera.axis_length };
    pnanovdb_vec3_t yAxisEnd = { eyePosition.x + up.x * camera.axis_length, eyePosition.y + up.y * camera.axis_length,
                                 eyePosition.z + up.z * camera.axis_length };
    pnanovdb_vec3_t zAxisEnd = { eyePosition.x + forward.x * camera.axis_length,
                                 eyePosition.y + forward.y * camera.axis_length,
                                 eyePosition.z + forward.z * camera.axis_length };

    ImVec2 xAxisScreenPos = projectToScreen(xAxisEnd, &viewingCamera, windowSize.x, windowSize.y);
    ImVec2 yAxisScreenPos = projectToScreen(yAxisEnd, &viewingCamera, windowSize.x, windowSize.y);
    ImVec2 zAxisScreenPos = projectToScreen(zAxisEnd, &viewingCamera, windowSize.x, windowSize.y);

    xAxisScreenPos.x += windowPos.x;
    xAxisScreenPos.y += windowPos.y;
    yAxisScreenPos.x += windowPos.x;
    yAxisScreenPos.y += windowPos.y;
    zAxisScreenPos.x += windowPos.x;
    zAxisScreenPos.y += windowPos.y;

    auto drawAxis = [&](const ImVec2& from, const ImVec2& to, ImU32 color)
    {
        if (from != to && isValidScreenPoint(to))
        {
            drawList->AddLine(from, to, color, camera.axis_thickness);
        }
    };

    drawAxis(cameraScreenPos, xAxisScreenPos, IM_COL32(255, 0, 0, 255)); // right
    drawAxis(cameraScreenPos, yAxisScreenPos, IM_COL32(0, 255, 0, 255)); // up
    drawAxis(cameraScreenPos, zAxisScreenPos, IM_COL32(0, 0, 255, 255)); // forward

    // Camera position
    if (isValidScreenPoint(cameraScreenPos))
    {
        float posRadius = 1.5f * camera.axis_thickness;
        ImU32 posColor = IM_COL32(222, 220, 113, ImU32(alpha * 255));

        drawList->AddCircleFilled(cameraScreenPos, posRadius, posColor);
    }
}

void CameraFrustum::render(imgui_instance_user::Instance* ptr)
{
    if (!ptr->editor_scene)
    {
        return;
    }

    // Get the main viewport (central docking area)
    ImGuiViewport* mainViewport = ImGui::GetMainViewport();
    ImVec2 viewportPos = mainViewport->Pos;
    ImVec2 viewportSize = mainViewport->Size;

    // Create an overlay window to draw in the main viewport area
    ImGui::SetNextWindowPos(viewportPos);
    ImGui::SetNextWindowSize(viewportSize);
    ImGui::SetNextWindowBgAlpha(0.f);

    ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoFocusOnAppearing |
                                   ImGuiWindowFlags_NoBringToFrontOnFocus;

    ImGui::Begin("CameraFrustumOverlay", nullptr, windowFlags);
    {
        ImVec2 windowPos = ImGui::GetWindowPos();
        ImVec2 windowSize = ImGui::GetWindowSize();

        const auto& camera_views = ptr->editor_scene->get_camera_views();
        for (const auto& cameraPair : camera_views)
        {
            pnanovdb_camera_view_t* camera = cameraPair.second;
            if (!camera)
            {
                continue;
            }

            if (camera->is_visible)
            {
                auto selection = ptr->editor_scene->get_selection();
                bool isViewSelected = (!selection.name.empty() && selection.name == cameraPair.first);
                int selected = isViewSelected ? ptr->editor_scene->get_camera_frustum_index(selection.name) : -1;
                // first draw non-selected cameras with lower alpha
                for (int i = 0; i < camera->num_cameras; i++)
                {
                    if (i == selected)
                    {
                        continue;
                    }
                    drawCameraFrustum(ptr, windowPos, windowSize, *camera, i, isViewSelected ? 0.8f : 1.f);
                }
                // than draw selected camera with even lower alpha
                if (selected >= 0)
                {
                    drawCameraFrustum(ptr, windowPos, windowSize, *camera, selected, 0.4f);
                }
            }
        }
    }
    ImGui::End();
}

} // namespace pnanovdb_editor
