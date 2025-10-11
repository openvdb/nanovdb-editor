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

// Window rendering functions
void showSceneWindow(Instance* ptr);
void showCameraViews(Instance* ptr);
void showPropertiesWindow(Instance* ptr);
void showViewportSettingsWindow(Instance* ptr);
void showRenderSettingsWindow(Instance* ptr);
void showCompilerSettingsWindow(Instance* ptr);
void showShaderParamsWindow(Instance* ptr);
void showBenchmarkWindow(Instance* ptr);
void showFileHeaderWindow(Instance* ptr);
void showCodeEditorWindow(Instance* ptr);
void showProfilerWindow(Instance* ptr, float delta_time);
void showConsoleWindow(Instance* ptr);
void showAboutWindow(Instance* ptr);

void showFileDialogs(Instance* ptr);
void saveLoadSettings(Instance* ptr);

} // namespace imgui_instance_user
