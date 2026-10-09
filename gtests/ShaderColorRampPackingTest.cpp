// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include "GpuTestSupport.h"
#include "editor/ShaderParams.h"
#include "nanovdb_editor/putil/Compiler.h"
#include "nanovdb_editor/putil/Shader.hpp"

#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace
{

using FieldOffsets = std::map<std::string, uint32_t>;

std::filesystem::path compiledFile(const std::string& shader, const char* extension)
{
    return std::filesystem::path(PNANOVDB_TEST_SHADER_CACHE) /
           (std::filesystem::path(shader).filename().string() + extension);
}

FieldOffsets uniformFieldOffsets(const std::string& shader)
{
    std::ifstream file(compiledFile(shader, pnanovdb_shader::SHADER_EXT), std::ios::binary | std::ios::ate);
    if (!file)
    {
        return {};
    }
    const auto size = file.tellg();
    if (size < 20 || size % sizeof(uint32_t) != 0)
    {
        return {};
    }
    std::vector<uint32_t> words(static_cast<size_t>(size) / sizeof(uint32_t));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(words.data()), size);
    if (!file || words[0] != 0x07230203u)
    {
        return {};
    }
    std::map<std::pair<uint32_t, uint32_t>, std::string> names;
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> offsets;
    for (size_t i = 5; i < words.size();)
    {
        const uint32_t count = words[i] >> 16;
        const uint32_t opcode = words[i] & 0xffffu;
        if (count == 0 || count > words.size() - i)
        {
            return {};
        }
        if (opcode == 6 && count >= 4) // OpMemberName
        {
            const auto* text = reinterpret_cast<const char*>(&words[i + 3]);
            const size_t capacity = (count - 3) * sizeof(uint32_t);
            const auto* end = static_cast<const char*>(std::memchr(text, '\0', capacity));
            if (!end)
            {
                return {};
            }
            names[{ words[i + 1], words[i + 2] }] = std::string(text, end);
        }
        if (opcode == 72 && count == 5 && words[i + 3] == 35) // OpMemberDecorate, Offset
        {
            offsets[{ words[i + 1], words[i + 2] }] = words[i + 4];
        }
        i += count;
    }
    FieldOffsets result;
    for (const auto& [member, name] : names)
    {
        const auto offset = offsets.find(member);
        if (offset != offsets.end() && (name == "cold" || name == "hot" || name == "positions" || name == "count"))
        {
            result[name] = offset->second;
        }
    }
    return result;
}

FieldOffsets packedFieldOffsets(const std::string& shader)
{
    pnanovdb_editor::ShaderParams params;
    std::array<char, 64> buffer{};
    EXPECT_EQ(params.copy_params_to_buffer(shader, buffer.data(), buffer.size()), 44u);
    size_t offset = 0;
    FieldOffsets packed;
    for (const auto& field : params.snapshot(shader))
    {
        packed[field.name] = static_cast<uint32_t>(offset);
        offset += field.size * field.num_elements;
    }
    return packed;
}

class ShaderColorRampPackingTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        shader = (std::filesystem::path(__FILE__).parent_path() / "shaders/color_ramp_packing.slang").string();
        const auto id = std::chrono::steady_clock::now().time_since_epoch().count();
        directory = std::filesystem::temp_directory_path() / ("ramp_packing_" + std::to_string(id));
        std::filesystem::create_directories(directory);
        count_first = (directory / ("count_first_" + std::to_string(id) + ".slang")).string();
        std::ifstream file(shader);
        std::string source((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        const std::string fields = "    float4 cold;\n    float4 hot;\n    float2 positions;\n    uint count;";
        const auto start = source.find(fields);
        ASSERT_NE(start, std::string::npos);
        source.replace(start, fields.size(), "    uint count;\n    float2 positions;\n    float4 cold;\n    float4 hot;");
        std::ofstream(count_first) << source;
        std::filesystem::copy_file(shader + ".json", count_first + ".json");

        pnanovdb_compiler_load(&compiler);
        ASSERT_NE(compiler.module, nullptr);
        compiler_instance = compiler.create_instance();
        ASSERT_NE(compiler_instance, nullptr);
        pnanovdb_compiler_settings_t settings{};
        pnanovdb_compiler_settings_init(&settings);
        ASSERT_NE(
            compiler.compile_shader_from_file(compiler_instance, shader.c_str(), &settings, nullptr), PNANOVDB_FALSE);
        ASSERT_NE(compiler.compile_shader_from_file(compiler_instance, count_first.c_str(), &settings, nullptr),
                  PNANOVDB_FALSE);
    }

    void TearDown() override
    {
        if (compute.module)
        {
            for (auto* array : arrays)
            {
                compute.destroy_array(array);
            }
            if (device)
            {
                compute.device_interface.destroy_device(device_manager, device);
            }
            if (device_manager)
            {
                compute.device_interface.destroy_device_manager(device_manager);
            }
            pnanovdb_compute_free(&compute);
        }
        if (compiler_instance)
        {
            compiler.destroy_instance(compiler_instance);
        }
        if (compiler.module)
        {
            pnanovdb_compiler_free(&compiler);
        }
        if (!count_first.empty())
        {
            std::filesystem::remove(compiledFile(count_first, pnanovdb_shader::SHADER_EXT));
            std::filesystem::remove(compiledFile(count_first, pnanovdb_shader::JSON_EXT));
        }
        if (!directory.empty())
        {
            std::filesystem::remove_all(directory);
        }
    }

    pnanovdb_compute_array_t* array(size_t size, size_t count, const void* data)
    {
        auto* result = compute.create_array(size, count, data);
        if (result)
        {
            arrays.push_back(result);
        }
        return result;
    }

    std::string shader;
    std::string count_first;
    std::filesystem::path directory;
    pnanovdb_compiler_t compiler{};
    pnanovdb_compiler_instance_t* compiler_instance = nullptr;
    pnanovdb_compute_t compute{};
    pnanovdb_compute_device_manager_t* device_manager = nullptr;
    pnanovdb_compute_device_t* device = nullptr;
    std::vector<pnanovdb_compute_array_t*> arrays;
};

