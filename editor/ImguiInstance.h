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

namespace imgui_instance_user
{
static const char* s_render_settings_default = "default";

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

struct PendingState
{
    std::atomic<bool> update_shader = true; // needs to be initialized with true to map the shader after init
    std::atomic<bool> update_generated = false;
    bool capture_image = false;
    bool print_slice = false;
    bool load_nvdb = false;
    bool save_nanovdb = false;
    bool find_raster_file = false;
    bool find_callable_file = false;
    bool open_file = false;
    bool save_file = false;
    bool load_camera = false; // load camera state in editor update loop
    bool save_camera = true; // save default camera first
    bool save_render_settings = false;
    bool load_render_settings = false;
    std::string shader_name = "";
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
    bool show_debug_draw = true;
};

struct UniformState
{
};

struct Instance
{
    PendingState pending;
    WindowState window;

    const pnanovdb_compiler_t* compiler;
    const pnanovdb_compute_t* compute;

    pnanovdb_imgui_settings_render_t* render_settings;
    pnanovdb_compiler_settings_t compiler_settings;
    std::mutex compiler_settings_mutex;

    pnanovdb_uint64_t last_timestamp = 0llu;

    ViewportOption viewport_option = ViewportOption::Last;
    ViewportSettings viewport_settings[(int)ViewportOption::Last];

    std::string shader_name = ""; // shader used for the viewport
    std::string nanovdb_filepath = ""; // filename selected in the ImGuiFileDialog
    std::string raster_filepath = "";
    float raster_voxels_per_unit = 128.f;

    ShaderParams shader_params;
    std::string shader_group = "";

    ImVec2 dialog_size{ 768.f, 512.f };

    std::string render_settings_name = s_render_settings_default;
    std::map<std::string, pnanovdb_imgui_settings_render_t> saved_render_settings;

    std::vector<std::string> viewport_shaders;

    std::vector<std::string> additional_shader_directories;
    std::string pending_shader_directory = "";

    ProgressBar progress;

    std::shared_ptr<pnanovdb_compute_array_t> nanovdb_array = nullptr;

    std::map<std::string, pnanovdb_debug_camera_t*>* debug_cameras = nullptr;
    std::string selected_debug_camera = "";

    void set_default_shader(const std::string& shaderName);

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
}

pnanovdb_imgui_instance_interface_t* get_user_imgui_instance_interface();
