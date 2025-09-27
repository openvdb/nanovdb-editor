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
#include "CodeEditor.h"
#include "Console.h"
#include "Node2Verify.h"
#include "Profiler.h"
#include "RenderSettingsHandler.h"
#include "InstanceSettingsHandler.h"
#include "ShaderMonitor.h"
#include "ShaderCompileUtils.h"

#include <ImGuiFileDialog.h>

#include <stdio.h>
#include <imgui_internal.h> // for the docking branch
#include "misc/cpp/imgui_stdlib.h" // for std::string text input

#if defined(_WIN32)
#    include <Windows.h>
#else
#    include <time.h>
#endif

#include <filesystem>
#include <algorithm>
#include <cmath>
#include <limits>

#define IMGUI_CHECKBOX_SYNC(label, var)                                                                                \
    {                                                                                                                  \
        bool temp_bool = ((var) != PNANOVDB_FALSE);                                                                    \
        if (ImGui::Checkbox((label), &temp_bool))                                                                      \
        {                                                                                                              \
            (var) = temp_bool ? PNANOVDB_TRUE : PNANOVDB_FALSE;                                                        \
        }                                                                                                              \
    }

const float EPSILON = 1e-6f;

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

static const char* VIEWPORT_SETTINGS = "Viewport";
static const char* RENDER_SETTINGS = "Render";
static const char* COMPILER_SETTINGS = "Compiler";
static const char* PROFILER = "Profiler";
static const char* CODE_EDITOR = "Shader Editor";
static const char* CONSOLE = "Output";
static const char* SHADER_PARAMS = "Params";
static const char* BENCHMARK = "Benchmark";
static const char* FILE_HEADER = "File Header";
static const char* CAMERA_VIEWS = "Camera Views";

static const char* getGridTypeName(uint32_t gridType)
{
    switch (gridType)
    {
    case 0:
        return "UNKNOWN";
    case 1:
        return "FLOAT";
    case 2:
        return "DOUBLE";
    case 3:
        return "INT16";
    case 4:
        return "INT32";
    case 5:
        return "INT64";
    case 6:
        return "VEC3F";
    case 7:
        return "VEC3D";
    case 8:
        return "MASK";
    case 9:
        return "HALF";
    case 10:
        return "UINT32";
    case 11:
        return "BOOLEAN";
    case 12:
        return "RGBA8";
    case 13:
        return "FP4";
    case 14:
        return "FP8";
    case 15:
        return "FP16";
    case 16:
        return "FPN";
    case 17:
        return "VEC4F";
    case 18:
        return "VEC4D";
    case 19:
        return "INDEX";
    case 20:
        return "ONINDEX";
    case 21:
        return "INDEXMASK";
    case 22:
        return "ONINDEXMASK";
    case 23:
        return "POINTINDEX";
    case 24:
        return "VEC3U8";
    case 25:
        return "VEC3U16";
    case 26:
        return "UINT8";
    case 27:
        return "NODE2";
    default:
        return "INVALID";
    }
}

static const char* getGridClassName(uint32_t gridClass)
{
    switch (gridClass)
    {
    case 0:
        return "UNKNOWN";
    case 1:
        return "LEVEL_SET";
    case 2:
        return "FOG_VOLUME";
    case 3:
        return "STAGGERED";
    case 4:
        return "POINT_INDEX";
    case 5:
        return "POINT_DATA";
    case 6:
        return "TOPOLOGY";
    case 7:
        return "VOXEL_VOLUME";
    case 8:
        return "INDEX_GRID";
    case 9:
        return "TENSOR_GRID";
    default:
        return "INVALID";
    }
}

// Helper function to convert grid name from two uint32_t values to string
static std::string getGridNameString(uint32_t name0, uint32_t name1)
{
    char nameStr[9] = { 0 }; // 8 chars + null terminator

    // Pack the two uint32_t values into a char array (little-endian)
    nameStr[0] = (char)(name0 & 0xFF);
    nameStr[1] = (char)((name0 >> 8) & 0xFF);
    nameStr[2] = (char)((name0 >> 16) & 0xFF);
    nameStr[3] = (char)((name0 >> 24) & 0xFF);
    nameStr[4] = (char)(name1 & 0xFF);
    nameStr[5] = (char)((name1 >> 8) & 0xFF);
    nameStr[6] = (char)((name1 >> 16) & 0xFF);
    nameStr[7] = (char)((name1 >> 24) & 0xFF);

    // Find the actual end of the string (null terminator)
    for (int i = 0; i < 8; i++)
    {
        if (nameStr[i] == '\0')
        {
            break;
        }
        // Replace non-printable characters with '?'
        if (nameStr[i] < 32 || nameStr[i] > 126)
        {
            nameStr[i] = '?';
        }
    }

    return std::string(nameStr);
}

