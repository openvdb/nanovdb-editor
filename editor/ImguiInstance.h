// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/editor/ImguiInstance.h

    \author Andrew Reidmeyer, Petra Hapalova

    \brief
*/

#pragma once

#include "ShaderParams.h"
#include "imgui/ImguiWindow.h"

#include "nanovdb_editor/putil/Editor.h"
#include "nanovdb_editor/putil/Raster.h"
#include "nanovdb_editor/putil/Shader.hpp"

#ifndef IMGUI_DEFINE_MATH_OPERATORS
#    define IMGUI_DEFINE_MATH_OPERATORS
#endif // IMGUI_DEFINE_MATH_OPERATORS

#include <imgui.h>

#include <string>
#include <atomic>
#include <map>
#include <mutex>
#include <memory>
#include <deque>

#define IMGUI_CHECKBOX_SYNC(label, var)                                                                                \
    {                                                                                                                  \
        bool temp_bool = ((var) != PNANOVDB_FALSE);                                                                    \
        if (ImGui::Checkbox((label), &temp_bool))                                                                      \
        {                                                                                                              \
            (var) = temp_bool ? PNANOVDB_TRUE : PNANOVDB_FALSE;                                                        \
        }                                                                                                              \
    }

namespace imgui_instance_user
{
static const char* s_render_settings_default = "default";
static const char* s_viewer_profile_name = "viewer";

// UI labels
static const char* VIEWPORT_CAMERA = "Viewport Camera";
static const char* SCENE_ROOT_NODE = "Viewer";

static const char* VIEWPORT_SETTINGS = "Viewport";
static const char* RENDER_SETTINGS = "Settings";
static const char* COMPILER_SETTINGS = "Compiler";
static const char* PROFILER = "Profiler";
static const char* CODE_EDITOR = "Shader Editor";
static const char* CONSOLE = "Log";
static const char* SHADER_PARAMS = "Shader Params";
static const char* BENCHMARK = "Benchmark";
static const char* FILE_HEADER = "File Header";
static const char* SCENE = "Scenes";
static const char* PROPERTIES = "Properties";

enum class ViewportOption : int
{
    NanoVDB,
    Raster2D,
    Last,
};

enum class ShaderSelectionMode : int
{
    UseViewportShader,
    UseCodeEditorShader,
    UseShaderGroup,
};

struct ViewportSettings
{
    std::string render_settings_name = s_render_settings_default;
};

} // namespace imgui_instance_user

// Forward declaration
namespace pnanovdb_editor
{
class EditorScene;
}

namespace imgui_instance_user
{

struct PendingState
{
    std::atomic<bool> update_shader = true; // needs to be initialized with true to map the shader after init
    std::atomic<bool> update_generated = false;
    bool print_slice = false;
    bool load_nvdb = false;
    bool save_nanovdb = false;
    bool find_raster_file = false;
    bool find_callable_file = false;
    bool open_file = false;
    bool save_file = false;
    std::string viewport_gaussian_view = "";
    std::string viewport_nanovdb_array = "";
    bool update_memory_stats = false;
    bool update_raster = false;
    bool find_shader_directory = false;
    ShaderSelectionMode shader_selection_mode = ShaderSelectionMode::UseViewportShader;
};

struct ProgressBar
{
    std::string text;
    float value;

    ProgressBar()
    {
        reset();
    }

    void reset()
    {
        text = "";
        value = 0.f;
    }
};

struct WindowState
{
    bool show_profiler = false;
    bool show_code_editor = false;
    bool show_console = true;
    bool show_viewport_settings = true;
    bool show_render_settings = true;
    bool show_compiler_settings = false;
    bool show_shader_params = true;
    bool show_benchmark = false;
    bool show_file_header = false;
    bool show_scene = true;
    bool show_scene_properties = true;
    bool show_about = false;
};

struct UniformState
{
};

struct Instance
{
    PendingState pending;
    WindowState window;

    // Scene management and selection (delegated to EditorScene)
    pnanovdb_editor::EditorScene* editor_scene = nullptr;

