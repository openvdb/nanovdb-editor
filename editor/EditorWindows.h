// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/editor/EditorWindows.h

    \author Andrew Reidmeyer, Petra Hapalova

    \brief  Window rendering functions for the ImGui editor
*/

#pragma once

#include <string>

namespace imgui_instance_user
{
struct Instance;
} // namespace imgui_instance_user
namespace pnanovdb_editor
{
using namespace imgui_instance_user;

template <typename EditorSceneType>
bool load_scene_file_and_sync_viewport(EditorSceneType& editor_scene, const std::string& filepath)
{
    if (!editor_scene.load_scene_file(filepath))
        return false;
    editor_scene.sync_restored_viewport_camera();
    return true;
}

// Window rendering functions
void saveIniSettings(Instance* ptr);
void createMenu(Instance* ptr);
void showSceneWindow(Instance* ptr);
void showSceneParamsWindow(Instance* ptr);
void showCameraViews(Instance* ptr);
void showPropertiesWindow(Instance* ptr);
void showRenderSettingsWindow(Instance* ptr);
void showCompilerSettingsWindow(Instance* ptr);
void showShaderParamsWindow(Instance* ptr);
void showFileHeaderWindow(Instance* ptr);
void showCodeEditorWindow(Instance* ptr);
void showProfilerWindow(Instance* ptr, float delta_time);
void showConsoleWindow(Instance* ptr);
void showAboutWindow(Instance* ptr);

void showFileDialogs(Instance* ptr);

} // namespace pnanovdb_editor