TEST_F(ShaderColorRampPackingTest, DocumentedDeclarationMatchesCompiledVulkanOffsets)
{
    const FieldOffsets expected{ { "cold", 0 }, { "hot", 16 }, { "positions", 32 }, { "count", 40 } };
    EXPECT_EQ(uniformFieldOffsets(shader), expected);
    EXPECT_EQ(packedFieldOffsets(shader), expected);
}

TEST_F(ShaderColorRampPackingTest, CountFirstDeclarationIntroducesGpuAlignmentGaps)
{
    const FieldOffsets expected{ { "count", 0 }, { "positions", 8 }, { "cold", 16 }, { "hot", 32 } };
    EXPECT_EQ(uniformFieldOffsets(count_first), expected);
    const auto packed = packedFieldOffsets(count_first);
    EXPECT_EQ(packed, (FieldOffsets{ { "count", 0 }, { "positions", 4 }, { "cold", 12 }, { "hot", 28 } }));
    EXPECT_NE(packed, expected);
}

TEST_F(ShaderColorRampPackingTest, DocumentedRampValuesReachGpuWithoutRepacking)
{
    pnanovdb_compute_load(&compute, &compiler);
    ASSERT_NE(compute.module, nullptr);
    device_manager = compute.device_interface.create_device_manager(PNANOVDB_FALSE);
    ASSERT_NE(device_manager, nullptr);
    pnanovdb_compute_physical_device_desc_t physical{};
    if (!compute.device_interface.enumerate_devices(device_manager, 0u, &physical))
    {
        GTEST_SKIP() << "No Vulkan-compatible device available";
    }
    RecordProperty("vulkan_device", physical.device_name);
    pnanovdb_compute_device_desc_t descriptor{};
    descriptor.log_print = pnanovdb_editor_test::stderr_log_print;
    device = compute.device_interface.create_device(device_manager, &descriptor);
    ASSERT_NE(device, nullptr);

    pnanovdb_editor::ShaderParams params;
    std::array<char, 64> packed{};
    ASSERT_EQ(params.copy_params_to_buffer(shader, packed.data(), packed.size()), 44u);
    const uint32_t zero = 0;
    auto* input = array(sizeof(zero), 1, &zero);
    auto* constants = array(packed.size(), 1, packed.data());
    auto* output = array(sizeof(uint32_t), 11, nullptr);
    ASSERT_NE(input, nullptr);
    ASSERT_NE(constants, nullptr);
    ASSERT_NE(output, nullptr);
    ASSERT_NE(
        compute.dispatch_shader_on_array(&compute, device, shader.c_str(), 1, 1, 1, input, constants, output, 1, 0, 0),
        PNANOVDB_FALSE);
    const auto* actual = static_cast<const uint32_t*>(compute.map_array(output));
    ASSERT_NE(actual, nullptr);
    EXPECT_EQ(actual[0], 2u);
    const std::array<float, 10> expected{ 0.f, 1.f, 0.1f, 0.2f, 1.f, 0.8f, 1.f, 0.2f, 0.f, 0.8f };
    for (size_t i = 0; i < expected.size(); ++i)
    {
        uint32_t bits;
        std::memcpy(&bits, &expected[i], sizeof(bits));
        EXPECT_EQ(actual[i + 1], bits) << "component " << i;
    }
    compute.unmap_array(output);
}

} // namespace
