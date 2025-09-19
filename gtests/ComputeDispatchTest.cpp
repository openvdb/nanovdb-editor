// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <nanovdb_editor/putil/Compiler.h>
#include <nanovdb_editor/putil/Compute.h>

#include <vector>
#include <cstring>
#include <filesystem>
#include <cstdarg>
#include <cstdio>

struct constants_t
{
    int magic_number;
    int pad1;
    int pad2;
    int pad3;
};

static void test_log_print(pnanovdb_compute_log_level_t level, const char* fmt, ...)
{
    const char* level_str = level == PNANOVDB_COMPUTE_LOG_LEVEL_ERROR   ? "ERROR" :
                            level == PNANOVDB_COMPUTE_LOG_LEVEL_WARNING ? "WARN" :
                                                                          "INFO";
    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "[ComputeDispatchTest][%s] ", level_str);
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);
}

TEST(NanoVDBEditor, ComputeDispatchShaderAddsNumbers)
{
    const std::filesystem::path shader = std::filesystem::path(__FILE__).parent_path() / "shaders" / "test.slang";
    const std::string shader_path = shader.string();

    pnanovdb_compiler_t compiler = {};
    pnanovdb_compiler_load(&compiler);

    if (compiler.module == nullptr)
    {
        FAIL() << "Compiler module not available";
    }

    pnanovdb_compute_t compute = {};
    pnanovdb_compute_load(&compute, &compiler);

    if (compute.module == nullptr)
    {
        FAIL() << "Failed to load compute module";
    }

    pnanovdb_compute_device_desc_t device_desc = {};
    device_desc.log_print = test_log_print;

    pnanovdb_compute_device_manager_t* device_manager = compute.device_interface.create_device_manager(PNANOVDB_FALSE);
    if (!device_manager || !compute.device_interface.create_device)
    {
        FAIL() << "Failed to create compute device manager";
    }

    // Skip test gracefully if no device is available (e.g. headless CI without Vulkan ICD)
    pnanovdb_compute_physical_device_desc_t phys_desc = {};
    if (!compute.device_interface.enumerate_devices(device_manager, 0u, &phys_desc))
    {
        GTEST_SKIP() << "No Vulkan-compatible device available on this machine";
    }

    pnanovdb_compute_device_t* device = compute.device_interface.create_device(device_manager, &device_desc);
    if (!device)
    {
        FAIL() << "Failed to create compute device";
    }

    constants_t params = { 4 };
    std::vector<int> input = { 0, 1, 2, 3, 4, 5, 6, 7 };
    const pnanovdb_uint64_t count = static_cast<pnanovdb_uint64_t>(input.size());

    pnanovdb_compute_array_t* data_in = compute.create_array(sizeof(int), count, input.data());
    pnanovdb_compute_array_t* constants = compute.create_array(sizeof(constants_t), 1u, &params);
    pnanovdb_compute_array_t* data_out = compute.create_array(sizeof(int), count, nullptr);

    int dispatch_result = compute.dispatch_shader_on_array(
        &compute, device, shader_path.c_str(), 8u, 1u, 1u, data_in, constants, data_out, 1u, 0llu, 0llu);

    if (dispatch_result == PNANOVDB_TRUE)
    {
        int* mapped = static_cast<int*>(compute.map_array(data_out));
        ASSERT_NE(mapped, nullptr);
        for (size_t i = 0; i < input.size(); ++i)
        {
            EXPECT_EQ(mapped[i], input[i] + params.magic_number);
        }
        compute.unmap_array(data_out);
    }
    else
    {
        FAIL() << "Shader dispatch failed";
    }

    compute.destroy_array(data_in);
    compute.destroy_array(constants);
    compute.destroy_array(data_out);

    compute.device_interface.destroy_device(device_manager, device);
    compute.device_interface.destroy_device_manager(device_manager);

    pnanovdb_compiler_free(&compiler);
    pnanovdb_compute_free(&compute);
}
