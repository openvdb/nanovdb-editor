// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <nanovdb_editor/putil/Compiler.h>
#include <filesystem>
#include <chrono>
#include <cstring>
#include <iostream>

class EditorSlangCompileSpeedTest : public ::testing::Test
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

    pnanovdb_compiler_t compiler = {};
    pnanovdb_compiler_instance_t* compiler_inst = nullptr;
};

TEST_F(EditorSlangCompileSpeedTest, EditorSlangCompilesToVulkan)
{
    const std::filesystem::path shader =
        std::filesystem::path(__FILE__).parent_path().parent_path() / "editor" / "shaders" / "editor.slang";
    const std::string shader_path = shader.string();

    ASSERT_TRUE(std::filesystem::exists(shader)) << "Shader file not found: " << shader_path;

    pnanovdb_compiler_settings_t compile_settings = {};
    pnanovdb_compiler_settings_init(&compile_settings);
    compile_settings.compile_target = PNANOVDB_COMPILE_TARGET_VULKAN;
    std::strcpy(compile_settings.entry_point_name, "main");

    auto start = std::chrono::high_resolution_clock::now();

    pnanovdb_bool_t result =
        compiler.compile_shader_from_file(compiler_inst, shader_path.c_str(), &compile_settings, nullptr);

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    std::cout << "[COMPILE TIME] editor.slang (Vulkan): " << duration.count() << " ms" << std::endl;

    ASSERT_NE(result, PNANOVDB_FALSE) << "Compilation of editor.slang failed: " << shader_path;
}

TEST_F(EditorSlangCompileSpeedTest, EditorSlangCompileSpeedBenchmark)
{
    const std::filesystem::path shader =
        std::filesystem::path(__FILE__).parent_path().parent_path() / "editor" / "shaders" / "editor.slang";
    const std::string shader_path = shader.string();

    ASSERT_TRUE(std::filesystem::exists(shader)) << "Shader file not found: " << shader_path;

    pnanovdb_compiler_settings_t compile_settings = {};
    pnanovdb_compiler_settings_init(&compile_settings);
    compile_settings.compile_target = PNANOVDB_COMPILE_TARGET_VULKAN;
    std::strcpy(compile_settings.entry_point_name, "main");

    constexpr int NUM_ITERATIONS = 3;
    std::vector<long long> durations;
    durations.reserve(NUM_ITERATIONS);

    for (int i = 0; i < NUM_ITERATIONS; ++i)
    {
        pnanovdb_compiler_instance_t* fresh_inst = compiler.create_instance();
        ASSERT_NE(fresh_inst, nullptr);

        auto start = std::chrono::high_resolution_clock::now();

        pnanovdb_bool_t result =
            compiler.compile_shader_from_file(fresh_inst, shader_path.c_str(), &compile_settings, nullptr);

        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        durations.push_back(duration.count());

        compiler.destroy_instance(fresh_inst);

        ASSERT_NE(result, PNANOVDB_FALSE) << "Compilation failed on iteration " << i;
    }

    long long total = 0;
    long long min_time = durations[0];
    long long max_time = durations[0];

    for (auto d : durations)
    {
        total += d;
        min_time = std::min(min_time, d);
        max_time = std::max(max_time, d);
    }

    double average = static_cast<double>(total) / NUM_ITERATIONS;

    std::cout << "[BENCHMARK] editor.slang compilation speed (Vulkan target):" << std::endl;
    std::cout << "  Iterations: " << NUM_ITERATIONS << std::endl;
    std::cout << "  Min:        " << min_time << " ms" << std::endl;
    std::cout << "  Max:        " << max_time << " ms" << std::endl;
    std::cout << "  Average:    " << average << " ms" << std::endl;

    constexpr long long MAX_ACCEPTABLE_MS = 5000;
    EXPECT_LT(average, MAX_ACCEPTABLE_MS) << "Compilation time exceeds acceptable threshold";
}

TEST_F(EditorSlangCompileSpeedTest, EditorSlangFirstVsSubsequentCompilation)
{
    const std::filesystem::path shader =
        std::filesystem::path(__FILE__).parent_path().parent_path() / "editor" / "shaders" / "editor.slang";
    const std::string shader_path = shader.string();

    ASSERT_TRUE(std::filesystem::exists(shader)) << "Shader file not found: " << shader_path;

    pnanovdb_compiler_settings_t compile_settings = {};
    pnanovdb_compiler_settings_init(&compile_settings);
    compile_settings.compile_target = PNANOVDB_COMPILE_TARGET_VULKAN;
    std::strcpy(compile_settings.entry_point_name, "main");

    // First compilation (cold)
    auto start1 = std::chrono::high_resolution_clock::now();
    pnanovdb_bool_t result1 =
        compiler.compile_shader_from_file(compiler_inst, shader_path.c_str(), &compile_settings, nullptr);
    auto end1 = std::chrono::high_resolution_clock::now();
    auto duration1 = std::chrono::duration_cast<std::chrono::milliseconds>(end1 - start1);

    ASSERT_NE(result1, PNANOVDB_FALSE) << "First compilation failed";

    // Second compilation (warm/cached)
    auto start2 = std::chrono::high_resolution_clock::now();
    pnanovdb_bool_t result2 =
        compiler.compile_shader_from_file(compiler_inst, shader_path.c_str(), &compile_settings, nullptr);
    auto end2 = std::chrono::high_resolution_clock::now();
    auto duration2 = std::chrono::duration_cast<std::chrono::milliseconds>(end2 - start2);

    ASSERT_NE(result2, PNANOVDB_FALSE) << "Second compilation failed";

    std::cout << "[CACHING TEST] editor.slang compilation:" << std::endl;
    std::cout << "  First compilation:  " << duration1.count() << " ms" << std::endl;
    std::cout << "  Second compilation: " << duration2.count() << " ms" << std::endl;

    if (duration2.count() < duration1.count())
    {
        double speedup = static_cast<double>(duration1.count()) / static_cast<double>(duration2.count());
        std::cout << "  Speedup factor:     " << speedup << "x" << std::endl;
    }
}
