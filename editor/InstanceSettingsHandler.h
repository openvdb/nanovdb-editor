// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/editor/InstanceSettingsHandler.h

    \author Petra Hapalova

    \brief  This file contains the ImGui settings handler for editor instance settings.
*/

#pragma once

#include "ImguiInstance.h"
#include "Console.h"
#include "ShaderMonitor.h"
#include "ShaderCompileUtils.h"

namespace imgui_instance_user
{
namespace InstanceSettingsHandler
{
static void ClearAll(ImGuiContext* ctx, ImGuiSettingsHandler* handler)
{
    Instance* instance = (Instance*)handler->UserData;
    instance->shader_group = "";
    instance->additional_shader_directories.clear();
}

static void* ReadOpen(ImGuiContext* ctx, ImGuiSettingsHandler* handler, const char* name)
{
    // name is "Settings" for the main settings section
    if (strcmp(name, "Settings") == 0)
    {
        return (void*)name;
    }
    return nullptr;
}

static void ReadLine(ImGuiContext* ctx, ImGuiSettingsHandler* handler, void* entry, const char* line)
{
    const char* name = (const char*)entry;
    Instance* instance = (Instance*)handler->UserData;

    if (name && strcmp(name, "Settings") == 0)
    {
        char buffer[1024] = {};
        if (sscanf(line, "GroupName=%[^\n]", buffer) == 1)
        {
            instance->shader_group = buffer;
        }
        else if (sscanf(line, "ShaderDirectory=%[^\n]", buffer) == 1)
        {
            instance->additional_shader_directories.push_back(buffer);
        }
        else if (sscanf(line, "SelectedRenderSettingsName=%[^\n]", buffer) == 1)
        {
            instance->render_settings_name = buffer;
            instance->viewport_settings[(int)instance->viewport_option].render_settings_name =
                instance->render_settings_name;
            instance->pending.load_camera = true;
        }
    }
}

static void WriteAll(ImGuiContext* ctx, ImGuiSettingsHandler* handler, ImGuiTextBuffer* buf)
{
    Instance* instance = (Instance*)handler->UserData;

    buf->appendf("[%s][Settings]\n", handler->TypeName);
    buf->appendf("GroupName=%s\n", instance->shader_group.c_str());
    buf->appendf("SelectedRenderSettingsName=%s\n", instance->render_settings_name.c_str());

    for (const auto& directory : instance->additional_shader_directories)
    {
        buf->appendf("ShaderDirectory=%s\n", directory.c_str());
    }

    buf->append("\n");
}

static void ApplyAll(ImGuiContext* ctx, ImGuiSettingsHandler* handler)
{
    Instance* instance = (Instance*)handler->UserData;

    for (const auto& directory : instance->additional_shader_directories)
    {
        auto callback = pnanovdb_editor::get_shader_recompile_callback(instance, instance->compiler);

        pnanovdb_editor::monitor_shader_dir(directory.c_str(), callback);
        pnanovdb_editor::Console::getInstance().addLog("Restored monitoring for shader directory: %s", directory.c_str());
    }
}

static void Register(ImGuiContext* context, Instance* instance)
{
    ImGuiSettingsHandler instance_handler;
    instance_handler.TypeName = "InstanceSettings";
    instance_handler.TypeHash = ImHashStr("InstanceSettings");
    instance_handler.ClearAllFn = ClearAll;
    instance_handler.ReadOpenFn = ReadOpen;
    instance_handler.ReadLineFn = ReadLine;
    instance_handler.WriteAllFn = WriteAll;
    instance_handler.ApplyAllFn = ApplyAll;
    instance_handler.UserData = instance;

    context->SettingsHandlers.push_back(instance_handler);
}
} // namespace InstanceSettingsHandler
}
