// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/editor/ViewportSettingsHandler.h

    \author Petra Hapalova

    \brief  ImGui settings handler for Viewport UI state
*/

#pragma once

#include <imgui/ImguiTLS.h>

#include "ImguiInstance.h"

#include <imgui_internal.h>

namespace imgui_instance_user
{
namespace ViewportSettingsHandler
{
static void ClearAll(ImGuiContext* ctx, ImGuiSettingsHandler* handler)
{
    Instance* instance = (Instance*)handler->UserData;
    instance->selected_scene_item.clear();
}

static void* ReadOpen(ImGuiContext* ctx, ImGuiSettingsHandler* handler, const char* name)
{
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
}

static void WriteAll(ImGuiContext* ctx, ImGuiSettingsHandler* handler, ImGuiTextBuffer* buf)
{
    Instance* instance = (Instance*)handler->UserData;

    buf->appendf("[%s][Settings]\n", handler->TypeName);
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
