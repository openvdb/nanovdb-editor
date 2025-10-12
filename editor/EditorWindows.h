// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/editor/EditorWindows.h

    \author Andrew Reidmeyer, Petra Hapalova

    \brief  Window rendering functions for the ImGui editor
*/

#pragma once

namespace imgui_instance_user
{
    struct Instance;
}

namespace pnanovdb_editor
{

// Window rendering functions
void showSceneWindow(imgui_instance_user::Instance* ptr);
void showCameraViews(imgui_instance_user::Instance* ptr);
void showPropertiesWindow(imgui_instance_user::Instance* ptr);
void showViewportSettingsWindow(imgui_instance_user::Instance* ptr);
void showRenderSettingsWindow(imgui_instance_user::Instance* ptr);
void showCompilerSettingsWindow(imgui_instance_user::Instance* ptr);
void showShaderParamsWindow(imgui_instance_user::Instance* ptr);
void showBenchmarkWindow(imgui_instance_user::Instance* ptr);
void showFileHeaderWindow(imgui_instance_user::Instance* ptr);
void showCodeEditorWindow(imgui_instance_user::Instance* ptr);
void showProfilerWindow(imgui_instance_user::Instance* ptr, float delta_time);
void showConsoleWindow(imgui_instance_user::Instance* ptr);

} // namespace pnanovdb_editor
