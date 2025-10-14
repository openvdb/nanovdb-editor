// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/editor/ImguiInstance.cpp

    \author Andrew Reidmeyer, Petra Hapalova

    \brief
*/

#include "ImguiInstance.h"

#include "imgui.h"
#include "imgui/ImguiRenderer.h"
#include "compute/ComputeShader.h"
#include "CameraFrustum.h"
#include "EditorWindows.h"
#include "CodeEditor.h"
#include "Console.h"
#include "RenderSettingsHandler.h"
#include "InstanceSettingsHandler.h"
#include "ImguiIni.h"

#include <stdio.h>
#include <imgui_internal.h> // for the docking branch
#include "misc/cpp/imgui_stdlib.h" // for std::string text input

#if defined(_WIN32)
#    include <Windows.h>
#else
#    include <time.h>
#endif

#include <algorithm>

PNANOVDB_INLINE void timestamp_capture(pnanovdb_uint64_t* ptr)
{
#if defined(_WIN32)
    LARGE_INTEGER tmpCpuTime = {};
    QueryPerformanceCounter(&tmpCpuTime);
    (*ptr) = tmpCpuTime.QuadPart;
#else
    timespec timeValue = {};
    clock_gettime(CLOCK_MONOTONIC, &timeValue);
    (*ptr) = 1E9 * pnanovdb_uint64_t(timeValue.tv_sec) + pnanovdb_uint64_t(timeValue.tv_nsec);
#endif
}
PNANOVDB_INLINE pnanovdb_uint64_t timestamp_frequency()
{
#if defined(_WIN32)
    LARGE_INTEGER tmpCpuFreq = {};
    QueryPerformanceFrequency(&tmpCpuFreq);
    return tmpCpuFreq.QuadPart;
#else
    return 1E9;
#endif
}
PNANOVDB_INLINE float timestamp_diff(pnanovdb_uint64_t begin, pnanovdb_uint64_t end, pnanovdb_uint64_t freq)
{
    return (float)(((double)(end - begin) / (double)(freq)));
}


namespace imgui_instance_user
{
pnanovdb_imgui_instance_t* create(void* userdata,
                                  void* user_settings,
                                  const pnanovdb_reflect_data_type_t* user_settings_data_type)
{
    auto ptr = new Instance();

    if (pnanovdb_reflect_layout_compare(
            user_settings_data_type, PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_imgui_settings_render_t)))
    {
        ptr->render_settings = (pnanovdb_imgui_settings_render_t*)user_settings;
    }

    *((Instance**)userdata) = ptr;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    ptr->update_ini_filename_for_profile(ptr->render_settings->ui_profile_name);

    // Register settings handlers
    ImGuiContext* context = ImGui::GetCurrentContext();
    RenderSettingsHandler::Register(context, ptr);
    InstanceSettingsHandler::Register(context, ptr);
    pnanovdb_editor::CodeEditor::getInstance().registerSettingsHandler(context);

    return cast(ptr);
}

void destroy(pnanovdb_imgui_instance_t* instance)
{
    auto ptr = cast(instance);

    if (ImGui::GetCurrentContext() && !ptr->is_viewer())
    {
        ImGuiIO& io = ImGui::GetIO();
        if (io.IniFilename && *io.IniFilename)
        {
            ptr->ini_window_width = (int)io.DisplaySize.x;
            ptr->ini_window_height = (int)io.DisplaySize.y;

            ptr->saved_render_settings[ptr->render_settings_name] = *ptr->render_settings;
            ImGui::SaveIniSettingsToDisk(io.IniFilename);
        }
    }

    ImGui::DestroyContext();

    delete ptr;
}

// #define INIT_VIEWER_DOCKING // uncomment to generate .ini string for ImguiIni.h

