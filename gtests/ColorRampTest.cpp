// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include "editor/ColorRamp.h"

#include <imgui_internal.h>

#include <limits>

namespace
{

using pnanovdb_editor::ColorRampPoint;
using pnanovdb_editor::addColorRampPoint;
using pnanovdb_editor::removeColorRampPoint;
using pnanovdb_editor::renderColorRamp;
using pnanovdb_editor::sampleColorRamp;

TEST(ColorRamp, InterpolatesUnsortedStopsWithLastDuplicateWinningAndUnclampedRgba)
{
    const std::vector<ColorRampPoint> points{
        { 1.f, { 4.f, 2.f, 0.f, 6.f } },
        { 0.f, { 1.f, 0.f, 0.f, 2.f } },
        { 0.f, { 2.f, 0.f, 2.f, 4.f } },
    };
    EXPECT_EQ(sampleColorRamp(points, 0.5f), (std::array<float, 4>{ 3.f, 1.f, 1.f, 5.f }));
    EXPECT_EQ(sampleColorRamp(points, 0.f), points[2].color);
    EXPECT_EQ(sampleColorRamp(points, -1.f), points[2].color);
    EXPECT_EQ(sampleColorRamp(points, 2.f), points[0].color);
}

TEST(ColorRamp, InsertionPreservesCurveAndEnforcesCapacityWhileRemovalKeepsOneStop)
{
    std::vector<ColorRampPoint> points{
        { 1.f, { 4.f, 2.f, 0.f, 6.f } },
        { 0.f, { 2.f, 0.f, 2.f, 4.f } },
    };
    const auto original = points;
    ASSERT_TRUE(addColorRampPoint(points, 3, 0.5f));
    EXPECT_EQ(points.back().color, (std::array<float, 4>{ 3.f, 1.f, 1.f, 5.f }));
    for (int i = 0; i <= 10; ++i)
    {
        const auto actual = sampleColorRamp(points, i * 0.1f);
        const auto expected = sampleColorRamp(original, i * 0.1f);
        for (size_t channel = 0; channel < actual.size(); ++channel)
        {
            EXPECT_FLOAT_EQ(actual[channel], expected[channel]);
        }
    }
    EXPECT_EQ(points[0].position, original[0].position);
    EXPECT_EQ(points[1].color, original[1].color);
    EXPECT_FALSE(addColorRampPoint(points, 3, 0.25f));
    EXPECT_FALSE(addColorRampPoint(points, 8, std::numeric_limits<float>::quiet_NaN()));
    EXPECT_FALSE(removeColorRampPoint(points, 3));
    ASSERT_TRUE(removeColorRampPoint(points, 1));
    EXPECT_FLOAT_EQ(points[1].position, 0.5f);
    ASSERT_TRUE(removeColorRampPoint(points, 0));
    EXPECT_FALSE(removeColorRampPoint(points, 0));
}

class ColorRampRenderTest : public ::testing::TestWithParam<bool>
{
protected:
    struct Layout
    {
        ImVec2 origin;
        ImRect bounds;
        float bar_y;
        float bar_width;
        float color_y;
        float color_component_width;
        float add_width;
        float frame_height;
        float inner_spacing;
        bool changed;
    };

    void SetUp() override
    {
        ImGui::CreateContext();
        auto& io = ImGui::GetIO();
        io.ConfigMacOSXBehaviors = GetParam();
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

    Layout frame()
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(288, 400));
        ImGui::Begin("Controls", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        const auto origin = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        const float frame_height = ImGui::GetFrameHeight();
        const auto& style = ImGui::GetStyle();
        const float bar_y = origin.y + ImGui::GetTextLineHeightWithSpacing();
        const float color_y = bar_y + 38.f + style.ItemSpacing.y + 2.f * (frame_height + style.ItemSpacing.y);
        const float item_width = width - ImGui::CalcTextSize("Position").x - style.ItemInnerSpacing.x;
        const float component_width = (item_width - frame_height - 4.f * style.ItemInnerSpacing.x) / 4.f;
        const float add_width = ImGui::CalcTextSize("Add stop").x + 2.f * style.FramePadding.x;
        const bool changed = renderColorRamp("Ramp", points, capacity);
        const ImRect bounds = ImGui::GetCurrentContext()->LastItemData.Rect;
        const Layout layout{ origin, bounds, bar_y, width - 12.f, color_y, component_width, add_width,
                             frame_height, style.ItemInnerSpacing.x, changed };
        ImGui::End();
        ImGui::Render();
        return layout;
    }

    void click(const ImVec2& position)
    {
        auto& io = ImGui::GetIO();
        io.AddMousePosEvent(position.x, position.y);
        frame();
        io.AddMouseButtonEvent(0, true);
        frame();
        io.AddMouseButtonEvent(0, false);
        frame();
    }