static std::string getFullGridName(pnanovdb_buf_t buf, pnanovdb_grid_handle_t grid, uint32_t flags)
{
    if (flags & (1 << 0)) // PNANOVDB_GRID_FLAGS_HAS_LONG_GRID_NAME
    {
        uint32_t blindMetadataCount = pnanovdb_grid_get_blind_metadata_count(buf, grid);
        int64_t blindMetadataOffset = pnanovdb_grid_get_blind_metadata_offset(buf, grid);

        if (blindMetadataCount > 0 && blindMetadataOffset != 0)
        {
            // Search through blind metadata for grid name
            for (uint32_t i = 0; i < blindMetadataCount; ++i)
            {
                pnanovdb_gridblindmetadata_handle_t metadata;
                metadata.address = pnanovdb_address_offset(
                    grid.address, blindMetadataOffset + i * 288); // 288 = PNANOVDB_GRIDBLINDMETADATA_SIZE

                uint32_t dataClass = pnanovdb_gridblindmetadata_get_data_class(buf, metadata);
                if (dataClass == 3) // PNANOVDB_GRIDBLINDMETADATA_CLASS_GRID_NAME
                {
                    int64_t dataOffset = pnanovdb_gridblindmetadata_get_data_offset(buf, metadata);
                    pnanovdb_address_t nameAddress = pnanovdb_address_offset(metadata.address, dataOffset);

                    // Read the string from blind data
                    uint64_t valueCount = pnanovdb_gridblindmetadata_get_value_count(buf, metadata);
                    std::string longName;
                    longName.reserve(valueCount);

                    for (uint64_t j = 0; j < valueCount && j < 1024; ++j) // Safety limit
                    {
                        uint8_t c = pnanovdb_read_uint8(buf, pnanovdb_address_offset(nameAddress, j));
                        if (c == '\0')
                            break;
                        if (c >= 32 && c <= 126) // Printable characters only
                        {
                            longName += (char)c;
                        }
                        else
                        {
                            longName += '?';
                        }
                    }

                    return longName;
                }
            }
        }

        // Fallback: if we couldn't find the long name in blind data, show error
        return "[Long name not found in blind data]";
    }
    else
    {
        // Standard short name (up to 256 characters)
        uint32_t name0 = pnanovdb_grid_get_grid_name(buf, grid, 0);
        uint32_t name1 = pnanovdb_grid_get_grid_name(buf, grid, 1);
        return getGridNameString(name0, name1);
    }
}

static const char* getMagicTypeName(uint64_t magic)
{
    if (magic == 0x304244566f6e614eULL)
        return "NanoVDB0";
    if (magic == 0x314244566f6e614eULL)
        return "NanoVDB1 (Grid)";
    if (magic == 0x324244566f6e614eULL)
        return "NanoVDB2 (File)";
    if ((magic & 0xFFFFFFFF) == 0x56444220UL)
        return "OpenVDB";
    return "Unknown";
}

