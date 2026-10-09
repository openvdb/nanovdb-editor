// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include "ConsoleTestSupport.h"
#include "editor/ShaderParams.h"
#include "ImGuiTestSupport.h"
#include "nanovdb_editor/putil/Shader.hpp"

#include <imgui_internal.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
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
        pnanovdb_editor_test::create_imgui_context();
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
        const auto log_path = directory / "ui.log";
        std::filesystem::remove(log_path);
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
        ImGui::LogToFile(0, log_path.string().c_str());
        if (group)
        {
            params.renderGroup(group_path);
        }
        else
        {
            params.render(shader);
        }
        ImGui::LogFinish();
        ImGui::End();
        ImGui::Render();
        std::ifstream log(log_path);
        return { std::istreambuf_iterator<char>(log), std::istreambuf_iterator<char>() };
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

TEST_F(ShaderColorRampTest, ValidRampKeepsCountFieldOrderWithoutChangingHdrValues)
{
    const auto before = bytes();
    for (int i = 0; i < 3; ++i)
    {
        const auto text = frame();
        EXPECT_LT(text.find("gain"), text.find("Fixture ramp"));
        EXPECT_EQ(text.find("stop_count"), std::string::npos);
        EXPECT_EQ(text.find("stop_positions"), std::string::npos);
        EXPECT_EQ(text.find("stop_red"), std::string::npos);
        EXPECT_EQ(before, bytes());
    }
}

TEST_F(ShaderColorRampTest, GroupAllocatesOmittedInactiveColorBeforeShaderRuns)
{
    hints["ShaderParams"].erase("stop_unused");
    reload();
    group_path = (directory / "group").string();
    std::ofstream(group_path + ".json") << nlohmann::json{ { "ShaderParams", { shader } } }.dump();
    ASSERT_TRUE(params.loadGroup(group_path, true));
    const auto text = frame(true);
    EXPECT_NE(text.find("Fixture ramp"), std::string::npos);
    EXPECT_EQ(text.find("stop_count"), std::string::npos);
    const auto fields = params.snapshot(shader);
    const auto unused =
        std::find_if(fields.begin(), fields.end(), [](const auto& field) { return field.name == "stop_unused"; });
    ASSERT_NE(unused, fields.end());
    ASSERT_NE(params.getValue(*unused), nullptr);
    EXPECT_EQ(values<float>("stop_unused"), (std::vector<float>{ 0.f, 0.f, 0.f, 0.f }));
}

TEST_F(ShaderColorRampTest, BoundFieldsBeforeCountDoNotRenderTwice)
{
    const auto fields = compiled["ShaderParams"];
    compiled["ShaderParams"] = nlohmann::ordered_json::object();
    for (const char* name : { "stop_red", "gain", "stop_blue", "stop_unused", "stop_positions", "stop_count" })
    {
        compiled["ShaderParams"][name] = fields[name];
    }
    reload();
    const auto before = bytes();
    const auto text = frame();
    EXPECT_LT(text.find("gain"), text.find("Fixture ramp"));
    EXPECT_EQ(text.find("stop_red"), std::string::npos);
    EXPECT_EQ(text.find("stop_positions"), std::string::npos);
    EXPECT_EQ(text.find("stop_count"), std::string::npos);
    EXPECT_EQ(before, bytes());
}

TEST_F(ShaderColorRampTest, GroupUsesLaterAuthoredDefaultsForOmittedBoundFields)
{
    hints["ShaderParams"].erase("stop_unused");
    reload();
    const auto later_shader = (directory / "later.slang").string();
    const auto later_compiled = pnanovdb_shader::getCompiledShaderParamsFilePath(later_shader.c_str());
    std::ofstream(later_shader) << "// Test reflection fixture\n";
    std::ofstream(later_compiled) << nlohmann::json{
        { "ShaderParams", { { "stop_unused", { { "type", "float" }, { "elementCount", 4 } } } } }
    }.dump();
    std::ofstream(later_shader + ".json") << nlohmann::json{
        { "ShaderParams", { { "stop_unused", { { "value", { 3.f, 4.f, 5.f, 6.f } }, { "step", 0.f } } } } }
    }.dump();
    group_path = (directory / "group").string();
    std::ofstream(group_path + ".json") << nlohmann::json{ { "ShaderParams", { shader, later_shader } } }.dump();
    const bool loaded = params.loadGroup(group_path, true);
    std::filesystem::remove(later_compiled);
    ASSERT_TRUE(loaded);
    EXPECT_NE(frame(true).find("Fixture ramp"), std::string::npos);
    const auto later = params.snapshot(later_shader);
    ASSERT_EQ(later.size(), 1u);
    const auto current = params.snapshot(shader);
    const auto unused =
        std::find_if(current.begin(), current.end(), [](const auto& field) { return field.name == "stop_unused"; });
    ASSERT_NE(unused, current.end());
    ASSERT_NE(params.getValue(*unused), nullptr);
    EXPECT_EQ(values<float>("stop_unused"), (std::vector<float>{ 3.f, 4.f, 5.f, 6.f }));
    EXPECT_EQ(unused->pool_index, later.front().pool_index);
}

