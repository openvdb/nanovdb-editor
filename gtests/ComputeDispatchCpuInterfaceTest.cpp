// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include "slang-com-helper.h"
#include "slang-com-ptr.h"
#include "slang.h"

#include <nanovdb_editor/putil/Compiler.h>

#include <array>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <string>

namespace
{
std::string g_cpu_interface_compile_diagnostics;

void capture_compiler_diagnostics(const char* message)
{
    if (message)
    {
        g_cpu_interface_compile_diagnostics += message;
    }
}
} // namespace

TEST(NanoVDBEditor, ComputeDispatchCpuInterfaceCallsHostMethod)
{
    const std::filesystem::path shader =
        std::filesystem::path(__FILE__).parent_path() / "shaders" / "test_cpu_interface.slang";
    const std::string shader_path = shader.string();

    pnanovdb_compiler_t compiler = {};
    pnanovdb_compiler_load(&compiler);

    if (compiler.module == nullptr)
    {
        FAIL() << "Compiler module not available";
    }

    pnanovdb_compiler_instance_t* compiler_inst = compiler.create_instance();
    ASSERT_NE(compiler_inst, nullptr);
    compiler.set_diagnostic_callback(compiler_inst, capture_compiler_diagnostics);

    pnanovdb_compiler_settings_t compile_settings = {};
    pnanovdb_compiler_settings_init(&compile_settings);
    compile_settings.compile_target = PNANOVDB_COMPILE_TARGET_CPU;
    std::strcpy(compile_settings.entry_point_name, "computeMain");

    g_cpu_interface_compile_diagnostics.clear();
    const pnanovdb_bool_t compile_result =
        compiler.compile_shader_from_file(compiler_inst, shader_path.c_str(), &compile_settings, nullptr);

    ASSERT_NE(compile_result, PNANOVDB_FALSE)
        << "Compilation of shader failed: " << shader_path
        << (g_cpu_interface_compile_diagnostics.empty() ? "" : "\n" + g_cpu_interface_compile_diagnostics);

    struct print_iface_t : public ISlangUnknown
    {
        std::atomic<int> call_count = 0;

        virtual SLANG_NO_THROW SlangResult SLANG_MCALL
        queryInterface(SlangUUID const&, void**) SLANG_OVERRIDE
        {
            return SLANG_E_NOT_IMPLEMENTED;
        }

        virtual SLANG_NO_THROW uint32_t SLANG_MCALL addRef() SLANG_OVERRIDE { return 1; }
        virtual SLANG_NO_THROW uint32_t SLANG_MCALL release() SLANG_OVERRIDE { return 1; }

        virtual SLANG_NO_THROW void SLANG_MCALL print()
        {
            call_count.fetch_add(1, std::memory_order_relaxed);
        }
    };

    struct constants_t
    {
        int magic_number;
        int pad1;
        print_iface_t* print;
    };

    struct uniform_state_t
    {
        int* data_in;
        size_t data_in_count;
        constants_t* constants_in;
        int* data_out;
        size_t data_out_count;
        int* scratch_out;
        size_t scratch_out_count;
    };

    std::array<int, 8> data_in = {1, 2, 3, 4, 5, 6, 7, 8};
    std::array<int, 8> data_out = {17, 27, 37, 47, 57, 67, 77, 87};
    print_iface_t print_iface;
    constants_t constants = {13, 0, &print_iface};
    uniform_state_t uniform_state = {};
    uniform_state.data_in = data_in.data();
    uniform_state.data_in_count = data_in.size();
    uniform_state.constants_in = &constants;
    uniform_state.data_out = data_out.data();
    uniform_state.data_out_count = data_out.size();

    const pnanovdb_bool_t execute_result =
        compiler.execute_cpu(compiler_inst, shader_path.c_str(), 1u, 1u, 1u, nullptr, &uniform_state);

    EXPECT_NE(execute_result, PNANOVDB_FALSE);

    for (size_t i = 0; i < data_in.size(); ++i)
    {
        EXPECT_EQ(data_out[i], data_in[i] + constants.magic_number);
    }

    EXPECT_EQ(print_iface.call_count.load(std::memory_order_relaxed), static_cast<int>(data_in.size()));

    compiler.destroy_instance(compiler_inst);
    pnanovdb_compiler_free(&compiler);
}