static std::string getVersionString(uint32_t version)
{
    uint32_t major = (version >> 16) & 0xFFFF;
    uint32_t minor = (version >> 8) & 0xFF;
    uint32_t patch = version & 0xFF;

    char versionStr[32];
    snprintf(versionStr, sizeof(versionStr), "%u.%u.%u", major, minor, patch);
    return std::string(versionStr);
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
        float right_dock_width = window_width * 0.33f;
        float bottom_dock_height = window_height * 0.2f;

        // clear existing layout
        ImGui::DockBuilderRemoveNode(dockspace_id);
        ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);

        // create dock spaces for various windows, the order matters!
        ImGuiID dock_id_right = ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Right, 0.f, nullptr, &dockspace_id);
        ImGui::DockBuilderSetNodeSize(dock_id_right, ImVec2(right_dock_width, window_height));
        ImGui::DockBuilderDockWindow(CODE_EDITOR, dock_id_right);
        ImGui::DockBuilderDockWindow(PROFILER, dock_id_right);
        ImGui::DockBuilderDockWindow(FILE_HEADER, dock_id_right);

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
        ImGui::DockBuilderDockWindow(CAMERA_VIEWS, dock_id_left_bottom);
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
            ImGui::MenuItem(CAMERA_VIEWS, "", &ptr->window.show_views);
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
    if (ptr->window.show_viewport_settings)
    {
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
                    ptr->pending.load_camera = true;
                }
                ImGui::SameLine();
                if (ImGui::RadioButton("Raster2D", selectedOption == ViewportOption::Raster2D))
                {
                    ptr->viewport_option = ViewportOption::Raster2D;
                    ptr->render_settings_name = ptr->viewport_settings[(int)ptr->viewport_option].render_settings_name;
                    ptr->pending.load_camera = true;
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
                            ptr->pending.load_camera = true;

                            // clear selected debug camera when switching to saved camera
                            ptr->selected_camera_view.clear();
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
                            // save camera state in editor update loop
                            ptr->pending.save_camera = true;
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

            ImGui::Text("Gaussian File");
            {
                ImGui::BeginGroup();
                ImGui::InputText("##viewport_raster_file", &ptr->raster_filepath);
                ImGui::SameLine();
                if (ImGui::Button("...##open_raster_file"))
                {
                    ptr->pending.find_raster_file = true;

                    IGFD::FileDialogConfig config;
                    config.path = ".";

                    ImGuiFileDialog::Instance()->OpenDialog("OpenRasterFileDlgKey", "Open Gaussian File",
                                                            "Gaussian Files (*.npy *.npz *.ply){.npy,.npz,.ply}", config);
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
                        pnanovdb_editor::Console::getInstance().addLog(
                            "Opening file '%s'", ptr->nanovdb_filepath.c_str());
                        ptr->pending.load_nvdb = true;
                    }
                    ImGui::EndGroup();
                }
            }
            ImGui::End();
        }
        else
        {
            ImGui::End();
        }
    }

    if (ptr->window.show_render_settings)
    {
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
                            ptr->pending.shader_name = shader;
                        }
                    }
                    ImGui::EndCombo();
                }
                ImGui::InputText("##viewport_shader_name", &ptr->pending.shader_name);
                ImGui::SameLine();
                if (ImGui::Button("Update"))
                {
                    pnanovdb_editor::CodeEditor::getInstance().setSelectedShader(ptr->pending.shader_name);
                    ptr->shader_name = ptr->pending.shader_name;
                    ptr->pending.update_shader = true;
                }
                ImGui::EndGroup();
            }

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

                IMGUI_CHECKBOX_SYNC("Upside down", settings->is_upside_down);
            }
            IMGUI_CHECKBOX_SYNC("Video Encode", settings->enable_encoder);
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
            ImGui::InputText("Server Address", settings->server_address, 256u);
            ImGui::InputInt("Server Port", &settings->server_port);

            ImGui::End();
        }
        else
        {
            ImGui::End();
        }
    }

    if (ptr->window.show_compiler_settings)
    {
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
                        pnanovdb_editor::Console::getInstance().addLog(
                            "Removed shader directory: %s", dirToRemove.c_str());
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

            ImGui::End();
        }
        else
        {
            ImGui::End();
        }
    }

    if (ptr->window.show_shader_params)
    {
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
            ImGui::Separator();

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

            ImGui::End();
        }
        else
        {
            ImGui::End();
        }
    }

    if (ptr->window.show_views)
    {
        if (ImGui::Begin(CAMERA_VIEWS, &ptr->window.show_views))
        {
            if (ptr->camera_views && !ptr->camera_views->empty())
            {
                const char* preview = ptr->selected_camera_view.empty() ? "Select..." : ptr->selected_camera_view.c_str();
                if (ImGui::BeginCombo("Viewport Camera", preview))
                {
                    for (const auto& cameraPair : *ptr->camera_views)
                    {
                        const std::string& name = cameraPair.first;
                        bool is_selected = (ptr->selected_camera_view == name);
                        if (ImGui::Selectable(name.c_str(), is_selected))
                        {
                            ptr->selected_camera_view = name;
                            pnanovdb_camera_view_t* cam = cameraPair.second;
                            if (cam)
                            {
                                ptr->render_settings->camera_state = cam->state;
                                ptr->render_settings->camera_config = cam->config;
                                ptr->render_settings->is_projection_rh = cam->config.is_projection_rh;
                                ptr->render_settings->is_orthographic = cam->config.is_orthographic;
                                ptr->render_settings->is_reverse_z = cam->config.is_reverse_z;
                                ptr->render_settings->sync_camera = PNANOVDB_TRUE;
                            }
                        }
                        if (is_selected)
                        {
                            ImGui::SetItemDefaultFocus();
                        }
                    }
                    ImGui::EndCombo();
                }
            }
            if (!ptr->camera_views)
            {
                ImGui::TextDisabled("No cameras available");
            }
            else if (ptr->camera_views->empty())
            {
                ImGui::TextDisabled("No cameras added");
            }
            else
            {
                if (ImGui::BeginChild("##CameraList", ImVec2(0, 200), true, ImGuiWindowFlags_AlwaysVerticalScrollbar))
                {
                    for (auto& cameraPair : *ptr->camera_views)
                    {
                        pnanovdb_camera_view_t* camera = cameraPair.second;
                        if (!camera)
                        {
                            continue;
                        }

                        const std::string& cameraName = cameraPair.first;
                        bool isVisible = (camera->is_visible != PNANOVDB_FALSE);
                        if (ImGui::Checkbox(("##Visible" + cameraName).c_str(), &isVisible))
                        {
                            camera->is_visible = isVisible ? PNANOVDB_TRUE : PNANOVDB_FALSE;
                        }
                        ImGui::SameLine();
                        bool isSelected = (ptr->selected_camera_view_frustum == cameraName);
                        if (ImGui::Selectable(cameraName.c_str(), isSelected))
                        {
                            ptr->selected_camera_view_frustum = cameraName;
                        }
                    }

                    ImVec2 avail = ImGui::GetContentRegionAvail();
                    if (avail.y > 0.0f)
                    {
                        if (ImGui::InvisibleButton("##ClearCameraFrustumSelectionArea", avail))
                        {
                            ptr->selected_camera_view_frustum.clear();
                        }
                    }
                }
                ImGui::EndChild();
            }
            if (ptr->selected_camera_view_frustum.empty())
            {
                ImGui::TextDisabled("No camera selected");
            }
            else
            {
                pnanovdb_camera_view_t* camera = ptr->camera_views->at(ptr->selected_camera_view_frustum);
                if (camera)
                {
                    pnanovdb_vec3_t eyePosition = camera->state.position;
                    eyePosition.x -= camera->state.eye_direction.x * camera->state.eye_distance_from_position;
                    eyePosition.y -= camera->state.eye_direction.y * camera->state.eye_distance_from_position;
                    eyePosition.z -= camera->state.eye_direction.z * camera->state.eye_distance_from_position;

                    ImGui::Text("Position: (%.3f, %.3f, %.3f)", eyePosition.x, eyePosition.y, eyePosition.z);
                    IMGUI_CHECKBOX_SYNC("Reversed Z", camera->config.is_reverse_z);
                    IMGUI_CHECKBOX_SYNC("Orthographic", camera->config.is_orthographic);
                    ImGui::DragFloat("FOV", &camera->config.fov_angle_y, 0.01f, 0.f, M_PI_2);
                    ImGui::DragFloat("Axis Length", &camera->axis_length, 1.f, 0.f, 100.f);
                    ImGui::DragFloat("Axis Thickness", &camera->axis_thickness, 0.1f, 0.f, 10.f);
                    ImGui::DragFloat("Axis Scale", &camera->axis_scale, 0.1f, 0.f, 10.f);
                    ImGui::DragFloat("Frustum Line Width", &camera->frustum_line_width, 0.1f, 0.f, 10.f);
                    ImGui::DragFloat("Frustum Scale", &camera->frustum_scale, 0.1f, 0.f, 10.f);
                }
            }

            ImGui::End();
        }
        else
        {
            ImGui::End();
        }
    }

    if (ptr->window.show_benchmark)
    {
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
            ImGui::End();
        }
        else
        {
            ImGui::End();
        }
    }

    if (ptr->window.show_file_header)
    {
        if (ImGui::Begin(FILE_HEADER, &ptr->window.show_file_header))
        {
            if (ptr->nanovdb_array && ptr->nanovdb_array->data && ptr->nanovdb_array->element_count > 0)
            {
                pnanovdb_buf_t buf =
                    pnanovdb_make_buf((pnanovdb_uint32_t*)ptr->nanovdb_array->data, ptr->nanovdb_array->element_count);
                pnanovdb_grid_handle_t grid = { pnanovdb_address_null() };

                ImGui::Text("NanoVDB Grid Information");
                ImGui::Separator();

                if (ImGui::CollapsingHeader("Grid Header", ImGuiTreeNodeFlags_DefaultOpen))
                {
                    uint64_t magic = pnanovdb_grid_get_magic(buf, grid);
                    ImGui::Text("Magic: %s", getMagicTypeName(magic));

                    uint32_t version = pnanovdb_grid_get_version(buf, grid);
                    std::string versionStr = getVersionString(version);
                    ImGui::Text("Version: %s", versionStr.c_str());

                    uint32_t flags = pnanovdb_grid_get_flags(buf, grid);
                    if (ImGui::CollapsingHeader("Grid Flags"))
                    {
                        ImGui::Indent();

                        struct FlagInfo
                        {
                            uint32_t bit;
                            const char* name;
                            const char* description;
                        };

                        FlagInfo flagInfos[] = { { 1 << 0, "Long Grid Name", "Grid name > 256 characters" },
                                                 { 1 << 1, "Bounding Box", "Nodes contain bounding boxes" },
                                                 { 1 << 2, "Min/Max Values", "Nodes contain min/max statistics" },
                                                 { 1 << 3, "Average Values", "Nodes contain average statistics" },
                                                 { 1 << 4, "Std Deviation", "Nodes contain standard deviation" },
                                                 { 1 << 5, "Breadth First", "Memory layout is breadth-first" } };

                        bool hasAnyFlags = false;
                        for (const auto& flagInfo : flagInfos)
                        {
                            bool isSet = (flags & flagInfo.bit) != 0;
                            if (isSet)
                                hasAnyFlags = true;

                            ImGui::PushStyleColor(ImGuiCol_Text, isSet ? IM_COL32(144, 238, 144, 255) : // Light green
                                                                                                        // for enabled
                                                                     IM_COL32(128, 128, 128, 255)); // Gray for disabled

                            ImGui::Text("[%s] %s", isSet ? "X" : " ", flagInfo.name);
                            if (ImGui::IsItemHovered())
                            {
                                ImGui::SetTooltip("%s", flagInfo.description);
                            }
                            ImGui::PopStyleColor();
                        }

                        if (!hasAnyFlags)
                        {
                            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(128, 128, 128, 255));
                            ImGui::Text("[ ] No flags set");
                            ImGui::PopStyleColor();
                        }

                        ImGui::Unindent();
                    }
                    ImGui::Text("Grid Index: %u", pnanovdb_grid_get_grid_index(buf, grid));
                    ImGui::Text("Grid Count: %u", pnanovdb_grid_get_grid_count(buf, grid));
                    ImGui::Text("Grid Size: %llu bytes", (unsigned long long)pnanovdb_grid_get_grid_size(buf, grid));
                    uint32_t gridType = pnanovdb_grid_get_grid_type(buf, grid);
                    ImGui::Text("Grid Type: %s", getGridTypeName(gridType));
                    uint32_t gridClass = pnanovdb_grid_get_grid_class(buf, grid);
                    ImGui::Text("Grid Class: %s", getGridClassName(gridClass));

                    std::string gridNameStr = getFullGridName(buf, grid, flags);
                    ImGui::Text("Grid Name: \"%s\"", gridNameStr.c_str());
                    if (flags & (1 << 0)) // HasLongGridName flag
                    {
                        ImGui::SameLine();
                        ImGui::TextColored(ImVec4(0.5f, 0.7f, 1.0f, 1.0f), "(Long Name)");
                        if (ImGui::IsItemHovered())
                        {
                            ImGui::SetTooltip("Grid name longer than 256 characters, stored in blind metadata");
                        }
                    }

                    ImGui::Text("World BBox Min: (%.3f, %.3f, %.3f)", pnanovdb_grid_get_world_bbox(buf, grid, 0),
                                pnanovdb_grid_get_world_bbox(buf, grid, 1), pnanovdb_grid_get_world_bbox(buf, grid, 2));
                    ImGui::Text("World BBox Max: (%.3f, %.3f, %.3f)", pnanovdb_grid_get_world_bbox(buf, grid, 3),
                                pnanovdb_grid_get_world_bbox(buf, grid, 4), pnanovdb_grid_get_world_bbox(buf, grid, 5));

                    ImGui::Text("Voxel Size: (%.6f, %.6f, %.6f)", pnanovdb_grid_get_voxel_size(buf, grid, 0),
                                pnanovdb_grid_get_voxel_size(buf, grid, 1), pnanovdb_grid_get_voxel_size(buf, grid, 2));
                }

                if (ImGui::CollapsingHeader("Tree Information", ImGuiTreeNodeFlags_DefaultOpen))
                {
                    pnanovdb_tree_handle_t tree = pnanovdb_grid_get_tree(buf, grid);
                    ImGui::Text("Active Voxel Count: %llu", (unsigned long long)pnanovdb_tree_get_voxel_count(buf, tree));
                    ImGui::Text("Leaf Node Count: %u", pnanovdb_tree_get_node_count_leaf(buf, tree));
                    ImGui::Text("Lower Internal Node Count: %u", pnanovdb_tree_get_node_count_lower(buf, tree));
                    ImGui::Text("Upper Internal Node Count: %u", pnanovdb_tree_get_node_count_upper(buf, tree));
                    ImGui::Text("Lower Tile Count: %u", pnanovdb_tree_get_tile_count_lower(buf, tree));
                    ImGui::Text("Upper Tile Count: %u", pnanovdb_tree_get_tile_count_upper(buf, tree));
                    ImGui::Text("Root Tile Count: %u", pnanovdb_tree_get_tile_count_root(buf, tree));
                }

                if (ImGui::CollapsingHeader("Root Information", ImGuiTreeNodeFlags_DefaultOpen))
                {
                    pnanovdb_tree_handle_t tree = pnanovdb_grid_get_tree(buf, grid);
                    pnanovdb_root_handle_t root = pnanovdb_tree_get_root(buf, tree);

                    pnanovdb_coord_t root_bbox_min = pnanovdb_root_get_bbox_min(buf, root);
                    pnanovdb_coord_t root_bbox_max = pnanovdb_root_get_bbox_max(buf, root);
                    ImGui::Text("Index BBox Min: (%d, %d, %d)", root_bbox_min.x, root_bbox_min.y, root_bbox_min.z);
                    ImGui::Text("Index BBox Max: (%d, %d, %d)", root_bbox_max.x, root_bbox_max.y, root_bbox_max.z);

                    ImGui::Text("Root Tile Count: %u", pnanovdb_root_get_tile_count(buf, root));
                }

                if (ImGui::CollapsingHeader("Blind Metadata"))
                {
                    uint32_t blindMetadataCount = pnanovdb_grid_get_blind_metadata_count(buf, grid);
                    int64_t blindMetadataOffset = pnanovdb_grid_get_blind_metadata_offset(buf, grid);

                    ImGui::Text("Blind Metadata Count: %u", blindMetadataCount);
                    ImGui::Text("Blind Metadata Offset: %lld", (long long)blindMetadataOffset);

                    if (blindMetadataCount > 0 && blindMetadataOffset != 0)
                    {
                        ImGui::Indent();
                        for (uint32_t i = 0; i < blindMetadataCount && i < 10; ++i)
                        { // Limit to 10 for UI
                            pnanovdb_gridblindmetadata_handle_t metadata;
                            metadata.address = pnanovdb_address_offset(grid.address, blindMetadataOffset + i * 288);

                            uint32_t dataClass = pnanovdb_gridblindmetadata_get_data_class(buf, metadata);
                            uint32_t dataType = pnanovdb_gridblindmetadata_get_data_type(buf, metadata);
                            uint64_t valueCount = pnanovdb_gridblindmetadata_get_value_count(buf, metadata);
                            uint32_t valueSize = pnanovdb_gridblindmetadata_get_value_size(buf, metadata);

                            const char* className = "Unknown";
                            switch (dataClass)
                            {
                            case 0:
                                className = "Unknown";
                                break;
                            case 1:
                                className = "Index Array";
                                break;
                            case 2:
                                className = "Attribute Array";
                                break;
                            case 3:
                                className = "Grid Name";
                                break;
                            case 4:
                                className = "Channel Array";
                                break;
                            default:
                                className = "Invalid";
                                break;
                            }

                            ImGui::Text("Entry %u: %s (%u values, %u bytes each)", i, className, (unsigned)valueCount,
                                        valueSize);
                        }
                        if (blindMetadataCount > 10)
                        {
                            ImGui::Text("... and %u more entries", blindMetadataCount - 10);
                        }
                        ImGui::Unindent();
                    }
                    else
                    {
                        ImGui::Text("No blind metadata present");
                    }
                }

                if (ImGui::CollapsingHeader("Memory Usage"))
                {
                    ImGui::Text("Total Buffer Size: %llu bytes", (unsigned long long)(ptr->nanovdb_array->element_count *
                                                                                      ptr->nanovdb_array->element_size));
                    ImGui::Text("Element Size: %llu bytes", (unsigned long long)ptr->nanovdb_array->element_size);
                    ImGui::Text("Element Count: %llu", (unsigned long long)ptr->nanovdb_array->element_count);
                }
            }
            else
            {
                ImGui::Text("No NanoVDB file loaded");
                ImGui::Text("Please load a NanoVDB file to view debug information");
            }

            ImGui::End();
        }
        else
        {
            ImGui::End();
        }
    }

    if (ptr->window.show_code_editor)
    {
        pnanovdb_editor::CodeEditor::getInstance().setup(
            &ptr->shader_name, &ptr->pending.update_shader, ptr->dialog_size, ptr->run_shader);
        if (ImGui::Begin(
                CODE_EDITOR, &ptr->window.show_code_editor, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_MenuBar))
        {
            if (!pnanovdb_editor::CodeEditor::getInstance().render())
            {
                ptr->window.show_code_editor = false;
            }
            ImGui::End();
        }
        else
        {
            ImGui::End();
        }
    }

    if (ptr->window.show_profiler)
    {
        if (ImGui::Begin(PROFILER, &ptr->window.show_profiler))
        {
            if (!pnanovdb_editor::Profiler::getInstance().render(&ptr->pending.update_memory_stats, delta_time))
            {
                ptr->window.show_profiler = false;
            }
            ImGui::End();
        }
        else
        {
            ImGui::End();
        }
    }

    if (ptr->window.show_console)
    {
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

            ImGui::End();
        }
        else
        {
            ImGui::End();
        }
    }
}

