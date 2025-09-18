// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <nanovdb_editor/putil/Compiler.h>
#include <filesystem>
#include <cstring>

TEST(NanoVDBEditor, ShaderCompilesViaDynamicCompiler)
{
    const std::filesystem::path shader = std::filesystem::path(__FILE__).parent_path() / "shaders" / "test.slang";
    const std::string shader_path = shader.string();

    pnanovdb_compiler_t compiler = {};
    pnanovdb_compiler_load(&compiler);

    if (compiler.module == nullptr)
    {
        FAIL() << "Compiler module not available";
    }

    pnanovdb_compiler_instance_t* compiler_inst = compiler.create_instance();
    ASSERT_NE(compiler_inst, nullptr);

    pnanovdb_compiler_settings_t compile_settings = {};
    pnanovdb_compiler_settings_init(&compile_settings);
    compile_settings.compile_target = PNANOVDB_COMPILE_TARGET_VULKAN;
    std::strcpy(compile_settings.entry_point_name, "computeMain");

    pnanovdb_bool_t result =
        compiler.compile_shader_from_file(compiler_inst, shader_path.c_str(), &compile_settings, nullptr);

    compiler.destroy_instance(compiler_inst);
    pnanovdb_compiler_free(&compiler);

    ASSERT_NE(result, PNANOVDB_FALSE) << "Compilation of shader failed: " << shader_path;
}