static void initializeDocking(Instance* ptr)
{
    ImGuiID dockspace_id = 1u;
    ImGui::DockSpaceOverViewport(dockspace_id, nullptr, ImGuiDockNodeFlags_PassthruCentralNode);

    bool isViewerProfile = ptr->is_viewer();

#ifndef INIT_VIEWER_DOCKING
    if (isViewerProfile)
    {
        // Viewer's settings are loaded from memory, skip custom layout building
        return;
    }
#endif

    bool hasIniOnDisk = false;
    ImGuiIO& io = ImGui::GetIO();
    if (io.IniFilename && *io.IniFilename)
    {
        FILE* f = fopen(io.IniFilename, "rb");
        if (f)
        {
            hasIniOnDisk = true;
            fclose(f);
        }
    }

    if (!hasIniOnDisk && !ptr->is_docking_setup)
    {
        // setup docking once
        ptr->is_docking_setup = true;

        float window_width = ImGui::GetIO().DisplaySize.x;
        float window_height = ImGui::GetIO().DisplaySize.y;

#ifdef INIT_VIEWER_DOCKING
        float left_dock_width = window_width * 0.25f;
        float right_dock_width = window_width * 0.20f;
        float far_right_dock_width = window_width * 0.30f;
        float bottom_dock_height = window_height * 0.2f;

        // clear existing layout
        ImGui::DockBuilderRemoveNode(dockspace_id);
        ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);

        // create dock spaces for various windows, the order matters!
        ImGuiID dock_id_right_far =
            ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Right, 0.f, nullptr, &dockspace_id);
        ImGui::DockBuilderSetNodeSize(dock_id_right_far, ImVec2(far_right_dock_width, window_height));
        ImGui::DockBuilderDockWindow(CODE_EDITOR, dock_id_right_far);
        ImGui::DockBuilderDockWindow(PROFILER, dock_id_right_far);
        ImGui::DockBuilderDockWindow(FILE_HEADER, dock_id_right_far);

        ImGuiID dock_id_right = ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Right, 0.f, nullptr, &dockspace_id);
        ImGui::DockBuilderSetNodeSize(dock_id_right, ImVec2(right_dock_width, window_height));

        ImGuiID dock_id_right_top = dock_id_right;

        ImGuiID dock_id_right_bottom =
            ImGui::DockBuilderSplitNode(dock_id_right_top, ImGuiDir_Down, 0.6f, nullptr, &dock_id_right_top);

        ImGuiID dock_id_bottom = ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Down, 0.f, nullptr, &dockspace_id);
        ImGui::DockBuilderSetNodeSize(dock_id_bottom, ImVec2(window_width, bottom_dock_height));
        ImGui::DockBuilderDockWindow(CONSOLE, dock_id_bottom);

        ImGuiID dock_id_left = ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Left, 0.f, nullptr, &dockspace_id);
        ImGui::DockBuilderSetNodeSize(dock_id_left, ImVec2(left_dock_width, window_height));

        ImGuiID dock_id_left_top = dock_id_left;
        ImGui::DockBuilderDockWindow(SCENE, dock_id_left_top);
        ImGui::DockBuilderDockWindow(VIEWPORT_SETTINGS, dock_id_left_top);
        ImGui::DockBuilderDockWindow(RENDER_SETTINGS, dock_id_left_top);
        ImGui::DockBuilderDockWindow(COMPILER_SETTINGS, dock_id_left_top);

        ImGuiID dock_id_left_bottom =
            ImGui::DockBuilderSplitNode(dock_id_left_top, ImGuiDir_Down, 0.f, nullptr, &dock_id_left_top);
        ImGui::DockBuilderSetNodeSize(dock_id_left_bottom, ImVec2(left_dock_width, window_height));
        ImGui::DockBuilderDockWindow(PROPERTIES, dock_id_left_bottom);
        ImGui::DockBuilderDockWindow(SHADER_PARAMS, dock_id_left_bottom);
        ImGui::DockBuilderDockWindow(BENCHMARK, dock_id_left_bottom);