struct CameraBasisVectors
{
    pnanovdb_vec3_t right;
    pnanovdb_vec3_t up;
    pnanovdb_vec3_t forward;
};

static void calculateFrustumCorners(pnanovdb_camera_state_t camera_state,
                                    pnanovdb_camera_config_t camera_config,
                                    float width,
                                    float height,
                                    pnanovdb_vec3_t corners[8],
                                    CameraBasisVectors* basisVectors = nullptr,
                                    float frustum_scale = 1.f)
{
    pnanovdb_vec3_t eyePosition = camera_state.position;
    eyePosition.x -= camera_state.eye_direction.x * camera_state.eye_distance_from_position;
    eyePosition.y -= camera_state.eye_direction.y * camera_state.eye_distance_from_position;
    eyePosition.z -= camera_state.eye_direction.z * camera_state.eye_distance_from_position;

    // Calculate camera basis vectors
    pnanovdb_vec3_t forward = camera_state.eye_direction;
    if (camera_config.is_reverse_z)
    {
        forward.x = -forward.x;
        forward.y = -forward.y;
        forward.z = -forward.z;
    }
    pnanovdb_vec3_t up = camera_state.eye_up;
    pnanovdb_vec3_t right = { forward.y * up.z - forward.z * up.y, forward.z * up.x - forward.x * up.z,
                              forward.x * up.y - forward.y * up.x };

    // Normalize right vector
    float rightLength = sqrt(right.x * right.x + right.y * right.y + right.z * right.z);
    if (rightLength > EPSILON)
    {
        right.x /= rightLength;
        right.y /= rightLength;
        right.z /= rightLength;
    }
    else
    {
        right = { 1.f, 0.f, 0.f };
    }

    up.x = right.y * forward.z - right.z * forward.y;
    up.y = right.z * forward.x - right.x * forward.z;
    up.z = right.x * forward.y - right.y * forward.x;

    // Normalize the recalculated up vector
    float upLength = sqrt(up.x * up.x + up.y * up.y + up.z * up.z);
    if (upLength > EPSILON)
    {
        up.x /= upLength;
        up.y /= upLength;
        up.z /= upLength;
    }
    else
    {
        up = { 0.f, 1.f, 0.f };
    }

    if (basisVectors)
    {
        basisVectors->right = right;
        basisVectors->up = up;
        basisVectors->forward = forward;
    }

    float nearPlaneClamped = std::max(camera_config.near_plane, EPSILON);
    float farPlaneClamped = std::min(camera_config.far_plane, 10000000.f);

    // Calculate frustum dimensions
    float aspectRatio = width / height;
    float nearPlane = camera_config.is_reverse_z ? farPlaneClamped : nearPlaneClamped;
    float farPlane = camera_config.is_reverse_z ? nearPlaneClamped : farPlaneClamped;

    float nearHeight, nearWidth, farHeight, farWidth;

    if (camera_config.is_orthographic)
    {
        float orthoScale = camera_state.orthographic_scale;
        nearHeight = farHeight = orthoScale * frustum_scale;
        nearWidth = farWidth = orthoScale * aspectRatio * frustum_scale;
    }
    else
    {
        float tanHalfFov = tan(camera_config.fov_angle_y * 0.5f);
        nearHeight = nearPlane * tanHalfFov * frustum_scale;
        nearWidth = nearHeight * aspectRatio;
        farHeight = farPlane * tanHalfFov * frustum_scale;
        farWidth = farHeight * aspectRatio;
    }

    // Calculate near plane corners
    pnanovdb_vec3_t nearCenter = { eyePosition.x + forward.x * nearPlane, eyePosition.y + forward.y * nearPlane,
                                   eyePosition.z + forward.z * nearPlane };

    corners[0] = { nearCenter.x - right.x * nearWidth * 0.5f - up.x * nearHeight * 0.5f,
                   nearCenter.y - right.y * nearWidth * 0.5f - up.y * nearHeight * 0.5f,
                   nearCenter.z - right.z * nearWidth * 0.5f - up.z * nearHeight * 0.5f };

    corners[1] = { nearCenter.x + right.x * nearWidth * 0.5f - up.x * nearHeight * 0.5f,
                   nearCenter.y + right.y * nearWidth * 0.5f - up.y * nearHeight * 0.5f,
                   nearCenter.z + right.z * nearWidth * 0.5f - up.z * nearHeight * 0.5f };

    corners[2] = { nearCenter.x + right.x * nearWidth * 0.5f + up.x * nearHeight * 0.5f,
                   nearCenter.y + right.y * nearWidth * 0.5f + up.y * nearHeight * 0.5f,
                   nearCenter.z + right.z * nearWidth * 0.5f + up.z * nearHeight * 0.5f };

    corners[3] = { nearCenter.x - right.x * nearWidth * 0.5f + up.x * nearHeight * 0.5f,
                   nearCenter.y - right.y * nearWidth * 0.5f + up.y * nearHeight * 0.5f,
                   nearCenter.z - right.z * nearWidth * 0.5f + up.z * nearHeight * 0.5f };

    // Calculate far plane corners
    pnanovdb_vec3_t farCenter = { eyePosition.x + forward.x * farPlane, eyePosition.y + forward.y * farPlane,
                                  eyePosition.z + forward.z * farPlane };

    corners[4] = { farCenter.x - right.x * farWidth * 0.5f - up.x * farHeight * 0.5f,
                   farCenter.y - right.y * farWidth * 0.5f - up.y * farHeight * 0.5f,
                   farCenter.z - right.z * farWidth * 0.5f - up.z * farHeight * 0.5f };

    corners[5] = { farCenter.x + right.x * farWidth * 0.5f - up.x * farHeight * 0.5f,
                   farCenter.y + right.y * farWidth * 0.5f - up.y * farHeight * 0.5f,
                   farCenter.z + right.z * farWidth * 0.5f - up.z * farHeight * 0.5f };

    corners[6] = { farCenter.x + right.x * farWidth * 0.5f + up.x * farHeight * 0.5f,
                   farCenter.y + right.y * farWidth * 0.5f + up.y * farHeight * 0.5f,
                   farCenter.z + right.z * farWidth * 0.5f + up.z * farHeight * 0.5f };

    corners[7] = { farCenter.x - right.x * farWidth * 0.5f + up.x * farHeight * 0.5f,
                   farCenter.y - right.y * farWidth * 0.5f + up.y * farHeight * 0.5f,
                   farCenter.z - right.z * farWidth * 0.5f + up.z * farHeight * 0.5f };
}