    void typeAt(const ImVec2& position, const char* text)
    {
        auto& io = ImGui::GetIO();
        const auto shortcut = io.ConfigMacOSXBehaviors ? ImGuiMod_Super : ImGuiMod_Ctrl;
        io.AddKeyEvent(shortcut, true);
        click(position);
        io.AddKeyEvent(ImGuiKey_A, true);
        frame();
        io.AddKeyEvent(ImGuiKey_A, false);
        io.AddKeyEvent(shortcut, false);
        io.AddInputCharactersUTF8(text);
        frame();
        io.AddKeyEvent(ImGuiKey_Enter, true);
        frame();
        io.AddKeyEvent(ImGuiKey_Enter, false);
        frame();
    }

    std::vector<ColorRampPoint> points{
        { 0.f, { 2.f, 0.f, 0.f, 2.f } },
        { 1.f, { 0.f, 0.f, 4.f, 6.f } },
    };
    size_t capacity = 8;
};

TEST_P(ColorRampRenderTest, RenderingPreservesUnsortedDuplicateSlotsAndHdrValues)
{
    points.push_back({ 0.f, { 0.f, 3.f, 0.f, 4.f } });
    const auto original = points;
    for (int iteration = 0; iteration < 3; ++iteration)
    {
        EXPECT_FALSE(frame().changed);
    }
    for (size_t i = 0; i < points.size(); ++i)
    {
        EXPECT_EQ(points[i].position, original[i].position);
        EXPECT_EQ(points[i].color, original[i].color);
    }
}

TEST_P(ColorRampRenderTest, DraggingMovesTheSelectedSlotWithoutReorderingItsColor)
{
    points.push_back({ 0.f, { 0.f, 3.f, 0.f, 4.f } });
    const auto layout = frame();
    auto& io = ImGui::GetIO();
    io.AddMousePosEvent(layout.origin.x + 6.f, layout.bar_y + 30.f);
    frame();
    io.AddMouseButtonEvent(0, true);
    frame();
    io.AddMousePosEvent(layout.origin.x + 6.f + layout.bar_width * 0.5f, layout.bar_y + 30.f);
    EXPECT_TRUE(frame().changed);
    EXPECT_FLOAT_EQ(points[2].position, 0.5f);
    EXPECT_EQ(points[2].color, (std::array<float, 4>{ 0.f, 3.f, 0.f, 4.f }));
    EXPECT_FLOAT_EQ(points[0].position, 0.f);
    EXPECT_FLOAT_EQ(points[1].position, 1.f);
    io.AddMousePosEvent(layout.origin.x + layout.bar_width * 2.f, layout.bar_y + 30.f);
    frame();
    EXPECT_FLOAT_EQ(points[2].position, 1.f);
    io.AddMouseButtonEvent(0, false);
    frame();
}

TEST_P(ColorRampRenderTest, AddAndRemoveButtonsRespectCapacityAndMinimum)
{
    capacity = 3;
    auto layout = frame();
    const ImVec2 add(layout.origin.x + layout.add_width * 0.5f, layout.bounds.Max.y - layout.frame_height * 0.5f);
    click(add);
    ASSERT_EQ(points.size(), 3u);
    EXPECT_FLOAT_EQ(points[2].position, 0.5f);
    EXPECT_EQ(points[2].color, (std::array<float, 4>{ 1.f, 0.f, 2.f, 4.f }));
    click(add);
    EXPECT_EQ(points.size(), 3u);
    const ImVec2 remove(layout.origin.x + layout.add_width + 30.f, add.y);
    click(remove);
    ASSERT_EQ(points.size(), 2u);
    EXPECT_FLOAT_EQ(points[0].position, 0.f);
    EXPECT_FLOAT_EQ(points[1].position, 1.f);
    click(remove);
    ASSERT_EQ(points.size(), 1u);
    click(remove);
    EXPECT_EQ(points.size(), 1u);
}

TEST_P(ColorRampRenderTest, ColorInputsEditRgbAndAlphaAboveOneWithoutChangingOtherStops)
{
    const auto untouched = points[1];
    auto layout = frame();
    typeAt(ImVec2(layout.origin.x + layout.color_component_width * 0.5f,
                  layout.color_y + layout.frame_height * 0.5f), "3.5");
    EXPECT_FLOAT_EQ(points[0].color[0], 3.5f);
    layout = frame();
    typeAt(ImVec2(layout.origin.x + 3.f * (layout.color_component_width + layout.inner_spacing) +
                      layout.color_component_width * 0.5f,
                  layout.color_y + layout.frame_height * 0.5f), "5.5");
    EXPECT_FLOAT_EQ(points[0].color[3], 5.5f);
    EXPECT_EQ(points[1].color, untouched.color);
    EXPECT_FLOAT_EQ(points[1].position, untouched.position);
}

INSTANTIATE_TEST_SUITE_P(KeyboardBehaviors, ColorRampRenderTest, ::testing::Bool());

} // namespace