TEST_F(ShaderColorRampTest, ConsoleWarningsDoNotRepeatAcrossGroupAndShaderViews)
{
    for (auto& hint : hints["ShaderParams"])
    {
        hint["step"] = 0.f;
    }
    reload();
    bytes();
    const auto first_shader = shader;
    const auto second_shader = (directory / "second.slang").string();
    const auto second_compiled = pnanovdb_shader::getCompiledShaderParamsFilePath(second_shader.c_str());
    auto second_layout = compiled;
    auto second_hints = hints;
    second_layout["ShaderParams"].erase("stop_count");
    second_layout["ShaderParams"]["other_count"] = { { "type", "uint" }, { "elementCount", 1 } };
    second_hints["ShaderParams"].erase("stop_count");
    second_hints["ShaderParams"]["other_count"] = hints["ShaderParams"]["stop_count"];
    second_hints["ShaderParams"]["other_count"]["label"] = "Second ramp";
    std::ofstream(second_shader) << "// Test reflection fixture\n";
    std::ofstream(second_compiled) << second_layout.dump();
    std::ofstream(second_shader + ".json") << second_hints.dump();
    const bool loaded = params.load(second_shader, true);
    std::filesystem::remove(second_compiled);
    ASSERT_TRUE(loaded);
    group_path = (directory / "two_shader_group").string();
    std::ofstream(group_path + ".json") << nlohmann::json{ { "ShaderParams", { first_shader, second_shader } } }.dump();
    ASSERT_TRUE(params.loadGroup(group_path, true));
    const auto pool_index = [&](const std::string& shader_name, const char* name)
    {
        for (const auto& field : params.snapshot(shader_name))
        {
            if (field.name == name)
            {
                return field.pool_index;
            }
        }
        return SIZE_MAX;
    };
    ASSERT_NE(pool_index(first_shader, "stop_positions"), SIZE_MAX);
    ASSERT_NE(pool_index(first_shader, "stop_red"), SIZE_MAX);
    ASSERT_EQ(pool_index(first_shader, "stop_positions"), pool_index(second_shader, "stop_positions"));
    ASSERT_EQ(pool_index(first_shader, "stop_red"), pool_index(second_shader, "stop_red"));
    pnanovdb_editor::test::clearConsoleWarnings();
    shader = second_shader;
    for (int repeat = 0; repeat < 3; ++repeat)
    {
        const auto grouped = frame(true);
        EXPECT_NE(grouped.find("Color ramp 'other_count' unavailable:"), std::string::npos);
        const auto single = frame();
        EXPECT_NE(single.find("Second ramp"), std::string::npos);
        EXPECT_EQ(single.find("unavailable"), std::string::npos);
    }
    const auto warnings = pnanovdb_editor::test::consoleWarnings();
    ASSERT_EQ(warnings.size(), 1u);
    EXPECT_NE(warnings.front().find("Color ramp 'other_count'"), std::string::npos);
}

