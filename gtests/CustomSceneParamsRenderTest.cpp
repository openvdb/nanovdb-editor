// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include "editor/CustomSceneParams.h"

#include <imgui_internal.h>

#include <cstring>

namespace
{

class CustomSceneParamsRenderTest : public ::testing::Test
{
protected:
    struct Item
    {
        ImGuiID id;
        ImRect rect;
        ImVec2 start;
        int active_color_vertices;
    };

    void SetUp() override
    {
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
    }

    Item frame(const char* group = nullptr, bool exclude_group = false, bool prefix = false)
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(600, 400));
        ImGui::Begin("Controls", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        if (prefix)
            ImGui::Button("External control");
        const auto start = ImGui::GetCursorScreenPos();
        params.render(group, exclude_group);
        const auto& last = ImGui::GetCurrentContext()->LastItemData;
        Item item{ last.ID, last.Rect, start, 0 };
        const ImU32 active_color = ImGui::GetColorU32(ImGuiCol_ButtonActive);
        for (const auto& vertex : ImGui::GetWindowDrawList()->VtxBuffer)
        {
            if (vertex.col == active_color)
                ++item.active_color_vertices;
        }
        ImGui::End();
        ImGui::Render();
        return item;
    }

    Item click(Item item, const char* group = nullptr, bool exclude_group = false)
    {
        auto& io = ImGui::GetIO();
        const ImVec2 center = item.rect.GetCenter();
        io.AddMousePosEvent(center.x, center.y);
        frame(group, exclude_group);
        io.AddMouseButtonEvent(0, true);
        frame(group, exclude_group);
        io.AddMouseButtonEvent(0, false);
        frame(group, exclude_group);
        io.AddMousePosEvent(-100, -100);
        return frame(group, exclude_group);
    }

    pnanovdb_bool_t* value(const char* name)
    {
        const auto* type = params.dataType();
        for (pnanovdb_uint64_t i = 0; type && i < type->child_reflect_data_count; ++i)
        {
            const auto& field = type->child_reflect_datas[i];
            if (std::strcmp(field.name, name) == 0)
                return reinterpret_cast<pnanovdb_bool_t*>(static_cast<char*>(params.data()) + field.data_offset);
        }
        return nullptr;
    }

    pnanovdb_editor::CustomSceneParams params;
};

TEST_F(CustomSceneParamsRenderTest, ButtonLatchesUntilConsumedAndFiltersGroups)
{
    ASSERT_TRUE(params.loadFromJsonString(R"({"SceneParams": {
        "Other": {"type":"bool", "widget":"button", "group":"Other"},
        "Step": {"type":"bool", "widget":"button", "group":"Simulation", "tooltip":"Advance one frame"}
    }})"));
    auto* step = value("Step");
    auto* other = value("Other");
    ASSERT_NE(step, nullptr);
    ASSERT_NE(other, nullptr);
    click(frame("Simulation"), "Simulation");
    EXPECT_EQ(*step, PNANOVDB_TRUE);
    EXPECT_EQ(*other, PNANOVDB_FALSE);
    frame("Simulation");
    EXPECT_EQ(*step, PNANOVDB_TRUE);
    *step = PNANOVDB_FALSE;
    click(frame("Simulation", true), "Simulation", true);
    EXPECT_EQ(*other, PNANOVDB_TRUE);
    EXPECT_EQ(*step, PNANOVDB_FALSE);
}

TEST_F(CustomSceneParamsRenderTest, ToggleKeepsIdAcrossLabelsAndHighlightsActiveState)
{
    ASSERT_TRUE(params.loadFromJsonString(R"({"SceneParams": {
        "Play": {"type":"bool32", "value":false, "elementCount":1,
                 "widget":"toggleButton", "activeLabel":"Stop"}
    }})"));
    auto* play = value("Play");
    ASSERT_NE(play, nullptr);
    const Item inactive = frame();
    EXPECT_EQ(inactive.active_color_vertices, 0);
    const Item active = click(inactive);
    EXPECT_EQ(*play, PNANOVDB_TRUE);
    EXPECT_EQ(active.id, inactive.id);
    EXPECT_GT(active.active_color_vertices, 0);
    const Item stopped = click(active);
    EXPECT_EQ(*play, PNANOVDB_FALSE);
    EXPECT_EQ(stopped.id, inactive.id);
    EXPECT_EQ(stopped.active_color_vertices, 0);
}

TEST_F(CustomSceneParamsRenderTest, ReadOnlyButtonIgnoresClicks)
{
    ASSERT_TRUE(params.loadFromJsonString(R"({"SceneParams": {
        "Restart": {"type":"bool", "widget":"button", "readOnly":true}
    }})"));
    auto* restart = value("Restart");
    ASSERT_NE(restart, nullptr);
    click(frame());
    EXPECT_EQ(*restart, PNANOVDB_FALSE);
}

TEST_F(CustomSceneParamsRenderTest, SameLineStartsAfterFirstVisibleFieldInThisCall)
{
    ASSERT_TRUE(params.loadFromJsonString(R"({"SceneParams": {
        "Other": {"type":"bool", "widget":"button", "group":"Other"},
        "Hidden": {"type":"bool", "widget":"button", "hidden":true, "group":"Simulation"},
        "Step": {"type":"bool", "widget":"button", "sameLine":true, "group":"Simulation"},
        "Restart": {"type":"bool", "widget":"button", "sameLine":true, "group":"Simulation"}
    }})"));
    const Item last = frame("Simulation", false, true);
    EXPECT_FLOAT_EQ(last.rect.Min.y, last.start.y);
    EXPECT_GT(last.rect.Min.x, last.start.x);
}

} // namespace
