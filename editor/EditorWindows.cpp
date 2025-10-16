// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/editor/EditorWindows.h

    \author Andrew Reidmeyer, Petra Hapalova

    \brief  Window rendering functions for the ImGui editor
*/

#include "EditorWindows.h"

#include "ImguiInstance.h"
#include "CodeEditor.h"
#include "Console.h"
#include "Profiler.h"
#include "FileHeaderInfo.h"
#include "RenderSettingsHandler.h"
#include "InstanceSettingsHandler.h"
#include "ShaderMonitor.h"
#include "ShaderCompileUtils.h"
#include "Node2Verify.h"
#include "SceneTree.h"
#include "Properties.h"

#include "misc/cpp/imgui_stdlib.h" // for std::string text input

#include <ImGuiFileDialog.h>

#include <cmath>
#include <filesystem>

#ifndef M_PI_2
#    define M_PI_2 1.57079632679489661923
#endif

namespace pnanovdb_editor
{
const float EPSILON = 1e-6f;

void showSceneWindow(imgui_instance_user::Instance* ptr)
{
    if (!ptr->window.show_scene)
    {
        return;
    }

    SceneTree::getInstance().render(ptr);
}

void showPropertiesWindow(imgui_instance_user::Instance* ptr)
{
    if (!ptr->window.show_scene_properties)
    {
        return;
    }

    Properties::getInstance().render(ptr);
}

void showViewportSettingsWindow(imgui_instance_user::Instance* ptr)
{
    if (!ptr->window.show_viewport_settings)
    {
        return;
    }

    if (ImGui::Begin(VIEWPORT_SETTINGS, &ptr->window.show_viewport_settings))
    {
        // viewport options
        {
            ImGui::BeginGroup();
            ViewportOption selectedOption = ptr->viewport_option;
            if (ImGui::RadioButton("NanoVDB", selectedOption == ViewportOption::NanoVDB))
            {
                ptr->viewport_option = ViewportOption::NanoVDB;
                ptr->render_settings_name = ptr->viewport_settings[(int)ptr->viewport_option].render_settings_name;
                if (!ptr->is_viewer())
                {
                    // Load camera state
                    auto it = ptr->saved_camera_states.find(ptr->render_settings_name);
                    if (it != ptr->saved_camera_states.end())
                    {
                        ptr->render_settings->camera_state = it->second;
                        ptr->render_settings->sync_camera = PNANOVDB_TRUE;
                    }
                }
            }
            ImGui::SameLine();
            if (ImGui::RadioButton("Raster2D", selectedOption == ViewportOption::Raster2D))
            {
                ptr->viewport_option = ViewportOption::Raster2D;
                ptr->render_settings_name = ptr->viewport_settings[(int)ptr->viewport_option].render_settings_name;
                if (!ptr->is_viewer())
                {
                    // Load camera state
                    auto it = ptr->saved_camera_states.find(ptr->render_settings_name);
                    if (it != ptr->saved_camera_states.end())
                    {
                        ptr->render_settings->camera_state = it->second;
                        ptr->render_settings->sync_camera = PNANOVDB_TRUE;
                    }
                }
            }
            ImGui::EndGroup();
        }
        {
            ImGui::BeginGroup();
            if (ImGui::BeginCombo("Viewport Camera", "Select..."))
            {
                for (const auto& pair : ptr->saved_render_settings)
                {
                    bool is_selected = (ptr->render_settings_name == pair.first);
                    if (ImGui::Selectable(pair.first.c_str(), is_selected))
                    {
                        ptr->render_settings_name = pair.first;
                        ptr->viewport_settings[(int)ptr->viewport_option].render_settings_name =
                            ptr->render_settings_name;

                        auto it = ptr->saved_camera_states.find(ptr->render_settings_name);
                        if (it != ptr->saved_camera_states.end())
                        {
                            ptr->render_settings->camera_state = it->second;
                            ptr->render_settings->sync_camera = PNANOVDB_TRUE;
                        }

                        // TODO: clear selected debug camera when switching to saved camera

                        ImGui::MarkIniSettingsDirty();
                    }
                    if (is_selected)
                    {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::InputText("##render_settings_name", &ptr->render_settings_name);
            ImGui::SameLine();
            if (ImGui::Button("Save"))
            {
                if (!ptr->render_settings_name.empty())
                {
                    if (ptr->saved_render_settings.find(ptr->render_settings_name) == ptr->saved_render_settings.end())
                    {
                        ptr->saved_camera_states[ptr->render_settings_name] = ptr->render_settings->camera_state;

                        imgui_instance_user::copyPersistentFields(
                            ptr->saved_render_settings[ptr->render_settings_name], *ptr->render_settings);

                        ImGui::MarkIniSettingsDirty();

                        pnanovdb_editor::Console::getInstance().addLog(
                            "Render settings '%s' saved", ptr->render_settings_name.c_str());
                    }
                    else
                    {
                        pnanovdb_editor::Console::getInstance().addLog(
                            "Render settings '%s' already exists", ptr->render_settings_name.c_str());
                    }
                }
                else
                {
                    pnanovdb_editor::Console::getInstance().addLog("Please enter a name for the render settings");
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Remove"))
            {
                if (ptr->saved_render_settings.erase(ptr->render_settings_name))
                {
                    pnanovdb_editor::Console::getInstance().addLog(
                        "Render settings '%s' removed", ptr->render_settings_name.c_str());
                    ptr->render_settings_name = "";
                }
                else
                {
                    pnanovdb_editor::Console::getInstance().addLog(
                        "Render settings '%s' not found", ptr->render_settings_name.c_str());
                }
            }
            ImGui::EndGroup();
        }
        ImGui::Separator();

        ImGui::Text("Gaussian File");
        {
            ImGui::BeginGroup();
            ImGui::InputText("##viewport_raster_file", &ptr->raster_filepath);
            ImGui::SameLine();
            if (ImGui::Button("...##open_raster_file"))
            {
                ptr->pending.find_raster_file = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("Show##Gaussian"))
            {
                // runs rasterization on a worker thread first
                ptr->pending.update_raster = true;
            }
            if (ptr->viewport_option == ViewportOption::NanoVDB)
            {
                ImGui::InputFloat("VoxelsPerUnit", &ptr->raster_voxels_per_unit);
            }
            ImGui::EndGroup();
        }

        if (ptr->viewport_option == ViewportOption::NanoVDB)
        {
            ImGui::Text("NanoVDB File");
            {
                ImGui::BeginGroup();
                ImGui::InputText("##viewport_nanovdb_file", &ptr->nanovdb_filepath);
                ImGui::SameLine();
                if (ImGui::Button("...##open_nanovddb_file"))
                {
                    ptr->pending.open_file = true;
                }
                ImGui::SameLine();
                if (ImGui::Button("Show##NanoVDB"))
                {
                    // TODO: does it need to be on a worker thread?
                    pnanovdb_editor::Console::getInstance().addLog("Opening file '%s'", ptr->nanovdb_filepath.c_str());
                    ptr->pending.load_nvdb = true;
                }
                ImGui::EndGroup();
            }
        }
        ImGui::Separator();

        // UI profile
        {
            const char* profile_options[] = { "default", "viewer" }; // TODO: load from current dir
            const char* current_profile =
                ptr->render_settings->ui_profile_name[0] != '\0' ? ptr->render_settings->ui_profile_name : "default";
            if (ImGui::BeginCombo("UI Profile", current_profile))
            {
                for (int i = 0; i < IM_ARRAYSIZE(profile_options); i++)
                {
                    bool is_selected = (strcmp(current_profile, profile_options[i]) == 0);
                    if (ImGui::Selectable(profile_options[i], is_selected))
                    {
                        strcpy(ptr->render_settings->ui_profile_name, profile_options[i]);
                    }
                    if (is_selected)
                    {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }
        }
    }
    ImGui::End();
}

void showRenderSettingsWindow(imgui_instance_user::Instance* ptr)
{
    if (!ptr->window.show_render_settings)
    {
        return;
    }

    if (ImGui::Begin(RENDER_SETTINGS, &ptr->window.show_render_settings))
    {
        ImGui::Text("Viewport Shader");
        {
            ImGui::BeginGroup();
            if (ImGui::BeginCombo("##viewport_shader", "Select..."))
            {
                for (const auto& shader : ptr->viewport_shaders)
                {
                    if (ImGui::Selectable(shader.c_str()))
                    {
                        ptr->pending.viewport_shader_name = shader;
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::InputText("##viewport_shader_name", &ptr->pending.viewport_shader_name);
            ImGui::SameLine();
            if (ImGui::Button("Update"))
            {
                pnanovdb_editor::CodeEditor::getInstance().setSelectedShader(ptr->pending.viewport_shader_name);
                ptr->shader_name = ptr->pending.viewport_shader_name;
                ptr->pending.update_shader = true;
            }
            ImGui::EndGroup();
        }
        ImGui::Separator();

        auto settings = ptr->render_settings;

        IMGUI_CHECKBOX_SYNC("VSync", settings->vsync);
        IMGUI_CHECKBOX_SYNC("Projection RH", settings->is_projection_rh);
        IMGUI_CHECKBOX_SYNC("Orthographic", settings->is_orthographic);
        IMGUI_CHECKBOX_SYNC("Reverse Z", settings->is_reverse_z);
        {
            int up_axis = settings->is_y_up ? 0 : 1;
            const char* up_axis_items[] = { "Y", "Z" };
            if (ImGui::Combo("Up Axis", &up_axis, up_axis_items, IM_ARRAYSIZE(up_axis_items)))
            {
                settings->is_y_up = (up_axis == 0);
            }
        }
        IMGUI_CHECKBOX_SYNC("Upside Down", settings->is_upside_down);
        ImGui::DragFloat("Camera Speed Multiplier", &settings->camera_speed_multiplier, 0.f, 1.f, 10000.f, "%.1f",
                         ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp);
        ImGui::Separator();
        if (settings->enable_encoder)
        {
            ImGui::AlignTextToFramePadding();
            ImGui::Text("Video encoding is running...");
        }
        else
        {
            IMGUI_CHECKBOX_SYNC("Video Encode", settings->enable_encoder);
        }
        if (ImGui::BeginCombo("Resolution", "Select..."))
        {
            const char* labels[4] = { "1440x720", "1920x1080", "2560x1440", "3840x2160" };
            const int widths[4] = { 1440, 1920, 2560, 3840 };
            const int heights[4] = { 720, 1080, 1440, 2160 };
            for (int i = 0; i < 4; i++)
            {
                if (ImGui::Selectable(labels[i]))
                {
                    settings->encode_width = widths[i];
                    settings->encode_height = heights[i];
                }
            }
            ImGui::EndCombo();
        }
        IMGUI_CHECKBOX_SYNC("Fit Resolution", settings->encode_resize);
        IMGUI_CHECKBOX_SYNC("Video Record", settings->encode_to_file);
        ImGui::InputText("Server Address", settings->server_address, 256u);
        ImGui::InputInt("Server Port", &settings->server_port);
    }
    ImGui::End();
}

void showCompilerSettingsWindow(imgui_instance_user::Instance* ptr)
{
    if (!ptr->window.show_compiler_settings)
    {
        return;
    }

    if (ImGui::Begin(COMPILER_SETTINGS, &ptr->window.show_compiler_settings))
    {
        std::lock_guard<std::mutex> lock(ptr->compiler_settings_mutex);

        IMGUI_CHECKBOX_SYNC("Row Major Matrix Layout", ptr->compiler_settings.is_row_major);
        IMGUI_CHECKBOX_SYNC("Create HLSL Output", ptr->compiler_settings.hlsl_output);
        // TODO: add downstream compilers dropdown
        IMGUI_CHECKBOX_SYNC("Use glslang", ptr->compiler_settings.use_glslang);

        const char* compile_targets[] = { "Select...", "Vulkan", "CPU" };
        int compile_target = (int)ptr->compiler_settings.compile_target;
        if (ImGui::Combo("Target", &compile_target, compile_targets, IM_ARRAYSIZE(compile_targets)))
        {
            ptr->compiler_settings.compile_target = (pnanovdb_compile_target_type_t)(compile_target);
            // Clear entry point name, default is assigned in compiler
            ptr->compiler_settings.entry_point_name[0] = '\0';
        }
        ImGui::InputText("Entry Point Name", ptr->compiler_settings.entry_point_name,
                         IM_ARRAYSIZE(ptr->compiler_settings.entry_point_name));
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Empty uses default");
        }

        ImGui::Separator();
        ImGui::Text("Additional Shader Directories");
        if (!ptr->additional_shader_directories.empty())
        {
            for (size_t i = 0; i < ptr->additional_shader_directories.size(); i++)
            {
                ImGui::PushID(static_cast<int>(i));
                ImGui::Text("  %s", ptr->additional_shader_directories[i].c_str());
                ImGui::SameLine();
                if (ImGui::Button("-"))
                {
                    std::string dirToRemove = ptr->additional_shader_directories[i];
                    pnanovdb_editor::ShaderMonitor::getInstance().removePath(dirToRemove);
                    ptr->additional_shader_directories.erase(ptr->additional_shader_directories.begin() + i);
                    pnanovdb_editor::Console::getInstance().addLog("Removed shader directory: %s", dirToRemove.c_str());
                    ImGui::MarkIniSettingsDirty();
                    i--;
                }
                ImGui::PopID();
            }
        }

        ImGui::InputText("##shader_directory", &ptr->pending_shader_directory);
        ImGui::SameLine();
        if (ImGui::Button("...##select_shader_directory"))
        {
            ptr->pending.find_shader_directory = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("+"))
        {
            if (!ptr->pending_shader_directory.empty())
            {
                std::filesystem::path dirPath(ptr->pending_shader_directory);
                if (std::filesystem::exists(dirPath) && std::filesystem::is_directory(dirPath))
                {
                    auto it = std::find(ptr->additional_shader_directories.begin(),
                                        ptr->additional_shader_directories.end(), ptr->pending_shader_directory);
                    if (it == ptr->additional_shader_directories.end())
                    {
                        ptr->additional_shader_directories.push_back(ptr->pending_shader_directory);

                        auto callback = pnanovdb_editor::get_shader_recompile_callback(ptr, ptr->compiler);

                        pnanovdb_editor::monitor_shader_dir(ptr->pending_shader_directory.c_str(), callback);
                        pnanovdb_editor::Console::getInstance().addLog(
                            "Added shader directory: %s", ptr->pending_shader_directory.c_str());
                        ImGui::MarkIniSettingsDirty();
                    }
                    else
                    {
                        pnanovdb_editor::Console::getInstance().addLog(
                            "Directory already being monitored: %s", ptr->pending_shader_directory.c_str());
                    }
                    ptr->pending_shader_directory.clear();
                }
                else
                {
                    pnanovdb_editor::Console::getInstance().addLog(
                        "Invalid directory path: %s", ptr->pending_shader_directory.c_str());
                }
            }
        }

        const std::string compiledShadersDir = pnanovdb_shader::getShaderCacheDir();
        if (!compiledShadersDir.empty())
        {
            ImGui::Text("Shader Cache");
            ImGui::Text("  %s", compiledShadersDir.c_str());
            if (ImGui::Button("Clear"))
            {
                std::filesystem::path shaderDir(compiledShadersDir);
                if (std::filesystem::exists(shaderDir) && std::filesystem::is_directory(shaderDir))
                {
                    for (const auto& entry : std::filesystem::directory_iterator(shaderDir))
                    {
                        std::filesystem::remove_all(entry.path());
                    }
                    pnanovdb_editor::Console::getInstance().addLog("Shader cache cleared");
                }
            }
        }
    }
    ImGui::End();
}

void showShaderParamsWindow(imgui_instance_user::Instance* ptr)
{
    if (!ptr->window.show_shader_params)
    {
        return;
    }

    if (ImGui::Begin(SHADER_PARAMS, &ptr->window.show_shader_params))
    {
        std::string shader_name;

        ImGui::BeginGroup();

        // Show params which are parsed from the shader used in current vieweport
        if (ImGui::RadioButton(
                "Viewport Shader", ptr->pending.shader_selection_mode == ShaderSelectionMode::UseViewportShader))
        {
            ptr->pending.shader_selection_mode = ShaderSelectionMode::UseViewportShader;
        }

        // Show params which are parsed from the shader opened in the code editor
        if (ImGui::RadioButton("Shader Editor Selected:",
                               ptr->pending.shader_selection_mode == ShaderSelectionMode::UseCodeEditorShader))
        {
            ptr->pending.shader_selection_mode = ShaderSelectionMode::UseCodeEditorShader;
        }
        ImGui::SameLine();
        shader_name = pnanovdb_editor::CodeEditor::getInstance().getSelectedShader().c_str();
        // strip the directory, show only the filename
        std::filesystem::path fsPath(shader_name);
        ImGui::Text("%s", fsPath.filename().string().c_str());

        // Show params for a specific shader group
        // TODO: This will belong to the Pipeline window
        bool is_group_mode = ptr->pending.shader_selection_mode == ShaderSelectionMode::UseShaderGroup;
        if (ImGui::RadioButton("Shader Group", is_group_mode))
        {
            ptr->pending.shader_selection_mode = ShaderSelectionMode::UseShaderGroup;
        }
        ImGui::SameLine();
        if (ImGui::InputText("##shader_group", &ptr->shader_group))
        {
            ImGui::MarkIniSettingsDirty();
        }
        ImGui::EndGroup();

        const std::string& target_name = is_group_mode ? ptr->shader_group : shader_name;
        if (!target_name.empty())
        {
            if (ptr->shader_params.isJsonLoaded(target_name, is_group_mode))
            {
                if (ImGui::Button("Reload JSON"))
                {
                    if (is_group_mode)
                    {
                        if (ptr->shader_params.loadGroup(target_name, true))
                        {
                            pnanovdb_editor::Console::getInstance().addLog(
                                "Group params '%s' updated", target_name.c_str());
                        }
                        else
                        {
                            pnanovdb_editor::Console::getInstance().addLog(
                                "Failed to reload group params '%s'", target_name.c_str());
                        }
                    }
                    else
                    {
                        pnanovdb_editor::CodeEditor::getInstance().saveShaderParams();
                        if (ptr->shader_params.load(target_name, true))
                        {
                            pnanovdb_editor::Console::getInstance().addLog(
                                "Shader params for '%s' updated", target_name.c_str());
                        }
                        else
                        {
                            pnanovdb_editor::Console::getInstance().addLog(
                                "Failed to reload shader params for '%s'", target_name.c_str());
                        }
                    }
                }
            }
            else
            {
                if (ImGui::Button("Create JSON"))
                {
                    if (ptr->pending.shader_selection_mode == ShaderSelectionMode::UseShaderGroup)
                    {
                        ptr->shader_params.createGroup(target_name);
                    }
                    else
                    {
                        ptr->shader_params.create(target_name);
                        pnanovdb_editor::CodeEditor::getInstance().updateViewer();
                    }
                }
            }
        }

        ImGui::Separator();

        if (!target_name.empty())
        {
            if (ptr->pending.shader_selection_mode == ShaderSelectionMode::UseShaderGroup)
            {
                ptr->shader_params.renderGroup(target_name);
            }
            else
            {
                if (ptr->shader_params.isJsonLoaded(target_name))
                {
                    ptr->shader_params.render(target_name);
                }
                else
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(191, 97, 106, 255)); // Red color
                    ImGui::TextWrapped("Shader params JSON is missing");
                    ImGui::PopStyleColor();
                }
            }
        }
        else
        {
            ImGui::TextDisabled("No shader selected");
        }
    }
    ImGui::End();
}

void showBenchmarkWindow(imgui_instance_user::Instance* ptr)
{
    if (!ptr->window.show_benchmark)
        return;

    if (ImGui::Begin(BENCHMARK, &ptr->window.show_benchmark))
    {
        if (ImGui::Button("Run Benchmark"))
        {
            // For now, guess the ref nvdb by convention
            std::string ref_nvdb = ptr->nanovdb_filepath;
            std::string suffix = "_node2.nvdb";
            if (ref_nvdb.size() > suffix.size())
            {
                ref_nvdb.erase(ref_nvdb.size() - suffix.size(), suffix.size());
            }
            ref_nvdb.append(".nvdb");
            printf("nanovdbFile_(%s) ref_nvdb(%s) suffix(%s)\n", ptr->nanovdb_filepath.c_str(), ref_nvdb.c_str(),
                   suffix.c_str());
            pnanovdb_editor::node2_verify_gpu(ref_nvdb.c_str(), ptr->nanovdb_filepath.c_str());
        }
    }
    ImGui::End();
}

void showFileHeaderWindow(imgui_instance_user::Instance* ptr)
{
    if (!ptr->window.show_file_header)
    {
        return;
    }

    if (ImGui::Begin(FILE_HEADER, &ptr->window.show_file_header))
    {
        pnanovdb_compute_array_t* current_array = nullptr;
        if (ptr->selected_view_type == ViewsTypes::NanoVDBs && ptr->nanovdb_arrays)
        {
            auto it = ptr->nanovdb_arrays->find(ptr->selected_scene_item);
            if (it != ptr->nanovdb_arrays->end())
            {
                // TODO: fixed on a branch
                // current_array = it->second.nanovdb_array;
            }
        }
        pnanovdb_editor::FileHeaderInfo::getInstance().render(current_array);
    }
    ImGui::End();
}

void showCodeEditorWindow(imgui_instance_user::Instance* ptr)
{
    if (!ptr->window.show_code_editor)
    {
        return;
    }

    pnanovdb_editor::CodeEditor::getInstance().setup(
        &ptr->shader_name, &ptr->pending.update_shader, ptr->dialog_size, ptr->run_shader, ptr->is_viewer());

    if (ImGui::Begin(CODE_EDITOR, &ptr->window.show_code_editor, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_MenuBar))
    {
        if (!pnanovdb_editor::CodeEditor::getInstance().render())
        {
            ptr->window.show_code_editor = false;
        }
    }
    ImGui::End();

    if (ptr->pending.update_generated)
    {
        pnanovdb_editor::CodeEditor::getInstance().updateViewer();
        ptr->pending.update_generated = false;
    }
}

void showProfilerWindow(imgui_instance_user::Instance* ptr, float delta_time)
{
    if (!ptr->window.show_profiler)
    {
        return;
    }

    if (ImGui::Begin(PROFILER, &ptr->window.show_profiler))
    {
        if (!pnanovdb_editor::Profiler::getInstance().render(&ptr->pending.update_memory_stats, delta_time))
        {
            ptr->window.show_profiler = false;
        }
    }
    ImGui::End();
}

void showConsoleWindow(imgui_instance_user::Instance* ptr)
{
    if (!ptr->window.show_console)
    {
        return;
    }

    if (ImGui::Begin(CONSOLE, &ptr->window.show_console))
    {
        if (!pnanovdb_editor::Console::getInstance().render())
        {
            ptr->window.show_console = false;
        }

        ImGui::ProgressBar(ptr->progress.value, ImVec2(-1, 0), "");

        ImVec2 pos = ImGui::GetItemRectMin();
        ImVec2 size_bar = ImGui::GetItemRectSize();
        ImVec2 text_size = ImGui::CalcTextSize(ptr->progress.text.c_str());
        ImVec2 text_pos = ImVec2(pos.x + 5.f, pos.y + (size_bar.y - text_size.y) * 0.5f);

        ImGui::GetWindowDrawList()->AddText(text_pos, ImGui::GetColorU32(ImGuiCol_Text), ptr->progress.text.c_str());
    }
    ImGui::End();
}

void showFileDialogs(imgui_instance_user::Instance* ptr)
{
    if (ptr->pending.open_file)
    {
        ptr->pending.open_file = false;

        IGFD::FileDialogConfig config;
        config.path = ".";

        ImGuiFileDialog::Instance()->OpenDialog(
            "OpenNvdbFileDlgKey", "Open NanoVDB File", "NanoVDB Files (*.nvdb){.nvdb}", config);
    }
    if (ptr->pending.save_file)
    {
        ptr->pending.save_file = false;

        IGFD::FileDialogConfig config;
        config.path = ".";

        ImGuiFileDialog::Instance()->OpenDialog(
            "SaveNvdbFileDlgKey", "Save NanoVDB File", "NanoVDB Files (*.nvdb){.nvdb}", config);
    }
    if (ptr->pending.find_shader_directory)
    {
        ptr->pending.find_shader_directory = false;

        IGFD::FileDialogConfig config;
        config.path = ".";

        ImGuiFileDialog::Instance()->OpenDialog(
            "SelectShaderDirectoryDlgKey", "Select Shader Directory", nullptr, config);
    }
    if (ptr->pending.find_raster_file)
    {
        ptr->pending.find_raster_file = false;

        IGFD::FileDialogConfig config;
        config.path = ".";

        ImGuiFileDialog::Instance()->OpenDialog(
            "OpenRasterFileDlgKey", "Open Gaussian File", "Gaussian Files (*.npy *.npz *.ply){.npy,.npz,.ply}", config);
    }

    if (ImGuiFileDialog::Instance()->IsOpened())
    {
        ImGui::SetNextWindowSize(ptr->dialog_size, ImGuiCond_Appearing);
        if (ImGuiFileDialog::Instance()->Display("OpenNvdbFileDlgKey"))
        {
            if (ImGuiFileDialog::Instance()->IsOk())
            {
                ptr->nanovdb_filepath = ImGuiFileDialog::Instance()->GetFilePathName();
                // ptr->nanovdb_path = ImGuiFileDialog::Instance()->GetCurrentPath();
                pnanovdb_editor::Console::getInstance().addLog("Opening file '%s'", ptr->nanovdb_filepath.c_str());
                ptr->pending.load_nvdb = true;
            }
            ImGuiFileDialog::Instance()->Close();
        }
        else if (ImGuiFileDialog::Instance()->Display("OpenRasterFileDlgKey"))
        {
            if (ImGuiFileDialog::Instance()->IsOk())
            {
                ptr->raster_filepath = ImGuiFileDialog::Instance()->GetFilePathName();
                ptr->pending.find_raster_file = false;
            }
            ImGuiFileDialog::Instance()->Close();
        }
        else if (ImGuiFileDialog::Instance()->Display("SaveNvdbFileDlgKey"))
        {
            if (ImGuiFileDialog::Instance()->IsOk())
            {
                ptr->nanovdb_filepath = ImGuiFileDialog::Instance()->GetFilePathName();
                pnanovdb_editor::Console::getInstance().addLog("Saving file '%s'...", ptr->nanovdb_filepath.c_str());
                ptr->pending.save_nanovdb = true;
            }
            ImGuiFileDialog::Instance()->Close();
        }
        else if (ImGuiFileDialog::Instance()->Display("SelectShaderDirectoryDlgKey"))
        {
            if (ImGuiFileDialog::Instance()->IsOk())
            {
                ptr->pending_shader_directory = ImGuiFileDialog::Instance()->GetCurrentPath();
            }
            ImGuiFileDialog::Instance()->Close();
        }
    }
}


void showAboutWindow(imgui_instance_user::Instance* ptr)
{
    if (!ptr->window.show_about)
    {
        return;
    }

    // Center the window on first use
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(400, 200), ImGuiCond_Appearing);

    if (ImGui::Begin("About", &ptr->window.show_about, ImGuiWindowFlags_NoResize))
    {
        ImGui::Text("NanoVDB Editor");
        ImGui::Separator();

        // Display version from VERSION.txt
        ImGui::Text("Version: %s", NANOVDB_EDITOR_VERSION);

        // Display git hash from CMake
        ImGui::Text("Build: %s", NANOVDB_EDITOR_COMMIT_HASH);
        if (strlen(NANOVDB_EDITOR_FVDB_COMMIT_HASH) > 0)
        {
            ImGui::Text("fVDB Build: %s", NANOVDB_EDITOR_FVDB_COMMIT_HASH);
        }

        ImGui::Separator();
        ImGui::Text("Copyright Contributors to the OpenVDB Project");
        ImGui::Text("SPDX-License-Identifier: Apache-2.0");

        ImGui::Spacing();
    }
    ImGui::End();
}

} // pnanovdb_editor
