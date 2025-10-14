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
// Field name constants for INI serialization
static const char* FIELD_POSITION = "position";
static const char* FIELD_EYE_DIRECTION = "eye_direction";
static const char* FIELD_EYE_UP = "eye_up";
static const char* FIELD_EYE_DISTANCE = "eye_distance_from_position";
static const char* FIELD_ORTHOGRAPHIC_SCALE = "orthographic_scale";
static const char* FIELD_VSYNC = "vsync";
static const char* FIELD_IS_PROJECTION_RH = "is_projection_rh";
static const char* FIELD_IS_ORTHOGRAPHIC = "is_orthographic";
static const char* FIELD_IS_REVERSE_Z = "is_reverse_z";
static const char* FIELD_IS_Y_UP = "is_y_up";
static const char* FIELD_IS_UPSIDE_DOWN = "is_upside_down";
static const char* FIELD_CAMERA_SPEED_MULTIPLIER = "camera_speed_multiplier";
static const char* FIELD_UI_PROFILE_NAME = "ui_profile_name";
static const char* FIELD_ENCODE_WIDTH = "encode_width";
static const char* FIELD_ENCODE_HEIGHT = "encode_height";

static void ClearAll(ImGuiContext* ctx, ImGuiSettingsHandler* handler)
{
    Instance* instance = (Instance*)handler->UserData;
    instance->saved_render_settings.clear();
}

