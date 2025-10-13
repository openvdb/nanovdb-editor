// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/editor/RenderSettingsHandler.h

    \author Petra Hapalova

    \brief  This file contains the ImGui settings handler for editor render settings.
*/

#pragma once

#include "ImguiInstance.h"

#include "nanovdb_editor/putil/Camera.h"

#include <imgui_internal.h>

#include <string>
#include <map>

namespace imgui_instance_user
{
namespace RenderSettingsHandler
{
static void ClearAll(ImGuiContext* ctx, ImGuiSettingsHandler* handler)
{
    Instance* instance = (Instance*)handler->UserData;
    instance->saved_render_settings.clear();

    instance->saved_render_settings[s_render_settings_default] = {};
}

static void* ReadOpen(ImGuiContext* ctx, ImGuiSettingsHandler* handler, const char* name)
{
    // name is the camera state name after the [CameraState][name] header
    Instance* instance = (Instance*)handler->UserData;

    // Create a new entry in our camera states map if it doesn't exist
    if (instance->saved_render_settings.find(name) == instance->saved_render_settings.end())
    {
        instance->saved_render_settings[name] = {};
        // not in settings yet, use defaults
        pnanovdb_camera_config_default(&instance->saved_render_settings[name].camera_config);
    }

    return (void*)name;
}

static void ReadLine(ImGuiContext* ctx, ImGuiSettingsHandler* handler, void* entry, const char* line)
{
    const char* name = (const char*)entry;
    Instance* instance = (Instance*)handler->UserData;

    // Parse line in format "key=value"
    float x, y, z;
    int boolValue;
    if (sscanf(line, "position=%f,%f,%f", &x, &y, &z) == 3)
    {
        instance->saved_render_settings[name].camera_state.position.x = x;
        instance->saved_render_settings[name].camera_state.position.y = y;
        instance->saved_render_settings[name].camera_state.position.z = z;
    }
    else if (sscanf(line, "eye_direction=%f,%f,%f", &x, &y, &z) == 3)
    {
        instance->saved_render_settings[name].camera_state.eye_direction.x = x;
        instance->saved_render_settings[name].camera_state.eye_direction.y = y;
        instance->saved_render_settings[name].camera_state.eye_direction.z = z;
    }
    else if (sscanf(line, "eye_up=%f,%f,%f", &x, &y, &z) == 3)
    {
        instance->saved_render_settings[name].camera_state.eye_up.x = x;
        instance->saved_render_settings[name].camera_state.eye_up.y = y;
        instance->saved_render_settings[name].camera_state.eye_up.z = z;
    }
    else if (sscanf(line, "eye_distance_from_position=%f", &x) == 1)
    {
        instance->saved_render_settings[name].camera_state.eye_distance_from_position = x;
    }
    else if (sscanf(line, "orthographic_scale=%f", &x) == 1)
    {
        instance->saved_render_settings[name].camera_state.orthographic_scale = x;
    }
    else if (sscanf(line, "vsync=%d", &boolValue) == 1)
    {
        instance->saved_render_settings[name].vsync = (pnanovdb_bool_t)boolValue;
    }
    else if (sscanf(line, "is_projection_rh=%d", &boolValue) == 1)
    {
        instance->saved_render_settings[name].is_projection_rh = (pnanovdb_bool_t)boolValue;
    }
    else if (sscanf(line, "is_orthographic=%d", &boolValue) == 1)
    {
        instance->saved_render_settings[name].is_orthographic = (pnanovdb_bool_t)boolValue;
    }
    else if (sscanf(line, "reverse_z=%d", &boolValue) == 1)
    {
        instance->saved_render_settings[name].is_reverse_z = (pnanovdb_bool_t)boolValue;
    }
    else if (sscanf(line, "y_up=%d", &boolValue) == 1)
    {
        instance->saved_render_settings[name].is_y_up = (pnanovdb_bool_t)boolValue;
    }
    else if (sscanf(line, "is_upside_down=%d", &boolValue) == 1)
    {
        instance->saved_render_settings[name].is_upside_down = (pnanovdb_bool_t)boolValue;
    }
    else if (sscanf(line, "camera_speed_multiplier=%f", &x) == 1)
    {
        instance->saved_render_settings[name].camera_speed_multiplier = x;
    }
    // else if (sscanf(line, "ui_profile_name=%255s", instance->saved_render_settings[name].ui_profile_name) == 1)
    // {
    //     // String is safely read with length limit, ensuring null termination
    // }
}

static void WriteAll(ImGuiContext* ctx, ImGuiSettingsHandler* handler, ImGuiTextBuffer* buf)
{
    Instance* instance = (Instance*)handler->UserData;

    // Write all saved camera states
    for (const auto& pair : instance->saved_render_settings)
    {
        const std::string& name = pair.first;
        const pnanovdb_imgui_settings_render_t& render_settings = pair.second;

        buf->appendf("[%s][%s]\n", handler->TypeName, name.c_str());
        buf->appendf("position=%f,%f,%f\n", render_settings.camera_state.position.x,
                     render_settings.camera_state.position.y, render_settings.camera_state.position.z);
        buf->appendf("eye_direction=%f,%f,%f\n", render_settings.camera_state.eye_direction.x,
                     render_settings.camera_state.eye_direction.y, render_settings.camera_state.eye_direction.z);
        buf->appendf("eye_up=%f,%f,%f\n", render_settings.camera_state.eye_up.x, render_settings.camera_state.eye_up.y,
                     render_settings.camera_state.eye_up.z);
        buf->appendf("eye_distance_from_position=%f\n", render_settings.camera_state.eye_distance_from_position);
        buf->appendf("orthographic_scale=%f\n", render_settings.camera_state.orthographic_scale);
        buf->appendf("vsync=%d\n", render_settings.vsync);
        buf->appendf("is_projection_rh=%d\n", render_settings.is_projection_rh);
        buf->appendf("is_orthographic=%d\n", render_settings.is_orthographic);
        buf->appendf("is_reverse_z=%d\n", render_settings.is_reverse_z);
        buf->appendf("is_y_up=%d\n", render_settings.is_y_up);
        buf->appendf("is_upside_down=%d\n", render_settings.is_upside_down);
        buf->appendf("camera_speed_multiplier=%f\n", render_settings.camera_speed_multiplier);
        // buf->appendf("ui_profile_name=%s\n", render_settings.ui_profile_name);
        buf->append("\n");
    }
}

static void Register(ImGuiContext* context, Instance* instance)
{
    ImGuiSettingsHandler render_settings_handler;
    render_settings_handler.TypeName = "RenderSettings";
    render_settings_handler.TypeHash = ImHashStr("RenderSettings");
    render_settings_handler.ClearAllFn = ClearAll;
    render_settings_handler.ReadOpenFn = ReadOpen;
    render_settings_handler.ReadLineFn = ReadLine;
    render_settings_handler.WriteAllFn = WriteAll;
    render_settings_handler.UserData = instance;

    context->SettingsHandlers.push_back(render_settings_handler);
}
} // namespace RenderSettingsHandler
} // namespace imgui_instance_user
