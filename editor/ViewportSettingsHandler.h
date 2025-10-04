// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!\file   nanovdb_editor/editor/ViewportSettingsHandler.h
 * \author Petra Hapalova
 * \brief  ImGui settings handler for Viewport UI state.
 */

#pragma once

#include "ImguiInstance.h"

#include <imgui_internal.h>

namespace imgui_instance_user
{
namespace ViewportSettingsHandler
{
static void ClearAll(ImGuiContext* ctx, ImGuiSettingsHandler* handler)
{
    Instance* instance = (Instance*)handler->UserData;
    instance->selected_camera_frustum.clear();
}

static void* ReadOpen(ImGuiContext* ctx, ImGuiSettingsHandler* handler, const char* name)
{
    // Only one section for now: [ViewportSettings][Settings]
    if (strcmp(name, "Settings") == 0)
    {
        return (void*)name;
    }
    return nullptr;
}

static void ReadLine(ImGuiContext* ctx, ImGuiSettingsHandler* handler, void* entry, const char* line)
{
    const char* name = (const char*)entry;
    if (!name || strcmp(name, "Settings") != 0)
    {
        return;
    }

    Instance* instance = (Instance*)handler->UserData;

    char buffer[1024] = {};
    int indexValue = 0;
    if (sscanf(line, "SelectedCameraFrustum=%1023[^\n]", buffer) == 1)
    {
        instance->selected_camera_frustum = buffer;
    }
    else if (sscanf(line, "SelectedCameraIndex=%d", &indexValue) == 1)
    {
        if (!instance->selected_camera_frustum.empty())
        {
            instance->camera_frustum_index[instance->selected_camera_frustum] = indexValue;
        }
    }
}

static void WriteAll(ImGuiContext* ctx, ImGuiSettingsHandler* handler, ImGuiTextBuffer* buf)
{
    Instance* instance = (Instance*)handler->UserData;

    buf->appendf("[%s][Settings]\n", handler->TypeName);
    buf->appendf("SelectedCameraFrustum=%s\n", instance->selected_camera_frustum.c_str());
    int index = 0;
    if (!instance->selected_camera_frustum.empty())
    {
        auto it = instance->camera_frustum_index.find(instance->selected_camera_frustum);
        if (it != instance->camera_frustum_index.end())
        {
            index = it->second;
        }
    }
    buf->appendf("SelectedCameraIndex=%d\n", index);
    buf->append("\n");
}

static void Register(ImGuiContext* context, Instance* instance)
{
    ImGuiSettingsHandler handler;
    handler.TypeName = "ViewportSettings";
    handler.TypeHash = ImHashStr("ViewportSettings");
    handler.ClearAllFn = ClearAll;
    handler.ReadOpenFn = ReadOpen;
    handler.ReadLineFn = ReadLine;
    handler.WriteAllFn = WriteAll;
    handler.UserData = instance;

    context->SettingsHandlers.push_back(handler);
}
} // namespace ViewportSettingsHandler
} // namespace imgui_instance_user