#else
        float left_dock_width = window_width * 0.25f;
        float right_dock_width = window_width * 0.20f;
        float far_right_dock_width = window_width * 0.30f;
        float bottom_dock_height = window_height * 0.2f;
        float bottom_right_height = window_height * 0.4f;

        // clear existing layout
        ImGui::DockBuilderRemoveNode(dockspace_id);
        ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);

        // create dock spaces for various windows, the order matters!
        ImGuiID dock_id_right_far =
            ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Right, 0.f, nullptr, &dockspace_id);
        ImGui::DockBuilderSetNodeSize(dock_id_right_far, ImVec2(far_right_dock_width, window_height));
        ImGui::DockBuilderDockWindow(CODE_EDITOR, dock_id_right_far);
        ImGui::DockBuilderDockWindow(PROFILER, dock_id_right_far);
        ImGui::DockBuilderDockWindow(FILE_HEADER, dock_id_right_far);

        ImGuiID dock_id_right = ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Right, 0.f, nullptr, &dockspace_id);
        ImGui::DockBuilderSetNodeSize(dock_id_right, ImVec2(right_dock_width, window_height));

        ImGuiID dock_id_right_top = dock_id_right;
        ImGui::DockBuilderDockWindow(SCENE, dock_id_right_top);

        ImGuiID dock_id_right_bottom =
            ImGui::DockBuilderSplitNode(dock_id_right_top, ImGuiDir_Down, 0.6f, nullptr, &dock_id_right_top);
        ImGui::DockBuilderSetNodeSize(dock_id_right_bottom, ImVec2(window_width, bottom_right_height));
        ImGui::DockBuilderDockWindow(PROPERTIES, dock_id_right_bottom);

        ImGuiID dock_id_bottom = ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Down, 0.f, nullptr, &dockspace_id);
        ImGui::DockBuilderSetNodeSize(dock_id_bottom, ImVec2(window_width, bottom_dock_height));
        ImGui::DockBuilderDockWindow(CONSOLE, dock_id_bottom);

        ImGuiID dock_id_left = ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Left, 0.f, nullptr, &dockspace_id);
        ImGui::DockBuilderSetNodeSize(dock_id_left, ImVec2(left_dock_width, window_height));

        ImGuiID dock_id_left_top = dock_id_left;
        ImGui::DockBuilderDockWindow(VIEWPORT_SETTINGS, dock_id_left_top);
        ImGui::DockBuilderDockWindow(RENDER_SETTINGS, dock_id_left_top);
        ImGui::DockBuilderDockWindow(COMPILER_SETTINGS, dock_id_left_top);

        ImGuiID dock_id_left_bottom =
            ImGui::DockBuilderSplitNode(dock_id_left_top, ImGuiDir_Down, 0.f, nullptr, &dock_id_left_top);
        ImGui::DockBuilderSetNodeSize(dock_id_left_bottom, ImVec2(left_dock_width, window_height));
        ImGui::DockBuilderDockWindow(SHADER_PARAMS, dock_id_left_bottom);
        ImGui::DockBuilderDockWindow(BENCHMARK, dock_id_left_bottom);

        ImGui::DockBuilderFinish(dockspace_id);
#endif
    }
}

