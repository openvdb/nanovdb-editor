// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/editor/ShaderCompileUtils.h

    \author Petra Hapalova

    \brief
*/

#pragma once

#include "ImguiInstance.h"
#include "Console.h"
#include "ShaderMonitor.h"

namespace pnanovdb_editor
{
// Builds a shader recompilation callback that compiles the changed shader, logs diagnostics,
// updates ImGui instance pending flags, and optionally reuses a provided compiler instance.
// If sharedCompilerInstance is null, a new instance is created and destroyed per invocation.
inline ShaderCallback get_shader_recompile_callback(imgui_instance_user::Instance* instance,
                                                    const pnanovdb_compiler_t* compiler,
                                                    pnanovdb_compiler_instance_t* sharedCompilerInstance = nullptr)
{
    return [instance, compiler, sharedCompilerInstance](const std::string& path)
    {
        std::lock_guard<std::mutex> lock(instance->compiler_settings_mutex);

        pnanovdb_compiler_instance_t* compilerInst =
            sharedCompilerInstance ? sharedCompilerInstance : compiler->create_instance();

        std::string shader_name = pnanovdb_shader::getShaderName(path.c_str());
        Console::getInstance().addLog("Compiling shader: %s...", shader_name.c_str());

        bool to_hlsl = instance->compiler_settings.hlsl_output == PNANOVDB_TRUE;

        // Optionally compile to HLSL first
        if (to_hlsl)
        {
            pnanovdb_bool_t hlsl_ok =
                compiler->compile_shader_from_file(compilerInst, path.c_str(), &instance->compiler_settings, nullptr);
            if (!hlsl_ok)
            {
                Console::getInstance().addLog("Failed to compile shader to HLSL: %s", shader_name.c_str());
            }
            instance->pending.update_generated = true;
            instance->compiler_settings.hlsl_output = PNANOVDB_FALSE;
        }

        pnanovdb_bool_t shader_updated = PNANOVDB_FALSE;
        bool ok = compiler->compile_shader_from_file(
            compilerInst, path.c_str(), &instance->compiler_settings, &shader_updated);
        if (ok)
        {
            Console::getInstance().addLog("Compilation successful: %s", shader_name.c_str());
        }
        else
        {
            shader_updated = PNANOVDB_TRUE;
            Console::getInstance().addLog("Failed to compile shader: %s", shader_name.c_str());
        }

        if (to_hlsl)
        {
            instance->compiler_settings.hlsl_output = PNANOVDB_TRUE;
        }

        if (shader_updated == PNANOVDB_TRUE)
        {
            if (instance->compiler_settings.compile_target == PNANOVDB_COMPILE_TARGET_CPU)
            {
                instance->pending.update_generated = true;
            }
            // Always trigger shader update when a shader is recompiled
            instance->pending.update_shader = true;
        }

        if (!sharedCompilerInstance)
        {
            compiler->destroy_instance(compilerInst);
        }
    };
}
}
