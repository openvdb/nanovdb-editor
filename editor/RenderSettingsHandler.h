// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/editor/RenderSettingsHandler.h

    \author Petra Hapalova

    \brief  This file contains the ImGui settings handler for editor render settings.
*/

#pragma once

#include "ImguiInstance.h"
#include "RenderSettingsConfig.h"

#include "nanovdb_editor/putil/Camera.h"

#include <imgui_internal.h>

#include <string>
#include <map>

namespace imgui_instance_user
{
namespace RenderSettingsHandler
{
// Field name constants for INI serialization
static const char* FIELD_VSYNC = "vsync";
static const char* FIELD_IS_PROJECTION_RH = "is_projection_rh";
static const char* FIELD_IS_ORTHOGRAPHIC = "is_orthographic";
static const char* FIELD_IS_REVERSE_Z = "is_reverse_z";
static const char* FIELD_IS_Y_UP = "is_y_up";
static const char* FIELD_IS_UPSIDE_DOWN = "is_upside_down";
static const char* FIELD_CAMERA_SPEED_MULTIPLIER = "camera_speed_multiplier";
static const char* FIELD_UI_PROFILE_NAME = "ui_profile_name";

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

    // Only read INI_PERSISTENT fields
    float x;
    int boolValue;
    char fmt[128];

    if (snprintf(fmt, sizeof(fmt), "%s=%%d", FIELD_VSYNC), sscanf(line, fmt, &boolValue) == 1)
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

        // Only save INI_PERSISTENT fields (user preferences)
        buf->appendf("%s=%d\n", FIELD_VSYNC, render_settings.vsync);
        buf->appendf("%s=%d\n", FIELD_IS_PROJECTION_RH, render_settings.is_projection_rh);
        buf->appendf("%s=%d\n", FIELD_IS_ORTHOGRAPHIC, render_settings.is_orthographic);
        buf->appendf("%s=%d\n", FIELD_IS_REVERSE_Z, render_settings.is_reverse_z);
        buf->appendf("%s=%d\n", FIELD_IS_Y_UP, render_settings.is_y_up);
        buf->appendf("%s=%d\n", FIELD_IS_UPSIDE_DOWN, render_settings.is_upside_down);
        buf->appendf("%s=%f\n", FIELD_CAMERA_SPEED_MULTIPLIER, render_settings.camera_speed_multiplier);
        buf->appendf("%s=%s\n", FIELD_UI_PROFILE_NAME, render_settings.ui_profile_name);
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