static ImVec2 projectToScreen(const pnanovdb_vec3_t& worldPos,
                              pnanovdb_camera_t* viewingCamera,
                              float screenWidth,
                              float screenHeight)
{
    pnanovdb_camera_mat_t view, proj;
    pnanovdb_camera_get_view(viewingCamera, &view);
    pnanovdb_camera_get_projection(viewingCamera, &proj, screenWidth, screenHeight);
    pnanovdb_camera_mat_t viewProj = pnanovdb_camera_mat_mul(view, proj);

    pnanovdb_vec4_t worldPos4 = { worldPos.x, worldPos.y, worldPos.z, 1.f };
    pnanovdb_vec4_t clipPos = pnanovdb_camera_vec4_transform(worldPos4, viewProj);

    if (clipPos.w > EPSILON)
    {
        float x = clipPos.x / clipPos.w;
        float y = clipPos.y / clipPos.w;

        // Convert from NDC to screen coordinates
        float screenX = (x + 1.f) * 0.5f * screenWidth;
        float screenY = (1.f - y) * 0.5f * screenHeight; // Flip Y coordinate

        return ImVec2(screenX, screenY);
    }

    return ImVec2(std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::quiet_NaN());
}

static void drawCameraFrustumOverlay(
    Instance* ptr, ImVec2 windowPos, ImVec2 windowSize, pnanovdb_camera_view_t& camera, bool isSelected = false)
{
    ImDrawList* drawList = ImGui::GetWindowDrawList();

    auto isPointFinite = [](const ImVec2& p) { return std::isfinite(p.x) && std::isfinite(p.y); };
    auto isPointInsideWindow = [&](const ImVec2& p)
    {
        return (p.x >= windowPos.x && p.x <= windowPos.x + windowSize.x && p.y >= windowPos.y &&
                p.y <= windowPos.y + windowSize.y);
    };
    auto isValidScreenPoint = [&](const ImVec2& p) { return isPointFinite(p) && isPointInsideWindow(p); };

    pnanovdb_camera_t viewingCamera = {};
    viewingCamera.state = ptr->render_settings->camera_state;
    viewingCamera.config = ptr->render_settings->camera_config;

    pnanovdb_camera_state_t target_state = camera.state;
    pnanovdb_camera_config_t target_config = camera.config;

    pnanovdb_vec3_t frustumCorners[8];
    CameraBasisVectors basisVectors;
    calculateFrustumCorners(
        target_state, target_config, windowSize.x, windowSize.y, frustumCorners, &basisVectors, camera.frustum_scale);

    ImVec2 screenCorners[8];
    for (int i = 0; i < 8; i++)
    {
        screenCorners[i] = projectToScreen(frustumCorners[i], &viewingCamera, windowSize.x, windowSize.y);
        screenCorners[i].x += windowPos.x;
        screenCorners[i].y += windowPos.y;
    }

    ImU32 lineColor =
        IM_COL32(camera.frustum_color.x, camera.frustum_color.y, camera.frustum_color.z, isSelected ? 128 : 255);
    float frustumLineThickness = camera.frustum_scale * camera.frustum_line_width;

    auto drawEdge = [&](int a, int b)
    {
        if (!isValidScreenPoint(screenCorners[a]) || !isValidScreenPoint(screenCorners[b]))
        {
            return;
        }
        drawList->AddLine(screenCorners[a], screenCorners[b], lineColor, frustumLineThickness);
    };

    // Near plane
    drawEdge(0, 1);
    drawEdge(1, 2);
    drawEdge(2, 3);
    drawEdge(3, 0);

    // Far plane
    drawEdge(4, 5);
    drawEdge(5, 6);
    drawEdge(6, 7);
    drawEdge(7, 4);

    // Sides
    drawEdge(0, 4);
    drawEdge(1, 5);
    drawEdge(2, 6);
    drawEdge(3, 7);

    pnanovdb_vec3_t eyePosition = target_state.position;
    eyePosition.x -= target_state.eye_direction.x * target_state.eye_distance_from_position;
    eyePosition.y -= target_state.eye_direction.y * target_state.eye_distance_from_position;
    eyePosition.z -= target_state.eye_direction.z * target_state.eye_distance_from_position;
    ImVec2 cameraScreenPos = projectToScreen(eyePosition, &viewingCamera, windowSize.x, windowSize.y);
    cameraScreenPos.x += windowPos.x;
    cameraScreenPos.y += windowPos.y;

    pnanovdb_vec3_t forward = basisVectors.forward;
    pnanovdb_vec3_t up = basisVectors.up;
    pnanovdb_vec3_t right = basisVectors.right;

    float axisLength = camera.axis_scale * camera.axis_length;

    pnanovdb_vec3_t xAxisEnd = { eyePosition.x + right.x * axisLength, eyePosition.y + right.y * axisLength,
                                 eyePosition.z + right.z * axisLength };
    pnanovdb_vec3_t yAxisEnd = { eyePosition.x + up.x * axisLength, eyePosition.y + up.y * axisLength,
                                 eyePosition.z + up.z * axisLength };
    pnanovdb_vec3_t zAxisEnd = { eyePosition.x + forward.x * axisLength, eyePosition.y + forward.y * axisLength,
                                 eyePosition.z + forward.z * axisLength };

    ImVec2 xAxisScreenPos = projectToScreen(xAxisEnd, &viewingCamera, windowSize.x, windowSize.y);
    ImVec2 yAxisScreenPos = projectToScreen(yAxisEnd, &viewingCamera, windowSize.x, windowSize.y);
    ImVec2 zAxisScreenPos = projectToScreen(zAxisEnd, &viewingCamera, windowSize.x, windowSize.y);

    xAxisScreenPos.x += windowPos.x;
    xAxisScreenPos.y += windowPos.y;
    yAxisScreenPos.x += windowPos.x;
    yAxisScreenPos.y += windowPos.y;
    zAxisScreenPos.x += windowPos.x;
    zAxisScreenPos.y += windowPos.y;

    float axisThickness = camera.axis_scale * camera.axis_thickness;
    float posRadius = camera.axis_scale * axisThickness;
    ImU32 posColor = IM_COL32(222, 220, 113, isSelected ? 128 : 255);

    if (isValidScreenPoint(cameraScreenPos) && isValidScreenPoint(xAxisScreenPos))
    {
        drawList->AddLine(cameraScreenPos, xAxisScreenPos, IM_COL32(255, 0, 0, 255), axisThickness); // right
    }
    if (isValidScreenPoint(cameraScreenPos) && isValidScreenPoint(yAxisScreenPos))
    {
        drawList->AddLine(cameraScreenPos, yAxisScreenPos, IM_COL32(0, 255, 0, 255), axisThickness); // up
    }
    if (isValidScreenPoint(cameraScreenPos) && isValidScreenPoint(zAxisScreenPos))
    {
        drawList->AddLine(cameraScreenPos, zAxisScreenPos, IM_COL32(0, 0, 255, 255), axisThickness); // forward
    }
    if (isValidScreenPoint(cameraScreenPos))
    {
        drawList->AddCircleFilled(cameraScreenPos, posRadius, posColor);
    }
}