TEST_F(ShaderColorRampTest, ConsoleWarningsReportEachReasonOnceUntilJsonReload)
{
    const auto valid = bytes();
    auto invalid_count = valid;
    const uint32_t zero = 0;
    std::memcpy(invalid_count.data() + sizeof(float), &zero, sizeof(zero));
    auto nonfinite = valid;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    std::memcpy(nonfinite.data() + nonfinite.size() - sizeof(float), &nan, sizeof(nan));
    const auto show = [&](std::vector<char> data)
    {
        pnanovdb_compute_array_t array{ data.data(), 1, data.size(), nullptr };
        params.set_compute_array_for_shader(shader, &array);
        return frame();
    };
    pnanovdb_editor::test::clearConsoleWarnings();
    for (int repeat = 0; repeat < 3; ++repeat)
    {
        EXPECT_NE(show(invalid_count).find("The count must be between"), std::string::npos);
        EXPECT_NE(show(valid).find("Fixture ramp"), std::string::npos);
    }
    ASSERT_EQ(pnanovdb_editor::test::consoleWarnings().size(), 1u);
    EXPECT_NE(show(nonfinite).find("including inactive slots"), std::string::npos);
    EXPECT_NE(show(invalid_count).find("The count must be between"), std::string::npos);
    const auto warnings = pnanovdb_editor::test::consoleWarnings();
    ASSERT_EQ(warnings.size(), 2u);
    EXPECT_NE(warnings.front().find("The count must be between"), std::string::npos);
    EXPECT_NE(warnings.back().find("including inactive slots"), std::string::npos);
    reload();
    EXPECT_NE(show(invalid_count).find("The count must be between"), std::string::npos);
    EXPECT_EQ(pnanovdb_editor::test::consoleWarnings().size(), 3u);
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
        { { "widget", "colourRamp" } },
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
        EXPECT_NE(text.find("Color ramp 'stop_count' unavailable:"), std::string::npos) << patch.dump();
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
    const auto hidden_field = text.find("stop_red");
    ASSERT_NE(text.find("Color field 'stop_red'"), std::string::npos);
    EXPECT_EQ(text.find("stop_red", hidden_field + std::strlen("stop_red")), std::string::npos);
    EXPECT_NE(text.find("stop_count"), std::string::npos);
}

TEST_F(ShaderColorRampTest, HiddenCountReportsOnceAndRecoversAfterReload)
{
    hints["ShaderParams"]["stop_count"]["hidden"] = true;
    pnanovdb_editor::test::clearConsoleWarnings();
    reload();
    group_path = (directory / "hidden_count_group").string();
    std::ofstream(group_path + ".json") << nlohmann::json{ { "ShaderParams", { shader } } }.dump();
    ASSERT_TRUE(params.loadGroup(group_path, true));
    const auto before = bytes();
    for (int repeat = 0; repeat < 3; ++repeat)
    {
        for (bool group : { false, true })
        {
            const auto text = frame(group);
            EXPECT_EQ(text.find("Fixture ramp"), std::string::npos);
            EXPECT_EQ(text.find("stop_count"), std::string::npos);
            EXPECT_NE(text.find("stop_red"), std::string::npos);
            EXPECT_EQ(before, bytes());
        }
    }
    const auto warnings = pnanovdb_editor::test::consoleWarnings();
    ASSERT_EQ(warnings.size(), 1u);
    EXPECT_NE(warnings.front().find("The count field must be visible."), std::string::npos);
    hints["ShaderParams"]["stop_count"]["hidden"] = false;
    reload();
    ASSERT_TRUE(params.loadGroup(group_path, true));
    EXPECT_NE(frame().find("Fixture ramp"), std::string::npos);
    EXPECT_NE(frame(true).find("Fixture ramp"), std::string::npos);
    EXPECT_EQ(pnanovdb_editor::test::consoleWarnings().size(), 1u);
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

TEST_F(ShaderColorRampTest, NonfiniteInactiveColorReportsTheReasonAndRecovers)
{
    auto data = bytes();
    const auto valid = data;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    std::memcpy(data.data() + data.size() - sizeof(float), &nan, sizeof(nan));
    pnanovdb_compute_array_t array{ data.data(), 1, data.size(), nullptr };
    params.set_compute_array_for_shader(shader, &array);
    for (int i = 0; i < 3; ++i)
    {
        const auto text = frame();
        EXPECT_EQ(text.find("Fixture ramp"), std::string::npos);
        EXPECT_NE(text.find("including inactive slots"), std::string::npos);
        EXPECT_EQ(data, bytes());
    }
    data = valid;
    array.data = data.data();
    params.set_compute_array_for_shader(shader, &array);
    EXPECT_NE(frame().find("Fixture ramp"), std::string::npos);
    EXPECT_EQ(frame().find("unavailable"), std::string::npos);
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
