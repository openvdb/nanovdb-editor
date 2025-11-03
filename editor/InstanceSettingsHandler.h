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
#include "EditorToken.h"

namespace imgui_instance_user
{
namespace InstanceSettingsHandler
{
// Field name constants for INI serialization
static const char* FIELD_GROUP_NAME = "GroupName";
static const char* FIELD_SHADER_DIRECTORY = "ShaderDirectory";
static const char* FIELD_SELECTED_RENDER_SETTINGS_NAME = "SelectedRenderSettingsName";
static const char* FIELD_WINDOW_WIDTH = "WindowWidth";
static const char* FIELD_WINDOW_HEIGHT = "WindowHeight";
static const char* FIELD_SHOW_PROFILER = "ShowProfiler";
static const char* FIELD_SHOW_CODE_EDITOR = "ShowCodeEditor";
static const char* FIELD_SHOW_CONSOLE = "ShowConsole";
static const char* FIELD_SHOW_VIEWPORT_SETTINGS = "ShowViewportSettings";
static const char* FIELD_SHOW_RENDER_SETTINGS = "ShowRenderSettings";
static const char* FIELD_SHOW_COMPILER_SETTINGS = "ShowCompilerSettings";
static const char* FIELD_SHOW_SHADER_PARAMS = "ShowShaderParams";
static const char* FIELD_SHOW_BENCHMARK = "ShowBenchmark";
static const char* FIELD_SHOW_FILE_HEADER = "ShowFileHeader";
static const char* FIELD_SHOW_SCENE = "ShowScene";
static const char* FIELD_SHOW_SCENE_PROPERTIES = "ShowSceneProperties";
static const char* FIELD_SHOW_ABOUT = "ShowAbout";

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
        char fmt[128];

        snprintf(fmt, sizeof(fmt), "%s=%%[^\n]", FIELD_GROUP_NAME);
        if (sscanf(line, fmt, buffer) == 1)
        {
            instance->shader_group = buffer;
        }
        else if (snprintf(fmt, sizeof(fmt), "%s=%%[^\n]", FIELD_SHADER_DIRECTORY), sscanf(line, fmt, buffer) == 1)
        {
            instance->additional_shader_directories.push_back(buffer);
        }
        else if (snprintf(fmt, sizeof(fmt), "%s=%%[^\n]", FIELD_SELECTED_RENDER_SETTINGS_NAME),
                 sscanf(line, fmt, buffer) == 1)
        {
            instance->render_settings_name = buffer;
            instance->viewport_settings[(int)instance->viewport_option].render_settings_name =
                instance->render_settings_name;
            if (!instance->is_viewer())
            {
                // Load camera state when profile is loaded from INI
                if (instance->editor_scene)
                {
                    pnanovdb_editor_token_t* name_token =
                        pnanovdb_editor::EditorToken::getInstance().getToken(instance->render_settings_name.c_str());
                    const pnanovdb_camera_state_t* state = instance->editor_scene->get_saved_camera_state(name_token);
                    if (state)
                    {
                        instance->render_settings->camera_state = *state;
                        instance->render_settings->sync_camera = PNANOVDB_TRUE;
                    }
                }
            }
        }
        else
        {
            int value = 0;
            if (snprintf(fmt, sizeof(fmt), "%s=%%d", FIELD_WINDOW_WIDTH), sscanf(line, fmt, &value) == 1)
            {
                instance->ini_window_width = value;
            }
            else if (snprintf(fmt, sizeof(fmt), "%s=%%d", FIELD_WINDOW_HEIGHT), sscanf(line, fmt, &value) == 1)
            {
                instance->ini_window_height = value;
            }
            else if (snprintf(fmt, sizeof(fmt), "%s=%%d", FIELD_SHOW_PROFILER), sscanf(line, fmt, &value) == 1)
            {
                instance->window.show_profiler = (value != 0);
            }
            else if (snprintf(fmt, sizeof(fmt), "%s=%%d", FIELD_SHOW_CODE_EDITOR), sscanf(line, fmt, &value) == 1)
            {
                instance->window.show_code_editor = (value != 0);
            }
            else if (snprintf(fmt, sizeof(fmt), "%s=%%d", FIELD_SHOW_CONSOLE), sscanf(line, fmt, &value) == 1)
            {
                instance->window.show_console = (value != 0);
            }
            else if (snprintf(fmt, sizeof(fmt), "%s=%%d", FIELD_SHOW_VIEWPORT_SETTINGS), sscanf(line, fmt, &value) == 1)
            {
                instance->window.show_viewport_settings = (value != 0);
            }
            else if (snprintf(fmt, sizeof(fmt), "%s=%%d", FIELD_SHOW_RENDER_SETTINGS), sscanf(line, fmt, &value) == 1)
            {
                instance->window.show_render_settings = (value != 0);
            }
            else if (snprintf(fmt, sizeof(fmt), "%s=%%d", FIELD_SHOW_COMPILER_SETTINGS), sscanf(line, fmt, &value) == 1)
            {
                instance->window.show_compiler_settings = (value != 0);
            }
            else if (snprintf(fmt, sizeof(fmt), "%s=%%d", FIELD_SHOW_SHADER_PARAMS), sscanf(line, fmt, &value) == 1)
            {
                instance->window.show_shader_params = (value != 0);
            }
            else if (snprintf(fmt, sizeof(fmt), "%s=%%d", FIELD_SHOW_BENCHMARK), sscanf(line, fmt, &value) == 1)
            {
                instance->window.show_benchmark = (value != 0);
            }
            else if (snprintf(fmt, sizeof(fmt), "%s=%%d", FIELD_SHOW_FILE_HEADER), sscanf(line, fmt, &value) == 1)
            {
                instance->window.show_file_header = (value != 0);
            }
            else if (snprintf(fmt, sizeof(fmt), "%s=%%d", FIELD_SHOW_SCENE), sscanf(line, fmt, &value) == 1)
            {
                instance->window.show_scene = (value != 0);
            }
            else if (snprintf(fmt, sizeof(fmt), "%s=%%d", FIELD_SHOW_SCENE_PROPERTIES), sscanf(line, fmt, &value) == 1)
            {
                instance->window.show_scene_properties = (value != 0);
            }
            else if (snprintf(fmt, sizeof(fmt), "%s=%%d", FIELD_SHOW_ABOUT), sscanf(line, fmt, &value) == 1)
            {
                instance->window.show_about = (value != 0);
            }
        }
    }
}