static void drawCameraViews(Instance* ptr)
{
    if (!ptr->camera_views)
    {
        return;
    }

    // Get the main viewport (central docking area)
    ImGuiViewport* mainViewport = ImGui::GetMainViewport();
    ImVec2 viewportPos = mainViewport->Pos;
    ImVec2 viewportSize = mainViewport->Size;

    // Create an overlay window to draw in the main viewport area
    ImGui::SetNextWindowPos(viewportPos);
    ImGui::SetNextWindowSize(viewportSize);
    ImGui::SetNextWindowBgAlpha(0.f);

    ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoFocusOnAppearing |
                                   ImGuiWindowFlags_NoBringToFrontOnFocus;

    ImGui::Begin("Camera Frustum Overlay", nullptr, windowFlags);
    {
        ImVec2 windowPos = ImGui::GetWindowPos();
        ImVec2 windowSize = ImGui::GetWindowSize();

        for (const auto& cameraPair : *ptr->camera_views)
        {
            pnanovdb_camera_view_t* camera = cameraPair.second;
            if (!camera)
            {
                continue;
            }

            bool isOverlaySelected =
                (!ptr->selected_camera_view_frustum.empty() && ptr->selected_camera_view_frustum == cameraPair.first);

            if (camera->is_visible && ptr->selected_camera_view_frustum != cameraPair.first && !isOverlaySelected)
            {
                drawCameraFrustumOverlay(ptr, windowPos, windowSize, *camera);
            }

            if (isOverlaySelected)
            {
                drawCameraFrustumOverlay(ptr, windowPos, windowSize, *camera, true);
            }
        }
    }
    ImGui::End();
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

    drawCameraViews(ptr);

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
    pending.shader_name = shaderName;
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
