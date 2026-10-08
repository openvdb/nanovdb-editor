// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include "editor/ShaderParams.h"
#include "nanovdb_editor/putil/Shader.hpp"

#include <imgui_internal.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>

namespace
{

class ShaderColorRampTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        const auto id = std::chrono::steady_clock::now().time_since_epoch().count();
        directory = std::filesystem::temp_directory_path() / ("shader_ramp_" + std::to_string(id));
        std::filesystem::create_directories(directory);
        shader = (directory / ("ramp_" + std::to_string(id) + ".slang")).string();
        std::ofstream(shader) << "// Test reflection fixture\n";
        compiled_path = pnanovdb_shader::getCompiledShaderParamsFilePath(shader.c_str());
        compiled = { { "ShaderParams",
                       { { "gain", { { "type", "float" }, { "elementCount", 1 } } },
                         { "stop_count", { { "type", "uint" }, { "elementCount", 1 } } },
                         { "stop_positions", { { "type", "float" }, { "elementCount", 3 } } },
                         { "stop_red", { { "type", "float" }, { "elementCount", 4 } } },
                         { "stop_blue", { { "type", "float" }, { "elementCount", 4 } } },
                         { "stop_unused", { { "type", "float" }, { "elementCount", 4 } } } } } };
        hints = { { "ShaderParams",
                    { { "gain", { { "value", 0.5 } } },
                      { "stop_count",
                        { { "value", 2 },
                          { "widget", "colorRamp" },
                          { "label", "Fixture ramp" },
                          { "positions", { "stop_positions" } },
                          { "colors", { "stop_red", "stop_blue", "stop_unused" } } } },
                      { "stop_positions", { { "value", { 0.0, 1.0, 0.5 } } } },
                      { "stop_red", { { "value", { 2.0, 0.0, 0.0, 2.0 } } } },
                      { "stop_blue", { { "value", { 0.0, 0.0, 4.0, 6.0 } } } },
                      { "stop_unused", { { "value", { 0.0, 0.0, 0.0, 0.0 } } } } } } };
        reload();
        ImGui::CreateContext();
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(640, 480);
        io.DeltaTime = 1.f / 60.f;
        unsigned char* pixels;
        int width, height;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    }

    void TearDown() override
    {
        ImGui::DestroyContext();
        std::filesystem::remove(compiled_path);
        std::filesystem::remove_all(directory);
    }

    void reload()
    {
        std::ofstream(compiled_path) << compiled.dump();
        std::ofstream(shader + ".json") << hints.dump();
        ASSERT_TRUE(params.load(shader, true));
    }

    std::vector<char> bytes()
    {
        std::vector<char> result(256);
        result.resize(params.copy_params_to_buffer(shader, result.data(), result.size()));
        return result;
    }

    template <typename T>
    std::vector<T> values(const std::string& name)
    {
        for (const auto& field : params.snapshot(shader))
        {
            if (field.name == name)
            {
                std::vector<T> result(field.size * field.num_elements / sizeof(T));
                std::memcpy(result.data(), params.getValue(field), result.size() * sizeof(T));
                return result;
            }
        }
        return {};
    }

    std::string frame(bool group = false)
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(500, 460));
        ImGui::Begin("Controls", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        const std::string label = "Fixture ramp###" + shader + "/stop_count";
        ImGui::PushID(label.c_str());
        add_id = ImGui::GetID("Add stop");
        remove_id = ImGui::GetID("Remove stop");
        ImGui::PushID("Color");
        color_input_id = ImGui::GetID("##X");
        ImGui::PopID();
        ImGui::PopID();
        ImGui::LogToBuffer();
        if (group)
        {
            params.renderGroup(group_path);
        }
        else
        {
            params.render(shader);
        }
        const std::string text = ImGui::GetCurrentContext()->LogBuffer.c_str();
        ImGui::LogFinish();
        ImGui::End();
        ImGui::Render();
        return text;
    }

    void editColor(const char* value)
    {
        frame();
        ImGui::ActivateItemByID(color_input_id);
        ImGui::GetCurrentContext()->NavNextActivateFlags = ImGuiActivateFlags_PreferInput;
        frame();
        ImGui::GetIO().AddInputCharactersUTF8(value);
        frame();
        ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, true);
        frame();
        ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, false);
        frame();
    }

    pnanovdb_editor::ShaderParams params;
    nlohmann::ordered_json compiled;
    nlohmann::ordered_json hints;
    std::filesystem::path directory;
    std::string shader;
    std::string compiled_path;
    std::string group_path;
    ImGuiID add_id = 0;
    ImGuiID remove_id = 0;
    ImGuiID color_input_id = 0;
};

