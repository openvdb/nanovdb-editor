// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/editor/EditorWindows.h

    \author Andrew Reidmeyer, Petra Hapalova

    \brief  Window rendering functions for the ImGui editor
*/

#include "EditorWindows.h"

#include "ImguiInstance.h"
#include "EditorScene.h"
#include "EditorSceneManager.h"
#include "EditorToken.h"
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
#include <string>
#include <filesystem>
#include <type_traits>

#ifndef M_PI_2
#    define M_PI_2 1.57079632679489661923
#endif

namespace pnanovdb_editor
{
const float EPSILON = 1e-6f;

static inline void logRecordingSavedOnStop(bool wasRecording, pnanovdb_imgui_settings_render_t* settings)
{
    const bool isRecording = (settings->encode_to_file != 0);
    if (wasRecording && !isRecording)
    {
        const char* encodeFilename = settings->encode_filename;
        const char* basePtr = (encodeFilename && encodeFilename[0]) ? encodeFilename : "capture_stream";
        std::string base(basePtr);
        std::string filename = base + ".h264";
        pnanovdb_editor::Console::getInstance().addLog("Saved recording to '%s'", filename.c_str());
    }
}

static inline bool showResolutionCombo(const char* label, pnanovdb_int32_t* width, pnanovdb_int32_t* height)
{
    bool changed = false;
    if (ImGui::BeginCombo(label, "Select..."))
    {
        const char* labels[4] = { "1440x720", "1920x1080", "2560x1440", "3840x2160" };
        const int widths[4] = { 1440, 1920, 2560, 3840 };
        const int heights[4] = { 720, 1080, 1440, 2160 };
        for (int i = 0; i < 4; i++)
        {
            if (ImGui::Selectable(labels[i]))
            {
                *width = widths[i];
                *height = heights[i];
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

void saveIniSettings(imgui_instance_user::Instance* ptr)
{
    if (!ptr || ptr->is_viewer())
    {
        return;
    }

    ImGuiIO& io = ImGui::GetIO();
    if (io.IniFilename && *io.IniFilename)
    {
        // Save unscaled (1.0f scale) window size to INI; ImGuiWindow (GLFW) scales on load
        const float scale = (io.DisplayFramebufferScale.x > 0.f) ? io.DisplayFramebufferScale.x : 1.f;
        ptr->ini_window_width = (int)(io.DisplaySize.x / scale);
        ptr->ini_window_height = (int)(io.DisplaySize.y / scale);

        imgui_instance_user::copyPersistentFields(
            ptr->saved_render_settings[ptr->render_settings_name], *ptr->render_settings);
        ImGui::SaveIniSettingsToDisk(io.IniFilename);
    }
}

void createMenu(imgui_instance_user::Instance* ptr)
{
    bool isViewerProfile = ptr->is_viewer();

    if (ImGui::BeginMainMenuBar())
    {
        if (!isViewerProfile && ImGui::BeginMenu("File"))
        {
            ImGui::MenuItem("Load NanoVDB...", "", &ptr->pending.open_file);
            ImGui::MenuItem("Import Gaussian...", "", &ptr->pending.find_raster_file);
            ImGui::MenuItem("Save NanoVDB...", "", &ptr->pending.save_file);
            ImGui::Separator();
            if (ImGui::MenuItem("Save INI"))
            {
                saveIniSettings(ptr);
            }
            if (ImGui::MenuItem("Load INI"))
            {
                // INI loading disabled
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Windows"))
        {
            ImGui::MenuItem(RENDER_SETTINGS, "", &ptr->window.show_render_settings);
            if (!isViewerProfile)
            {
                ImGui::MenuItem(COMPILER_SETTINGS, "", &ptr->window.show_compiler_settings);
            }
            ImGui::MenuItem(SCENE, "", &ptr->window.show_scene);
            ImGui::MenuItem(PROPERTIES, "", &ptr->window.show_scene_properties);
            ImGui::MenuItem(PROFILER, "", &ptr->window.show_profiler);
            ImGui::MenuItem(CODE_EDITOR, "", &ptr->window.show_code_editor);
            ImGui::MenuItem(SHADER_PARAMS, "", &ptr->window.show_shader_params);
            if (!isViewerProfile)
            {
                ImGui::MenuItem(FILE_HEADER, "", &ptr->window.show_file_header);
            }
            ImGui::MenuItem(CONSOLE, "", &ptr->window.show_console);
            if (!isViewerProfile)
            {
                ImGui::MenuItem(BENCHMARK, "", &ptr->window.show_benchmark);
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Help"))
        {
            ImGui::MenuItem("About", "", &ptr->window.show_about);
            ImGui::EndMenu();
        }

        // Center-aligned application label
        {
            std::string centerText;

            // Always show the current scene name
            pnanovdb_editor_token_t* current_scene = ptr->editor_scene->get_current_scene_token();
            if (current_scene && current_scene->str)
            {
                centerText += std::string(current_scene->str) + " - ";
            }

            // Then add selected object name if available
            auto selection = ptr->editor_scene->get_properties_selection();
            if (selection.name_token && selection.name_token->str)
            {
                centerText += std::string(selection.name_token->str) + " - ";
            }

            centerText += "NanoVDB Editor";
            if (isViewerProfile)
            {
                centerText += " - fVDB (" + std::to_string(ptr->render_settings->server_port) + ")";
            }
            else
            {
                if (ptr->render_settings->enable_encoder)
                {
                    centerText += " (" + std::string(ptr->render_settings->server_address) + ":" +
                                  std::to_string(ptr->render_settings->server_port) + ")";
                }
            }
            float windowWidth = ImGui::GetWindowWidth();
            float textWidth = ImGui::CalcTextSize(centerText.c_str()).x;
            ImGui::SetCursorPosX((windowWidth - textWidth) * 0.5f);
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::Text("%s", centerText.c_str());
            ImGui::PopStyleColor();
        }

        // Right-aligned Recording toggle
        if (ptr->render_settings->enable_encoder)
        {
            const bool isRecording = (ptr->render_settings->encode_to_file != 0);

            // Use red color for both record (circle) and stop (square)
            const ImVec4 baseColor = ImVec4(0.85f, 0.20f, 0.20f, 1.0f);

            const float h = ImGui::GetFrameHeight();
            const ImVec2 size(h, h);

            const float windowWidthR = ImGui::GetWindowWidth();
            const ImGuiStyle& style = ImGui::GetStyle();
            const float rightPadding = style.FramePadding.x;

            ImGui::SameLine(windowWidthR - size.x - rightPadding);

            ImVec2 pMin = ImGui::GetCursorScreenPos();
            ImVec2 pMax = ImVec2(pMin.x + size.x, pMin.y + size.y);

            // Click/hover handling via an invisible button covering the icon area
            ImGui::InvisibleButton("##recording", size);
            const bool hovered = ImGui::IsItemHovered();
            const bool held = ImGui::IsItemActive();
            if (ImGui::IsItemClicked())
            {
                const bool wasRecording = isRecording;
                ptr->render_settings->encode_to_file ^= PNANOVDB_TRUE;
                logRecordingSavedOnStop(wasRecording, ptr->render_settings);
            }

            if (hovered)
            {
                ImGui::SetTooltip("Stream to file");
            }

            // Adjust color for hover/active feedback
            ImVec4 drawColor = baseColor;
            if (held)
            {
                drawColor.x *= 0.85f;
                drawColor.y *= 0.85f;
                drawColor.z *= 0.85f;
            }
            else if (hovered)
            {
                drawColor.x *= 1.20f;
                drawColor.y *= 1.20f;
                drawColor.z *= 1.20f;
            }

            ImDrawList* dl = ImGui::GetWindowDrawList();

            // Optional subtle background hover hint
            if (hovered || held)
            {
                const ImU32 bgCol = ImGui::GetColorU32(ImVec4(1.f, 1.f, 1.f, held ? 0.06f : 0.08f));
                dl->AddRectFilled(pMin, pMax, bgCol, 4.0f);
            }

            // Draw icon using primitives
            const float inner = size.y * 0.22f;
            const float yOffset = 0.0f;
            const ImVec2 innerMin(pMin.x + inner, pMin.y + inner + yOffset);
            const ImVec2 innerMax(pMax.x - inner, pMax.y - inner + yOffset);
            const ImU32 iconCol = ImGui::GetColorU32(drawColor);
            if (isRecording)
            {
                // White square (stop)
                const ImU32 whiteCol = IM_COL32(255, 255, 255, 255);
                dl->AddRectFilled(innerMin, innerMax, whiteCol, 2.0f);
            }
            else
            {
                // Red circle (record)
                const ImVec2 center((innerMin.x + innerMax.x) * 0.5f, (innerMin.y + innerMax.y) * 0.5f);
                const float radius = 0.5f * std::min(innerMax.x - innerMin.x, innerMax.y - innerMin.y);
                dl->AddCircleFilled(center, radius, iconCol, 24);
            }
        }

        ImGui::EndMainMenuBar();
    }
}

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

void showRenderSettingsWindow(imgui_instance_user::Instance* ptr)
{
    if (!ptr->window.show_render_settings)
    {
        return;
    }

    if (ImGui::Begin(RENDER_SETTINGS, &ptr->window.show_render_settings))
    {
        auto settings = ptr->render_settings;

        ImGui::SeparatorText("Camera");
        IMGUI_CHECKBOX_SYNC("Upside Down", settings->is_upside_down);
        {
            int up_axis = settings->is_y_up ? 0 : 1;
            const char* up_axis_items[] = { "Y", "Z" };
            if (ImGui::Combo("Up Axis", &up_axis, up_axis_items, IM_ARRAYSIZE(up_axis_items)))
            {
                settings->is_y_up = (up_axis == 0);
            }
        }
        {
            if (ImGui::RadioButton("Perspective", ptr->render_settings->is_orthographic == PNANOVDB_FALSE))
            {
                ptr->render_settings->is_orthographic = PNANOVDB_FALSE;
                ptr->render_settings->camera_config.is_orthographic = PNANOVDB_FALSE;
            }
            ImGui::SameLine();
            if (ImGui::RadioButton("Orthographic", ptr->render_settings->is_orthographic == PNANOVDB_TRUE))
            {
                ptr->render_settings->is_orthographic = PNANOVDB_TRUE;
                ptr->render_settings->camera_config.is_orthographic = PNANOVDB_TRUE;
            }
        }
        ImGui::DragFloat("Speed Multiplier", &settings->camera_speed_multiplier, 0.f, 1.f, 10000.f, "%.1f",
                         ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp);

        ImGui::SeparatorText("Streaming");
        ImGui::InputText("Server Address", settings->server_address, 256u);
        ImGui::InputInt("Server Port", &settings->server_port);
        if (settings->enable_encoder)
        {
            ImGui::AlignTextToFramePadding();
            ImGui::Text("Streaming is running...");

            {
                const bool wasRecording = (settings->encode_to_file != 0);
                bool temp_bool = wasRecording;
                if (ImGui::Checkbox("Stream To File", &temp_bool))
                {
                    settings->encode_to_file = temp_bool ? PNANOVDB_TRUE : PNANOVDB_FALSE;
                    logRecordingSavedOnStop(wasRecording, settings);
                }
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(220.0f);
            ImGui::InputText("##StreamFileName", settings->encode_filename, IM_ARRAYSIZE(settings->encode_filename));

            {
                std::string baseName = settings->encode_filename[0] ? settings->encode_filename : "capture_stream";
                std::string inputFile = baseName + ".h264";
                std::string outputFile = baseName + ".mp4";
                std::string ffmpeg_cmd =
                    std::string("ffmpeg -i \"") + inputFile + "\" -c:v copy -f mp4 \"" + outputFile + "\"";
                ImGui::Indent(16.0f);
                ImGui::BeginGroup();

                const float h = ImGui::GetFrameHeight() * 1.1f;
                const ImVec2 size(h, h);
                if (ImGui::InvisibleButton("##copy_ffmpeg_cmd", size))
                {
                    ImGui::SetClipboardText(ffmpeg_cmd.c_str());
                }

                const bool hovered = ImGui::IsItemHovered();
                const bool held = ImGui::IsItemActive();
                if (hovered)
                {
                    ImGui::SetTooltip("Copy to clipboard");
                }

                ImDrawList* dl = ImGui::GetWindowDrawList();
                ImVec2 pMin = ImGui::GetItemRectMin();
                ImVec2 pMax = ImGui::GetItemRectMax();

                ImVec4 colV = hovered || held ? ImGui::GetStyleColorVec4(ImGuiCol_Text) :
                                                ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
                if (held)
                {
                    colV.x *= 0.9f;
                    colV.y *= 0.9f;
                    colV.z *= 0.9f;
                }
                const ImU32 col = ImGui::GetColorU32(colV);

                const float pad = h * 0.22f;
                const ImVec2 rectMin(pMin.x + pad, pMin.y + pad);
                const ImVec2 rectMax(pMax.x - pad, pMax.y - pad);
                const ImVec2 backOffset(-pad * 0.6f, -pad * 0.6f);

                // Draw copy icon (two overlapping rounded rectangles)
                dl->AddRect(rectMin + backOffset, rectMax + backOffset, col, 2.0f, 0, 1.5f);
                dl->AddRect(rectMin, rectMax, col, 2.0f, 0, 2.0f);

                ImGui::SameLine();
                ImGui::AlignTextToFramePadding();
                // ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (h - ImGui::GetTextLineHeight()) * 0.20f);
                ImGui::TextDisabled("%s", ffmpeg_cmd.c_str());

                ImGui::EndGroup();
                ImGui::Unindent(16.0f);
            }
        }
        else
        {
            if (ImGui::Button("Start Streaming"))
            {
                settings->enable_encoder = PNANOVDB_TRUE;
                if (!settings->window_resize && !settings->encode_resize)
                {
                    settings->window_resize = PNANOVDB_TRUE;
                }
            }
        }

        ImGui::SeparatorText("Resolution");
        {
            // Resolution mode selection
            if (settings->enable_encoder)
            {
                // When streaming, show radio buttons for Fixed vs Fit Resolution
                bool isFixedResolution = settings->window_resize && !settings->encode_resize;
                bool isFitResolution = settings->encode_resize && !settings->window_resize;

                if (ImGui::RadioButton("Fixed", isFixedResolution))
                {
                    settings->window_resize = PNANOVDB_TRUE;
                    settings->encode_resize = PNANOVDB_FALSE;
                }
                ImGui::SameLine();
                if (ImGui::RadioButton("Fit Client", isFitResolution))
                {
                    settings->window_resize = PNANOVDB_FALSE;
                    settings->encode_resize = PNANOVDB_TRUE;
                }

                if (settings->window_resize)
                {
                    showResolutionCombo("Resolution", &settings->window_width, &settings->window_height);
                    ImGui::InputInt("Width", &settings->window_width);
                    ImGui::InputInt("Height", &settings->window_height);
                }
                else if (settings->encode_resize)
                {
                    ImGui::BeginDisabled();
                    ImGui::Text("Width: %d", settings->window_width);
                    ImGui::Text("Height: %d", settings->window_height);
                    ImGui::EndDisabled();
                }
            }
            else
            {
                // When not streaming, just show Fixed Resolution checkbox
                IMGUI_CHECKBOX_SYNC("Fixed Resolution", settings->window_resize);
                if (settings->window_resize)
                {
                    showResolutionCombo("Resolution", &settings->window_width, &settings->window_height);
                    ImGui::InputInt("Width", &settings->window_width);
                    ImGui::InputInt("Height", &settings->window_height);
                }
                else
                {
                    ImGui::BeginDisabled();
                    ImGui::Text("Width: %d", settings->window_width);
                    ImGui::Text("Height: %d", settings->window_height);
                    ImGui::EndDisabled();
                }
            }
        }

        ImGui::SeparatorText("Advanced");
        IMGUI_CHECKBOX_SYNC("VSync", settings->vsync);
        IMGUI_CHECKBOX_SYNC("Projection RH", settings->is_projection_rh);
        IMGUI_CHECKBOX_SYNC("Reverse Z Buffer", settings->is_reverse_z);

        {
            static bool s_device_list_built = false;
            static std::vector<std::string> s_device_labels;
            static std::vector<const char*> s_device_label_ptrs;

            if (!s_device_list_built && ptr->compute)
            {
                pnanovdb_compute_device_manager_t* device_manager =
                    ptr->compute->device_interface.create_device_manager(PNANOVDB_FALSE);
                if (device_manager)
                {
                    for (pnanovdb_uint32_t idx = 0u;; idx++)
                    {
                        pnanovdb_compute_physical_device_desc_t desc = {};
                        if (!ptr->compute->device_interface.enumerate_devices(device_manager, idx, &desc))
                        {
                            break;
                        }

                        const char* name = desc.device_name[0] ? desc.device_name : "Unknown";
                        std::string label = std::to_string(idx) + " - " + name;
                        s_device_labels.push_back(label);
                    }

                    ptr->compute->device_interface.destroy_device_manager(device_manager);
                }

                s_device_label_ptrs.clear();
                s_device_label_ptrs.reserve(s_device_labels.size());
                for (const auto& s : s_device_labels)
                {
                    s_device_label_ptrs.push_back(s.c_str());
                }
                s_device_list_built = true;
            }

            // Use preselected device index captured during initialization
            int device_index = ptr->device_index;
            if (!s_device_label_ptrs.empty())
            {
                ImGui::SeparatorText("Devices");
                ImGui::BeginDisabled(true);
                for (int i = 0; i < (int)s_device_label_ptrs.size(); ++i)
                {
                    bool selected = (i == device_index);
                    ImGui::Selectable(s_device_label_ptrs[i], selected);
                }
                ImGui::EndDisabled();
            }
            else
            {
                ImGui::TextDisabled("No devices found");
            }
        }

        // UI Profile - only visible with default profile
        if (!ptr->is_viewer())
        {
            ImGui::SeparatorText("UI Profile");
            const char* profile_options[] = { "default", "viewer" };
            const char* current_profile =
                ptr->render_settings->ui_profile_name[0] != '\0' ? ptr->render_settings->ui_profile_name : "default";
            if (ImGui::BeginCombo("##ui_profile", current_profile))
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

        const char* optimization_levels[] = { "None (Fastest)", "Default", "High", "Maximal (Slowest)" };
        int optimization_level = (int)ptr->compiler_settings.optimization_level;
        if (ImGui::Combo(
                "Optimization Level", &optimization_level, optimization_levels, IM_ARRAYSIZE(optimization_levels)))
        {
            ptr->compiler_settings.optimization_level = (pnanovdb_optimization_level_t)(optimization_level);
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

    auto* scene_manager = ptr->editor_scene ? ptr->editor_scene->get_scene_manager() : nullptr;
    if (!scene_manager)
    {
        return;
    }
    auto& shader_params = scene_manager->shader_params;

    if (ImGui::Begin(SHADER_PARAMS, &ptr->window.show_shader_params))
    {
        std::string shader_name;
        const std::string viewport_shader = ptr->editor_scene->get_selected_object_shader_name();

        ImGui::BeginGroup();

        // Show params which are parsed from the shader used in current vieweport
        if (ImGui::RadioButton(
                "Viewport Shader: ", ptr->pending.shader_selection_mode == ShaderSelectionMode::UseViewportShader))
        {
            ptr->pending.shader_selection_mode = ShaderSelectionMode::UseViewportShader;
        }

        // Display the currently active viewport shader filename (if any)
        if (!viewport_shader.empty())
        {
            ImGui::SameLine();
            std::filesystem::path vpPath(viewport_shader);
            ImGui::Text("%s", vpPath.filename().string().c_str());
        }

        // Show params which are parsed from the shader opened in the code editor
        if (ImGui::RadioButton("Shader Editor Selected: ",
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

        const bool is_viewport_mode = ptr->pending.shader_selection_mode == ShaderSelectionMode::UseViewportShader;
        const std::string& target_name =
            is_group_mode ? ptr->shader_group : (is_viewport_mode ? viewport_shader : shader_name);
        if (!target_name.empty())
        {
            if (shader_params.isJsonLoaded(target_name, is_group_mode))
            {
                if (ImGui::Button("Reload JSON"))
                {
                    if (is_group_mode)
                    {
                        if (shader_params.loadGroup(target_name, true))
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
                        if (shader_params.load(target_name, true))
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
                        shader_params.createGroup(target_name);
                    }
                    else
                    {
                        shader_params.create(target_name);
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
                shader_params.renderGroup(target_name);
            }
            else
            {
                if (shader_params.isJsonLoaded(target_name))
                {
                    shader_params.render(target_name);
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
        auto selection = ptr->editor_scene->get_properties_selection();
        const char* selected_name =
            (selection.name_token && selection.name_token->str) ? selection.name_token->str : nullptr;

        ptr->editor_scene->for_each_view(
            ViewType::NanoVDBs,
            [&](uint64_t name_id, const auto& ctx)
            {
                using CtxT = std::decay_t<decltype(ctx)>;
                if constexpr (std::is_same_v<CtxT, NanoVDBContext>)
                {
                    if (selected_name && selection.name_token && selection.name_token->id == name_id)
                    {
                        current_array = ctx.nanovdb_array.get();
                    }
                }
            });
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
        &ptr->editor_shader_name, &ptr->pending.update_shader, ptr->dialog_size, ptr->run_shader, ptr->is_viewer());

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

    if (ImGui::Begin(
            CONSOLE, &ptr->window.show_console, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
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

// Custom side pane callback for Gaussian import dialog
static bool gaussianImportSidePane(const char* /*vFilter*/, IGFDUserDatas vUserDatas, bool* /*cantContinue*/)
{
    auto* ptr = static_cast<imgui_instance_user::Instance*>(vUserDatas);
    if (!ptr)
        return false;

    ImGui::Text("Import Options:");
    ImGui::Separator();

    int rasterMode = ptr->raster_to_nanovdb ? 1 : 0;
    ImGui::RadioButton("Raster2D (Gaussian Splatting)", &rasterMode, 0);
    ImGui::RadioButton("Raster3D (Convert to NanoVDB)", &rasterMode, 1);
    ptr->raster_to_nanovdb = (rasterMode == 1);

    ImGui::Spacing();

    if (ptr->raster_to_nanovdb)
    {
        ImGui::Text("Voxel Size:");
        ImGui::SetNextItemWidth(150.0f);
        ImGui::InputFloat("##VoxelSizeRaster", &ptr->raster_voxels_per_unit, 1.0f, 10.0f, "%.2f");
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Voxels per unit (higher = finer resolution)");
        }
    }

    return true;
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
        config.sidePane = gaussianImportSidePane;
        config.sidePaneWidth = 250.0f;
        config.flags = ImGuiFileDialogFlags_None;
        config.userDatas = ptr;

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
                pnanovdb_editor::Console::getInstance().addLog(
                    "Importing Gaussian file '%s' as %s%s", ptr->raster_filepath.c_str(),
                    ptr->raster_to_nanovdb ? "Raster3D" : "Raster2D",
                    ptr->raster_to_nanovdb ?
                        (" (voxel size: " + std::to_string(ptr->raster_voxels_per_unit) + ")").c_str() :
                        "");
                ptr->pending.update_raster = true;
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
#ifdef NANOVDB_EDITOR_PYPI_BUILD
        ImGui::Text("Version: %s (PyPI Build)", NANOVDB_EDITOR_VERSION);
#else
        ImGui::Text("Version: %s", NANOVDB_EDITOR_VERSION);
#endif

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