static void createMenu(Instance* ptr)
{
    bool isViewerProfile = ptr->is_viewer();

    if (ImGui::BeginMainMenuBar())
    {
        if (!isViewerProfile && ImGui::BeginMenu("File"))
        {
            ImGui::MenuItem("Open...", "", &ptr->pending.open_file);
            ImGui::MenuItem("Save...", "", &ptr->pending.save_file);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Window"))
        {
            ImGui::MenuItem(CODE_EDITOR, "", &ptr->window.show_code_editor);
            ImGui::MenuItem(PROFILER, "", &ptr->window.show_profiler);
            ImGui::MenuItem(FILE_HEADER, "", &ptr->window.show_file_header);
            ImGui::MenuItem(CONSOLE, "", &ptr->window.show_console);
            ImGui::MenuItem(SHADER_PARAMS, "", &ptr->window.show_shader_params);
            ImGui::MenuItem(SCENE, "", &ptr->window.show_scene);
            ImGui::MenuItem(PROPERTIES, "", &ptr->window.show_scene_properties);
            if (!isViewerProfile)
            {
                ImGui::MenuItem(BENCHMARK, "", &ptr->window.show_benchmark);
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Settings"))
        {
            if (!isViewerProfile)
            {
                ImGui::MenuItem(VIEWPORT_SETTINGS, "", &ptr->window.show_viewport_settings);
            }
            ImGui::MenuItem(RENDER_SETTINGS, "", &ptr->window.show_render_settings);
            ImGui::MenuItem(COMPILER_SETTINGS, "", &ptr->window.show_compiler_settings);

            if (!isViewerProfile)
            {
                ImGui::Separator();
                if (ImGui::MenuItem("Save INI"))
                {
                    ImGuiIO& io = ImGui::GetIO();
                    if (io.IniFilename && *io.IniFilename)
                    {
                        ptr->ini_window_width = (int)io.DisplaySize.x;
                        ptr->ini_window_height = (int)io.DisplaySize.y;

                        ptr->saved_render_settings[ptr->render_settings_name] = *ptr->render_settings;
                        ImGui::SaveIniSettingsToDisk(io.IniFilename);
                    }
                }
                if (ImGui::MenuItem("Load INI"))
                {
                    ImGuiIO& io = ImGui::GetIO();
                    if (io.IniFilename && *io.IniFilename)
                    {
                        ImGui::LoadIniSettingsFromDisk(io.IniFilename);
                        *ptr->render_settings = ptr->saved_render_settings[ptr->render_settings_name];
                        ptr->render_settings->sync_camera = PNANOVDB_TRUE;
                    }
                }
            }

            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Help"))
        {
            ImGui::MenuItem("About", "", &ptr->window.show_about);
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }
}

static void showWindows(Instance* ptr, float delta_time)
{
    using namespace pnanovdb_editor;

    showSceneWindow(ptr);
    showViewportSettingsWindow(ptr);
    showRenderSettingsWindow(ptr);
    showCompilerSettingsWindow(ptr);
    showShaderParamsWindow(ptr);
    showPropertiesWindow(ptr);
    showBenchmarkWindow(ptr);
    showFileHeaderWindow(ptr);
    showCodeEditorWindow(ptr);
    showProfilerWindow(ptr, delta_time);
    showConsoleWindow(ptr);
    showAboutWindow(ptr);
}

// If new windows appeared (not present in ini yet), mark settings dirty so they get saved
static void markIniDirtyIfNewWindowsAppeared(Instance* ptr)
{
    bool isViewerProfile = ptr->is_viewer();
    if (isViewerProfile)
    {
        return;
    }

    bool need_dirty = false;
    auto ensure_entry = [&](const char* name)
    {
        if (ImGui::FindWindowSettingsByID(ImHashStr(name)) == nullptr)
        {
            if (ImGui::FindWindowByName(name) != nullptr)
            {
                need_dirty = true;
            }
        }
    };

    ensure_entry(CODE_EDITOR);
    ensure_entry(PROFILER);
    ensure_entry(FILE_HEADER);
    ensure_entry(CONSOLE);
    ensure_entry(SHADER_PARAMS);
    ensure_entry(SCENE);
    ensure_entry(PROPERTIES);
    ensure_entry(BENCHMARK);
    ensure_entry(VIEWPORT_SETTINGS);
    ensure_entry(RENDER_SETTINGS);
    ensure_entry(COMPILER_SETTINGS);

    if (need_dirty)
    {
        ImGui::MarkIniSettingsDirty();
    }
}

void update(pnanovdb_imgui_instance_t* instance)
{
    auto ptr = cast(instance);

    // compute delta_time
    float delta_time = 0.f;
    pnanovdb_uint64_t end;
    timestamp_capture(&end);

    if (ptr->last_timestamp != 0llu)
    {
        delta_time = timestamp_diff(end, ptr->last_timestamp, timestamp_frequency());
    }
    ptr->last_timestamp = end;
    if (delta_time > 1.f / 30.f) // limit maximum time step to cover hitching
    {
        delta_time = 1.f / 30.f;
    }

    // Handle profile switching and ini loading
    {
        ImGuiIO& io = ImGui::GetIO();

        const char* profile_name = ptr->render_settings->ui_profile_name;
        bool profile_changed = (ptr->current_profile_name != profile_name);
        if (profile_changed)
        {
            ptr->update_ini_filename_for_profile(profile_name);

            // Force reload settings and docking for the new profile
            ptr->loaded_ini_once = false;
            ptr->is_docking_setup = false;
        }

        if (!ptr->loaded_ini_once)
        {
            // For viewer profile, load from memory; otherwise load from disk
            bool isViewerProfile = ptr->is_viewer();
            if (isViewerProfile)
            {
                ImGui::LoadIniSettingsFromMemory(viewer_ini.c_str(), viewer_ini.size());
            }
            else if (io.IniFilename && *io.IniFilename)
            {
                ImGui::LoadIniSettingsFromDisk(io.IniFilename);
            }
            ptr->loaded_ini_once = true;

            // Ensure default render settings entry exists (if not loaded from INI, create it)
            if (ptr->saved_render_settings.find(s_render_settings_default) == ptr->saved_render_settings.end())
            {
                auto& default_settings = ptr->saved_render_settings[s_render_settings_default];
                pnanovdb_camera_config_default(&default_settings.camera_config);
                pnanovdb_camera_state_default(&default_settings.camera_state, default_settings.is_y_up);
            }

            // Apply loaded render settings immediately after INI is loaded
            auto it = ptr->saved_render_settings.find(ptr->render_settings_name);
            if (it != ptr->saved_render_settings.end())
            {
                *ptr->render_settings = it->second;
                ptr->render_settings->sync_camera = PNANOVDB_TRUE;
            }
        }
    }

    ImGui::NewFrame();

    initializeDocking(ptr);

    pnanovdb_editor::CameraFrustum::getInstance().render(ptr);

    createMenu(ptr);

    // bool show_demo_window = true;
    // ImGui::ShowDemoWindow(&show_demo_window);

    showWindows(ptr, delta_time);

    markIniDirtyIfNewWindowsAppeared(ptr);

    pnanovdb_editor::saveLoadSettings(ptr);

    pnanovdb_editor::showFileDialogs(ptr);

    ImGui::Render();
}

ImGuiStyle* get_style(pnanovdb_imgui_instance_t* instance)
{
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();

    return &s;
}

ImGuiIO* get_io(pnanovdb_imgui_instance_t* instance)
{
    ImGuiIO& io = ImGui::GetIO();

    return &io;
}

void get_tex_data_as_rgba32(pnanovdb_imgui_instance_t* instance, unsigned char** out_pixels, int* out_width, int* out_height)
{
    ImGuiIO& io = ImGui::GetIO();

    io.Fonts->GetTexDataAsRGBA32(out_pixels, out_width, out_height);
}

ImDrawData* get_draw_data(pnanovdb_imgui_instance_t* instance)
{
    return ImGui::GetDrawData();
}

void Instance::set_default_shader(const std::string& shaderName)
{
    shader_name = shaderName;
    pending.viewport_shader_name = shaderName;
    pnanovdb_editor::CodeEditor::getInstance().setSelectedShader(shaderName);
}

void Instance::update_ini_filename_for_profile(const char* profile_name)
{
    ImGuiIO& io = ImGui::GetIO();

    current_profile_name = profile_name ? profile_name : "";

    bool isViewer = (profile_name && strcmp(profile_name, s_viewer_profile_name) == 0);
    if (isViewer)
    {
        // Viewer profile: load from memory, no file persistence
        io.IniFilename = nullptr;
        current_ini_filename = "";
    }
    else
    {
        // Generate profile-specific INI filename
        std::string ini_filename = "imgui_";
        if (profile_name && profile_name[0] != '\0')
        {
            ini_filename += profile_name;
        }
        else
        {
            current_ini_filename = "imgui";
            io.IniFilename = current_ini_filename.c_str();
            return;
        }
        ini_filename += ".ini";

        current_ini_filename = ini_filename;
        io.IniFilename = current_ini_filename.c_str();
    }
}

bool ini_window_resolution(const char* profile_name, int* width, int* height)
{
    if (!profile_name || !width || !height)
    {
        return false;
    }

    std::string ini_filename = "imgui_";
    if (profile_name && profile_name[0] != '\0')
    {
        ini_filename += profile_name;
    }
    else
    {
        ini_filename = "imgui";
    }
    ini_filename += ".ini";

    FILE* f = fopen(ini_filename.c_str(), "r");
    if (!f)
    {
        return false;
    }

    bool found_width = false;
    bool found_height = false;
    bool in_instance_settings = false;
    char line[1024];

    while (fgets(line, sizeof(line), f))
    {
        if (strstr(line, "[InstanceSettings][Settings]") != nullptr)
        {
            in_instance_settings = true;
            continue;
        }

        if (line[0] == '[' && in_instance_settings)
        {
            if (strstr(line, "[InstanceSettings][Settings]") == nullptr)
            {
                in_instance_settings = false;
            }
        }

        if (in_instance_settings)
        {
            int value = 0;
            if (sscanf(line, "WindowWidth=%d", &value) == 1)
            {
                *width = value;
                found_width = true;
            }
            else if (sscanf(line, "WindowHeight=%d", &value) == 1)
            {
                *height = value;
                found_height = true;
            }
        }

        if (found_width && found_height)
        {
            break;
        }
    }

    fclose(f);

    return found_width && found_height;
}

}

pnanovdb_imgui_instance_interface_t* get_user_imgui_instance_interface()
{
    using namespace imgui_instance_user;
    static pnanovdb_imgui_instance_interface_t iface = { PNANOVDB_REFLECT_INTERFACE_INIT(
        pnanovdb_imgui_instance_interface_t) };
    iface.create = create;
    iface.destroy = destroy;
    iface.update = update;
    iface.get_style = get_style;
    iface.get_io = get_io;
    iface.get_tex_data_as_rgba32 = get_tex_data_as_rgba32;
    iface.get_draw_data = get_draw_data;
    return &iface;
}
