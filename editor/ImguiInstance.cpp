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

#include <ImGuiFileDialog.h>

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

    ImGui::DestroyContext();

    delete ptr;
}

static void initializeDocking()
{
    ImGuiID dockspace_id = 1u;
    ImGui::DockSpaceOverViewport(dockspace_id, nullptr, ImGuiDockNodeFlags_PassthruCentralNode);

    static bool is_docking_setup = false;
    if (!is_docking_setup)
    {
        // setup docking once
        is_docking_setup = true;

        float window_width = ImGui::GetIO().DisplaySize.x;
        float window_height = ImGui::GetIO().DisplaySize.y;

        float left_dock_width = window_width * 0.25f;
        float right_dock_width = window_width * 0.20f;
        float bottom_dock_height = window_height * 0.2f;

        // clear existing layout
        ImGui::DockBuilderRemoveNode(dockspace_id);
        ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);

        // create dock spaces for various windows, the order matters!
        ImGuiID dock_id_right_far =
            ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Right, 0.f, nullptr, &dockspace_id);
        ImGui::DockBuilderSetNodeSize(dock_id_right_far, ImVec2(right_dock_width, window_height));
        ImGui::DockBuilderDockWindow(CODE_EDITOR, dock_id_right_far);
        ImGui::DockBuilderDockWindow(PROFILER, dock_id_right_far);
        ImGui::DockBuilderDockWindow(FILE_HEADER, dock_id_right_far);

        ImGuiID dock_id_right = ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Right, 0.f, nullptr, &dockspace_id);
        ImGui::DockBuilderSetNodeSize(dock_id_right, ImVec2(right_dock_width, window_height));

        ImGuiID dock_id_right_top = dock_id_right;
        ImGuiID dock_id_right_bottom =
            ImGui::DockBuilderSplitNode(dock_id_right_top, ImGuiDir_Down, 0.6f, nullptr, &dock_id_right_top);

        ImGui::DockBuilderDockWindow(SCENE, dock_id_right_top);
        ImGui::DockBuilderDockWindow(PROPERTIES, dock_id_right_bottom);
        ImGui::DockBuilderDockWindow(CAMERA_VIEW, dock_id_right_bottom);

        ImGuiID dock_id_bottom = ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Down, 0.f, nullptr, &dockspace_id);
        ImGui::DockBuilderSetNodeSize(dock_id_bottom, ImVec2(window_width, bottom_dock_height));
        ImGui::DockBuilderDockWindow(CONSOLE, dock_id_bottom);

        ImGuiID dock_id_left = ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Left, 0.f, nullptr, &dockspace_id);
        ImGui::DockBuilderSetNodeSize(dock_id_left, ImVec2(left_dock_width, window_height));
        ImGui::DockBuilderDockWindow(VIEWPORT_SETTINGS, dock_id_left);
        ImGui::DockBuilderDockWindow(RENDER_SETTINGS, dock_id_left);
        ImGui::DockBuilderDockWindow(COMPILER_SETTINGS, dock_id_left);

        ImGuiID dock_id_left_bottom =
            ImGui::DockBuilderSplitNode(dock_id_left, ImGuiDir_Down, 0.f, nullptr, &dock_id_left);
        ImGui::DockBuilderSetNodeSize(dock_id_left_bottom, ImVec2(left_dock_width, window_height));
        ImGui::DockBuilderDockWindow(SHADER_PARAMS, dock_id_left_bottom);
        ImGui::DockBuilderDockWindow(BENCHMARK, dock_id_left_bottom);

        ImGui::DockBuilderFinish(dockspace_id);
    }
}

static void createMenu(Instance* ptr)
{
    if (ImGui::BeginMainMenuBar())
    {
        if (ImGui::BeginMenu("File"))
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
            ImGui::MenuItem(CAMERA_VIEW, "", &ptr->window.show_camera_view);
            ImGui::MenuItem(PROPERTIES, "", &ptr->window.show_scene_properties);
            ImGui::MenuItem(BENCHMARK, "", &ptr->window.show_benchmark);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Settings"))
        {
            ImGui::MenuItem(VIEWPORT_SETTINGS, "", &ptr->window.show_viewport_settings);
            ImGui::MenuItem(RENDER_SETTINGS, "", &ptr->window.show_render_settings);
            ImGui::MenuItem(COMPILER_SETTINGS, "", &ptr->window.show_compiler_settings);
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }
}

static void showWindows(Instance* ptr, float delta_time)
{
    showViewportSettingsWindow(ptr);
    showRenderSettingsWindow(ptr);
    showCompilerSettingsWindow(ptr);

    showShaderParamsWindow(ptr);
    showBenchmarkWindow(ptr);

    showSceneWindow(ptr);
    showPropertiesWindow(ptr);
    showCameraViewWindow(ptr);

    showFileHeaderWindow(ptr);
    showCodeEditorWindow(ptr);
    showProfilerWindow(ptr, delta_time);

    showConsoleWindow(ptr);
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

    ImGui::NewFrame();

    initializeDocking();

    drawCameraFrustums(ptr);

    createMenu(ptr);

    // bool show_demo_window = true;
    // ImGui::ShowDemoWindow(&show_demo_window);

    showWindows(ptr, delta_time);

    if (ptr->pending.update_generated)
    {
        pnanovdb_editor::CodeEditor::getInstance().updateViewer();
        ptr->pending.update_generated = false;
    }

    if (ptr->pending.save_render_settings)
    {
        ptr->pending.save_render_settings = false;

        // Mark settings as dirty to trigger save
        ImGui::MarkIniSettingsDirty();

        pnanovdb_editor::Console::getInstance().addLog("Render settings '%s' saved", ptr->render_settings_name.c_str());
    }
    if (ptr->pending.load_render_settings)
    {
        ptr->pending.load_render_settings = false;

        auto it = ptr->saved_render_settings.find(ptr->render_settings_name);
        if (it != ptr->saved_render_settings.end())
        {
            // Copy saved camera state to current camera state
            ptr->render_settings_name = it->first;
            // pnanovdb_editor::Console::getInstance().addLog("Render settings '%s' loaded",
            // ptr->render_settings_name.c_str());
        }
        else
        {
            pnanovdb_editor::Console::getInstance().addLog(
                "Render settings '%s' not found", ptr->render_settings_name.c_str());
        }
    }

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