static void WriteAll(ImGuiContext* ctx, ImGuiSettingsHandler* handler, ImGuiTextBuffer* buf)
{
    Instance* instance = (Instance*)handler->UserData;

    buf->appendf("[%s][Settings]\n", handler->TypeName);
    buf->appendf("%s=%s\n", FIELD_GROUP_NAME, instance->shader_group.c_str());
    buf->appendf("%s=%s\n", FIELD_SELECTED_RENDER_SETTINGS_NAME, instance->render_settings_name.c_str());

    // Persist window resolution
    if (instance->ini_window_width > 0 && instance->ini_window_height > 0)
    {
        buf->appendf("%s=%d\n", FIELD_WINDOW_WIDTH, instance->ini_window_width);
        buf->appendf("%s=%d\n", FIELD_WINDOW_HEIGHT, instance->ini_window_height);
    }

    // Persist window visibility flags
    buf->appendf("%s=%d\n", FIELD_SHOW_PROFILER, instance->window.show_profiler ? 1 : 0);
    buf->appendf("%s=%d\n", FIELD_SHOW_CODE_EDITOR, instance->window.show_code_editor ? 1 : 0);
    buf->appendf("%s=%d\n", FIELD_SHOW_CONSOLE, instance->window.show_console ? 1 : 0);
    buf->appendf("%s=%d\n", FIELD_SHOW_VIEWPORT_SETTINGS, instance->window.show_viewport_settings ? 1 : 0);
    buf->appendf("%s=%d\n", FIELD_SHOW_RENDER_SETTINGS, instance->window.show_render_settings ? 1 : 0);
    buf->appendf("%s=%d\n", FIELD_SHOW_COMPILER_SETTINGS, instance->window.show_compiler_settings ? 1 : 0);
    buf->appendf("%s=%d\n", FIELD_SHOW_SHADER_PARAMS, instance->window.show_shader_params ? 1 : 0);
    buf->appendf("%s=%d\n", FIELD_SHOW_BENCHMARK, instance->window.show_benchmark ? 1 : 0);
    buf->appendf("%s=%d\n", FIELD_SHOW_FILE_HEADER, instance->window.show_file_header ? 1 : 0);
    buf->appendf("%s=%d\n", FIELD_SHOW_SCENE, instance->window.show_scene ? 1 : 0);
    buf->appendf("%s=%d\n", FIELD_SHOW_SCENE_PROPERTIES, instance->window.show_scene_properties ? 1 : 0);
    buf->appendf("%s=%d\n", FIELD_SHOW_ABOUT, instance->window.show_about ? 1 : 0);

    for (const auto& directory : instance->additional_shader_directories)
    {
        buf->appendf("%s=%s\n", FIELD_SHADER_DIRECTORY, directory.c_str());
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