    const pnanovdb_compiler_t* compiler;
    const pnanovdb_compute_t* compute;

    pnanovdb_imgui_settings_render_t* render_settings;
    pnanovdb_compiler_settings_t compiler_settings;
    std::mutex compiler_settings_mutex;

    pnanovdb_uint64_t last_timestamp = 0llu;

    ViewportOption viewport_option = ViewportOption::Last;
    ViewportSettings viewport_settings[(int)ViewportOption::Last];

    std::string nanovdb_filepath = ""; // filename selected in the ImGuiFileDialog
    std::string raster_filepath = "";
    float raster_voxels_per_unit = 128.f;

    std::string shader_group = ""; // selected group in shader params window
    std::string shader_name = ""; // current shader name (synced with selection)

    ImVec2 dialog_size{ 768.f, 512.f };

    std::string render_settings_name = s_render_settings_default;
    std::map<std::string, pnanovdb_imgui_settings_render_t> saved_render_settings;

    std::vector<std::string> additional_shader_directories;
    std::string pending_shader_directory = "";

    ProgressBar progress;

    bool is_docking_setup = false;
    bool loaded_ini_once = false;
    std::string current_profile_name = ""; // Track current profile for switching
    std::string current_ini_filename = ""; // INI filename for current profile

    int ini_window_width = 0;
    int ini_window_height = 0;

    pnanovdb_uint32_t device_index = 0;

    void update_ini_filename_for_profile(const char* profile_name);

    bool is_viewer() const
    {
        return strcmp(render_settings->ui_profile_name, s_viewer_profile_name) == 0;
    }

    pnanovdb_shader::run_shader_func_t run_shader = [this](const char* shaderName,
                                                           pnanovdb_uint32_t grid_dim_x,
                                                           pnanovdb_uint32_t grid_dim_y,
                                                           pnanovdb_uint32_t grid_dim_z)
    {
        const uint32_t compileTarget = pnanovdb_shader::getCompileTarget(shaderName);
        if (compileTarget == PNANOVDB_COMPILE_TARGET_CPU)
        {
            assert(compiler);
            pnanovdb_compiler_instance_t* compiler_inst = compiler->create_instance();
            pnanovdb_compiler_settings_t compile_settings = {};
            pnanovdb_compiler_settings_init(&compile_settings);
            compile_settings.compile_target = PNANOVDB_COMPILE_TARGET_CPU;
            compiler->compile_shader_from_file(compiler_inst, shaderName, &compiler_settings, nullptr);

            UniformState uniformState = {};
            compiler->execute_cpu(
                compiler_inst, shaderName, grid_dim_x, grid_dim_y, grid_dim_z, nullptr, (void*)&uniformState);
            compiler->destroy_instance(compiler_inst);
        }
        else if (compileTarget == PNANOVDB_COMPILE_TARGET_VULKAN)
        {
            assert(compute);
            // compute->dispatch_shader_on_array(compute, compute_device, shaderName, grid_dim_x, grid_dim_y,
            // grid_dim_z);
        }
    };
};

PNANOVDB_CAST_PAIR(pnanovdb_imgui_instance_t, Instance)

/*!
 * \brief Read saved window resolution from INI file
 *
 * This function reads the WindowWidth and WindowHeight values from the
 * [InstanceSettings][Settings] section of the profile-specific INI file.
 * The window resolution is automatically saved when:
 * - The editor is closed normally
 * - The user manually saves settings via Settings > Save Ini menu
 *
 * The saved resolution will be automatically applied on the next startup.
 *
 * \param profile_name The UI profile name (e.g., "default", "viewer")
 * \param width Pointer to store the loaded width (if found)
 * \param height Pointer to store the loaded height (if found)
 * \return true if resolution was found and loaded, false otherwise
 */
bool ini_window_resolution(const char* profile_name, int* width, int* height);

}

pnanovdb_imgui_instance_interface_t* get_user_imgui_instance_interface();
