// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/editor/CameraStateHandler.h

    \author Petra Hapalova

    \brief  This file contains the ImGui settings handler for camera states
*/

#pragma once

#include "ImguiInstance.h"

#include "nanovdb_editor/putil/Camera.h"

#include <imgui_internal.h>

#include <string>
#include <map>

namespace imgui_instance_user
{
namespace CameraStateHandler
{
static const char* FIELD_POSITION = "position";
static const char* FIELD_EYE_DIRECTION = "eye_direction";
static const char* FIELD_EYE_UP = "eye_up";
static const char* FIELD_EYE_DISTANCE = "eye_distance_from_position";
static const char* FIELD_ORTHOGRAPHIC_SCALE = "orthographic_scale";

static void ClearAll(ImGuiContext* ctx, ImGuiSettingsHandler* handler)
{
    Instance* instance = (Instance*)handler->UserData;
    instance->saved_camera_states.clear();
}

static void* ReadOpen(ImGuiContext* ctx, ImGuiSettingsHandler* handler, const char* name)
{
    Instance* instance = (Instance*)handler->UserData;

    // name is the camera state profile name after [CameraState][name]
    if (instance->saved_camera_states.find(name) == instance->saved_camera_states.end())
    {
        // Get is_y_up from render settings for proper initialization
        bool is_y_up = true;
        auto it = instance->saved_render_settings.find(name);
        if (it != instance->saved_render_settings.end())
        {
            is_y_up = it->second.is_y_up;
        }
        pnanovdb_camera_state_default(&instance->saved_camera_states[name], is_y_up);
    }

    return (void*)name;
}

static void ReadLine(ImGuiContext* ctx, ImGuiSettingsHandler* handler, void* entry, const char* line)
{
    const char* name = (const char*)entry;
    Instance* instance = (Instance*)handler->UserData;

    float x, y, z;
    char fmt[128];

    snprintf(fmt, sizeof(fmt), "%s=%%f,%%f,%%f", FIELD_POSITION);
    if (sscanf(line, fmt, &x, &y, &z) == 3)
    {
        instance->saved_camera_states[name].position.x = x;
        instance->saved_camera_states[name].position.y = y;
        instance->saved_camera_states[name].position.z = z;
    }
    else if (snprintf(fmt, sizeof(fmt), "%s=%%f,%%f,%%f", FIELD_EYE_DIRECTION), sscanf(line, fmt, &x, &y, &z) == 3)
    {
        instance->saved_camera_states[name].eye_direction.x = x;
        instance->saved_camera_states[name].eye_direction.y = y;
        instance->saved_camera_states[name].eye_direction.z = z;
    }
    else if (snprintf(fmt, sizeof(fmt), "%s=%%f,%%f,%%f", FIELD_EYE_UP), sscanf(line, fmt, &x, &y, &z) == 3)
    {
        instance->saved_camera_states[name].eye_up.x = x;
        instance->saved_camera_states[name].eye_up.y = y;
        instance->saved_camera_states[name].eye_up.z = z;
    }
    else if (snprintf(fmt, sizeof(fmt), "%s=%%f", FIELD_EYE_DISTANCE), sscanf(line, fmt, &x) == 1)
    {
        instance->saved_camera_states[name].eye_distance_from_position = x;
    }
    else if (snprintf(fmt, sizeof(fmt), "%s=%%f", FIELD_ORTHOGRAPHIC_SCALE), sscanf(line, fmt, &x) == 1)
    {
        instance->saved_camera_states[name].orthographic_scale = x;
    }
}

static void WriteAll(ImGuiContext* ctx, ImGuiSettingsHandler* handler, ImGuiTextBuffer* buf)
{
    Instance* instance = (Instance*)handler->UserData;

    auto save_camera_state = [&](const std::string& name, const pnanovdb_camera_state_t& camera_state)
    {
        buf->appendf("[%s][%s]\n", handler->TypeName, name.c_str());
        buf->appendf(
            "%s=%f,%f,%f\n", FIELD_POSITION, camera_state.position.x, camera_state.position.y, camera_state.position.z);
        buf->appendf("%s=%f,%f,%f\n", FIELD_EYE_DIRECTION, camera_state.eye_direction.x, camera_state.eye_direction.y,
                     camera_state.eye_direction.z);
        buf->appendf("%s=%f,%f,%f\n", FIELD_EYE_UP, camera_state.eye_up.x, camera_state.eye_up.y, camera_state.eye_up.z);
        buf->appendf("%s=%f\n", FIELD_EYE_DISTANCE, camera_state.eye_distance_from_position);
        buf->appendf("%s=%f\n", FIELD_ORTHOGRAPHIC_SCALE, camera_state.orthographic_scale);
        buf->append("\n");
    };

    for (const auto& pair : instance->saved_camera_states)
    {
        save_camera_state(pair.first, pair.second);
    }
}

static void Register(ImGuiContext* context, Instance* instance)
{
    ImGuiSettingsHandler camera_state_handler;
    camera_state_handler.TypeName = "CameraState";
    camera_state_handler.TypeHash = ImHashStr("CameraState");
    camera_state_handler.ClearAllFn = ClearAll;
    camera_state_handler.ReadOpenFn = ReadOpen;
    camera_state_handler.ReadLineFn = ReadLine;
    camera_state_handler.ApplyAllFn = nullptr; // No post-processing needed
    camera_state_handler.WriteAllFn = WriteAll;
    camera_state_handler.UserData = instance;

    context->SettingsHandlers.push_back(camera_state_handler);
}
} // namespace CameraStateHandler
} // namespace imgui_instance_user
