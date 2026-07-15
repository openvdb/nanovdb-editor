// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <nanovdb_editor/putil/Compiler.h>

#include "editor/PipelineRegistry.h"
#include "editor/PipelineTypes.h"

#include <cstring>
#include <string>
#include <vector>

namespace
{
std::string g_pipeline_shader_diagnostics;

void capture_diagnostics(const char* message)
{
    if (message)
    {
        g_pipeline_shader_diagnostics += message;
    }
}
} // namespace

class PipelineShaderCompileTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        pnanovdb_compiler_load(&compiler);
        if (compiler.module == nullptr)
        {
            GTEST_SKIP() << "Compiler module not available";
        }
        compiler_inst = compiler.create_instance();
        ASSERT_NE(compiler_inst, nullptr);
        compiler.set_diagnostic_callback(compiler_inst, capture_diagnostics);
    }

    void TearDown() override
    {
        if (compiler_inst != nullptr)
        {
            compiler.destroy_instance(compiler_inst);
        }
        if (compiler.module != nullptr)
        {
            pnanovdb_compiler_free(&compiler);
        }
    }

    bool compile_shader(const char* shader_name)
    {
        pnanovdb_compiler_settings_t settings = {};
        pnanovdb_compiler_settings_init(&settings);
        settings.compile_target = PNANOVDB_COMPILE_TARGET_VULKAN;
        std::strcpy(settings.entry_point_name, "main");

        g_pipeline_shader_diagnostics.clear();
        return compiler.compile_shader_from_file(compiler_inst, shader_name, &settings, nullptr) != PNANOVDB_FALSE;
    }

    pnanovdb_compiler_t compiler = {};
    pnanovdb_compiler_instance_t* compiler_inst = nullptr;
};

TEST_F(PipelineShaderCompileTest, AllRegisteredPipelineShadersCompile)
{
    ASSERT_GT(pnanovdb_pipeline_get_count(), 0u)
        << "Pipeline registry is empty; the editor library did not self-register its pipelines";

    pnanovdb_uint32_t shaders_checked = 0;
    std::vector<std::string> failures;

    for (pnanovdb_uint32_t type = 0; type < pnanovdb_pipeline_type_count; ++type)
    {
        const pnanovdb_pipeline_descriptor_t* desc =
            pnanovdb_pipeline_get_descriptor(static_cast<pnanovdb_pipeline_type_t>(type));
        if (!desc)
        {
            continue;
        }

        const char* pipeline_name = desc->ui_name ? desc->ui_name : "?";
        for (pnanovdb_uint32_t s = 0; s < desc->shader_count; ++s)
        {
            const char* shader_name = desc->shaders[s].shader_name;
            if (!shader_name || shader_name[0] == '\0')
            {
                continue;
            }

            ++shaders_checked;
            const bool ok = compile_shader(shader_name);
            EXPECT_TRUE(ok)
                << "Pipeline '" << pipeline_name << "' (type " << type << ") shader failed to compile: " << shader_name
                << (g_pipeline_shader_diagnostics.empty() ? "" : "\n" + g_pipeline_shader_diagnostics);
            if (!ok)
            {
                failures.push_back(std::string(shader_name) + " [" + pipeline_name + "]");
            }
        }
    }

    EXPECT_GT(shaders_checked, 0u) << "No pipeline shaders were found to compile";

    if (!failures.empty())
    {
        std::string summary = "Pipeline shaders that failed to compile:";
        for (const auto& f : failures)
        {
            summary += "\n  - " + f;
        }
        ADD_FAILURE() << summary;
    }
}