static void* ReadOpen(ImGuiContext* ctx, ImGuiSettingsHandler* handler, const char* name)
{
    Instance* instance = (Instance*)handler->UserData;

    // name is the render settings after the [RenderSettings][name] header
    if (instance->saved_render_settings.find(name) == instance->saved_render_settings.end())
    {
        // not in settings yet, init with defaults
        auto& settings = instance->saved_render_settings[name];
        pnanovdb_camera_config_default(&settings.camera_config);
        pnanovdb_camera_state_default(&settings.camera_state, settings.is_y_up);
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
    char fmt[128];

    snprintf(fmt, sizeof(fmt), "%s=%%f,%%f,%%f", FIELD_POSITION);
    if (sscanf(line, fmt, &x, &y, &z) == 3)
    {
        instance->saved_render_settings[name].camera_state.position.x = x;
        instance->saved_render_settings[name].camera_state.position.y = y;
        instance->saved_render_settings[name].camera_state.position.z = z;
    }
    else if (snprintf(fmt, sizeof(fmt), "%s=%%f,%%f,%%f", FIELD_EYE_DIRECTION), sscanf(line, fmt, &x, &y, &z) == 3)
    {
        instance->saved_render_settings[name].camera_state.eye_direction.x = x;
        instance->saved_render_settings[name].camera_state.eye_direction.y = y;
        instance->saved_render_settings[name].camera_state.eye_direction.z = z;
    }
    else if (snprintf(fmt, sizeof(fmt), "%s=%%f,%%f,%%f", FIELD_EYE_UP), sscanf(line, fmt, &x, &y, &z) == 3)
    {
        instance->saved_render_settings[name].camera_state.eye_up.x = x;
        instance->saved_render_settings[name].camera_state.eye_up.y = y;
        instance->saved_render_settings[name].camera_state.eye_up.z = z;
    }
    else if (snprintf(fmt, sizeof(fmt), "%s=%%f", FIELD_EYE_DISTANCE), sscanf(line, fmt, &x) == 1)
    {
        instance->saved_render_settings[name].camera_state.eye_distance_from_position = x;
    }
    else if (snprintf(fmt, sizeof(fmt), "%s=%%f", FIELD_ORTHOGRAPHIC_SCALE), sscanf(line, fmt, &x) == 1)
    {
        instance->saved_render_settings[name].camera_state.orthographic_scale = x;
    }
    else if (snprintf(fmt, sizeof(fmt), "%s=%%d", FIELD_VSYNC), sscanf(line, fmt, &boolValue) == 1)
    {
        instance->saved_render_settings[name].vsync = (pnanovdb_bool_t)boolValue;
    }
    else if (snprintf(fmt, sizeof(fmt), "%s=%%d", FIELD_IS_PROJECTION_RH), sscanf(line, fmt, &boolValue) == 1)
    {
        instance->saved_render_settings[name].is_projection_rh = (pnanovdb_bool_t)boolValue;
        instance->saved_render_settings[name].camera_config.is_projection_rh = (pnanovdb_bool_t)boolValue;
    }
    else if (snprintf(fmt, sizeof(fmt), "%s=%%d", FIELD_IS_ORTHOGRAPHIC), sscanf(line, fmt, &boolValue) == 1)
    {
        instance->saved_render_settings[name].is_orthographic = (pnanovdb_bool_t)boolValue;
        instance->saved_render_settings[name].camera_config.is_orthographic = (pnanovdb_bool_t)boolValue;
    }
    else if (snprintf(fmt, sizeof(fmt), "%s=%%d", FIELD_IS_REVERSE_Z), sscanf(line, fmt, &boolValue) == 1)
    {
        instance->saved_render_settings[name].is_reverse_z = (pnanovdb_bool_t)boolValue;
        instance->saved_render_settings[name].camera_config.is_reverse_z = (pnanovdb_bool_t)boolValue;
    }
    else if (snprintf(fmt, sizeof(fmt), "%s=%%d", FIELD_IS_Y_UP), sscanf(line, fmt, &boolValue) == 1)
    {
        instance->saved_render_settings[name].is_y_up = (pnanovdb_bool_t)boolValue;
    }
    else if (snprintf(fmt, sizeof(fmt), "%s=%%d", FIELD_IS_UPSIDE_DOWN), sscanf(line, fmt, &boolValue) == 1)
    {
        instance->saved_render_settings[name].is_upside_down = (pnanovdb_bool_t)boolValue;
    }
    else if (snprintf(fmt, sizeof(fmt), "%s=%%f", FIELD_CAMERA_SPEED_MULTIPLIER), sscanf(line, fmt, &x) == 1)
    {
        instance->saved_render_settings[name].camera_speed_multiplier = x;
    }
    else if (snprintf(fmt, sizeof(fmt), "%s=%%255s", FIELD_UI_PROFILE_NAME),
             sscanf(line, fmt, instance->saved_render_settings[name].ui_profile_name) == 1)
    {
        // String is safely read with length limit, ensuring null termination
    }
    else if (snprintf(fmt, sizeof(fmt), "%s=%%d", FIELD_ENCODE_WIDTH), sscanf(line, fmt, &boolValue) == 1)
    {
        instance->saved_render_settings[name].encode_width = boolValue;
    }
    else if (snprintf(fmt, sizeof(fmt), "%s=%%d", FIELD_ENCODE_HEIGHT), sscanf(line, fmt, &boolValue) == 1)
    {
        instance->saved_render_settings[name].encode_height = boolValue;
    }
}

static void ApplyAll(ImGuiContext* ctx, ImGuiSettingsHandler* handler)
{
    Instance* instance = (Instance*)handler->UserData;

    // After all settings are loaded from INI, apply far plane based on reverse Z and orthographic settings
    for (auto& pair : instance->saved_render_settings)
    {
        auto& settings = pair.second;
        if (settings.camera_config.is_reverse_z && !settings.camera_config.is_orthographic)
        {
            settings.camera_config.far_plane = INFINITY;
        }
        else
        {
            settings.camera_config.far_plane = 10000.f;
        }
    }
}

static void WriteAll(ImGuiContext* ctx, ImGuiSettingsHandler* handler, ImGuiTextBuffer* buf)
{
    Instance* instance = (Instance*)handler->UserData;

    auto save_render_settings = [&](const std::string& name, const pnanovdb_imgui_settings_render_t& render_settings)
    {
        buf->appendf("[%s][%s]\n", handler->TypeName, name.c_str());
        buf->appendf("%s=%f,%f,%f\n", FIELD_POSITION, render_settings.camera_state.position.x,
                     render_settings.camera_state.position.y, render_settings.camera_state.position.z);
        buf->appendf("%s=%f,%f,%f\n", FIELD_EYE_DIRECTION, render_settings.camera_state.eye_direction.x,
                     render_settings.camera_state.eye_direction.y, render_settings.camera_state.eye_direction.z);
        buf->appendf("%s=%f,%f,%f\n", FIELD_EYE_UP, render_settings.camera_state.eye_up.x,
                     render_settings.camera_state.eye_up.y, render_settings.camera_state.eye_up.z);
        buf->appendf("%s=%f\n", FIELD_EYE_DISTANCE, render_settings.camera_state.eye_distance_from_position);
        buf->appendf("%s=%f\n", FIELD_ORTHOGRAPHIC_SCALE, render_settings.camera_state.orthographic_scale);
        buf->appendf("%s=%d\n", FIELD_VSYNC, render_settings.vsync);
        buf->appendf("%s=%d\n", FIELD_IS_PROJECTION_RH, render_settings.is_projection_rh);
        buf->appendf("%s=%d\n", FIELD_IS_ORTHOGRAPHIC, render_settings.is_orthographic);
        buf->appendf("%s=%d\n", FIELD_IS_REVERSE_Z, render_settings.is_reverse_z);
        buf->appendf("%s=%d\n", FIELD_IS_Y_UP, render_settings.is_y_up);
        buf->appendf("%s=%d\n", FIELD_IS_UPSIDE_DOWN, render_settings.is_upside_down);
        buf->appendf("%s=%f\n", FIELD_CAMERA_SPEED_MULTIPLIER, render_settings.camera_speed_multiplier);
        buf->appendf("%s=%s\n", FIELD_UI_PROFILE_NAME, render_settings.ui_profile_name);
        buf->appendf("%s=%d\n", FIELD_ENCODE_WIDTH, render_settings.encode_width);
        buf->appendf("%s=%d\n", FIELD_ENCODE_HEIGHT, render_settings.encode_height);
        buf->append("\n");
    };

    for (const auto& pair : instance->saved_render_settings)
    {
        save_render_settings(pair.first, pair.second);
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
    render_settings_handler.ApplyAllFn = ApplyAll;
    render_settings_handler.WriteAllFn = WriteAll;
    render_settings_handler.UserData = instance;

    context->SettingsHandlers.push_back(render_settings_handler);
}
} // namespace RenderSettingsHandler
} // namespace imgui_instance_user