TEST_F(ShaderColorRampTest, ValidRampRendersFirstWithoutChangingHdrValues)
{
    const auto before = bytes();
    for (int i = 0; i < 3; ++i)
    {
        const auto text = frame();
        EXPECT_LT(text.find("Fixture ramp"), text.find("gain"));
        EXPECT_EQ(text.find("stop_count"), std::string::npos);
        EXPECT_EQ(text.find("stop_positions"), std::string::npos);
        EXPECT_EQ(text.find("stop_red"), std::string::npos);
        EXPECT_EQ(before, bytes());
    }
}

TEST_F(ShaderColorRampTest, AddAndRemoveWriteCountPositionsAndColorsTogether)
{
    frame();
    ImGui::ActivateItemByID(add_id);
    frame();
    EXPECT_EQ(values<uint32_t>("stop_count"), (std::vector<uint32_t>{ 3 }));
    EXPECT_EQ(values<float>("stop_positions"), (std::vector<float>{ 0.f, 1.f, 0.5f }));
    EXPECT_EQ(values<float>("stop_unused"), (std::vector<float>{ 1.f, 0.f, 2.f, 4.f }));
    EXPECT_EQ(values<float>("stop_red"), (std::vector<float>{ 2.f, 0.f, 0.f, 2.f }));
    EXPECT_EQ(values<float>("gain"), (std::vector<float>{ 0.5f }));
    ImGui::ActivateItemByID(remove_id);
    frame();
    EXPECT_EQ(values<uint32_t>("stop_count"), (std::vector<uint32_t>{ 2 }));
}

TEST_F(ShaderColorRampTest, ColorInputUpdatesShaderPoolAndRejectsNonfiniteEdits)
{
    editColor("3.5");
    EXPECT_EQ(values<float>("stop_red"), (std::vector<float>{ 3.5f, 0.f, 0.f, 2.f }));
    const auto before = bytes();
    editColor("1e39");
    EXPECT_EQ(before, bytes());
}

TEST_F(ShaderColorRampTest, InvalidMetadataAndRuntimeCountsFallBackToRawControls)
{
    const auto original = hints;
    const std::vector<nlohmann::ordered_json> invalid{
        { { "positions", { "missing" } } },
        { { "colors", { "stop_red", "stop_red", "stop_unused" } } },
        { { "colors", { "stop_red", "stop_blue" } } },
        { { "colors", "stop_red" } },
        { { "label", 2 } },
        { { "tooltip", false } },
        { { "value", 0 } },
        { { "value", 4 } },
    };
    for (const auto& patch : invalid)
    {
        hints = original;
        hints["ShaderParams"]["stop_count"].update(patch);
        reload();
        const auto before = bytes();
        const auto text = frame();
        EXPECT_EQ(text.find("Fixture ramp"), std::string::npos) << patch.dump();
        EXPECT_NE(text.find("stop_count"), std::string::npos) << patch.dump();
        EXPECT_NE(text.find("stop_red"), std::string::npos) << patch.dump();
        EXPECT_EQ(before, bytes());
    }
}

TEST_F(ShaderColorRampTest, WrongReflectedTypesAndHiddenBindingsFallBack)
{
    compiled["ShaderParams"]["stop_count"]["type"] = "int";
    reload();
    EXPECT_EQ(frame().find("Fixture ramp"), std::string::npos);
    compiled["ShaderParams"]["stop_count"]["type"] = "uint";
    compiled["ShaderParams"]["stop_positions"]["type"] = "float16";
    reload();
    EXPECT_EQ(frame().find("Fixture ramp"), std::string::npos);
    compiled["ShaderParams"]["stop_positions"]["type"] = "float";
    hints["ShaderParams"]["stop_red"]["hidden"] = true;
    reload();
    const auto text = frame();
    EXPECT_EQ(text.find("Fixture ramp"), std::string::npos);
    EXPECT_EQ(text.find("stop_red"), std::string::npos);
    EXPECT_NE(text.find("stop_count"), std::string::npos);
}

TEST_F(ShaderColorRampTest, NonfiniteDataFallsBackWithoutChangingPoolBytes)
{
    auto data = bytes();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    std::memcpy(data.data() + sizeof(float) + sizeof(uint32_t), &nan, sizeof(nan));
    pnanovdb_compute_array_t array{ data.data(), 1, data.size(), nullptr };
    params.set_compute_array_for_shader(shader, &array);
    EXPECT_EQ(frame().find("Fixture ramp"), std::string::npos);
    EXPECT_EQ(data, bytes());
}

TEST_F(ShaderColorRampTest, GroupReloadUsesCurrentMetadata)
{
    group_path = (directory / "group").string();
    std::ofstream(group_path + ".json") << nlohmann::json{ { "ShaderParams", { shader } } }.dump();
    ASSERT_TRUE(params.loadGroup(group_path, true));
    EXPECT_NE(frame(true).find("Fixture ramp"), std::string::npos);
    hints["ShaderParams"]["stop_count"].erase("widget");
    reload();
    const auto text = frame(true);
    EXPECT_EQ(text.find("Fixture ramp"), std::string::npos);
    EXPECT_NE(text.find("stop_count"), std::string::npos);
}

} // namespace
