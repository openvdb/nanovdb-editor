// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include "editor/ColorRamp.h"

#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <tuple>

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

class ColorRampRenderTest : public ::testing::TestWithParam<std::tuple<bool, float, bool>>
{
protected:
    struct Layout
    {
        ImVec2 origin;
        ImRect bar;
        bool changed;
        ImVec2 expected_text_size;
    };

    void SetUp() override
    {
        ImGui::CreateContext();
        auto& io = ImGui::GetIO();
        io.ConfigMacOSXBehaviors = std::get<0>(GetParam());
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1600, 1400);
        io.DeltaTime = 1.f / 60.f;
        io.FontGlobalScale = std::get<1>(GetParam());
        ImFontConfig font;
        if (std::get<2>(GetParam()))
        {
            font.SizePixels = 17.f;
            auto& style = ImGui::GetStyle();
            style.FramePadding = ImVec2(6.f, 5.f);
            style.ItemSpacing = ImVec2(11.f, 7.f);
            style.ItemInnerSpacing = ImVec2(9.f, 6.f);
            style.WindowPadding = ImVec2(13.f, 10.f);
            style.ColorButtonPosition = ImGuiDir_Left;
        }
        io.Fonts->AddFontDefault(&font);
        ImGui::GetStyle().ScaleAllSizes(io.FontGlobalScale);
        unsigned char* pixels;
        int width, height;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    }

    void TearDown() override
    {
        ImGui::DestroyContext();
    }

    Layout frame(const char* expected_text = nullptr)
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        const float font_size = ImGui::GetFontSize();
        ImGui::SetNextWindowSize(ImVec2(28.f * font_size, 36.f * font_size));
        ImGui::Begin("Controls", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        const auto origin = ImGui::GetCursorScreenPos();
        const float bar_y = origin.y + ImGui::GetTextLineHeightWithSpacing();
        ImGui::PushID("Ramp");
        add_id = ImGui::GetID("Add stop");
        remove_id = ImGui::GetID("Remove stop");
        ImGui::PushID("Color");
        color_button_id = ImGui::GetID("##ColorButton");
        const char* components[] = { "##X", "##Y", "##Z", "##W" };
        for (size_t i = 0; i < color_input_ids.size(); ++i)
        {
            color_input_ids[i] = ImGui::GetID(components[i]);
        }
        ImGui::PopID();
        ImGui::PopID();
        const bool changed = renderColorRamp("Ramp", points, capacity, author_hint);
        const auto& vertices = ImGui::GetWindowDrawList()->VtxBuffer;
        ImRect bar(origin, origin);
        for (int i = 0; i + 3 < vertices.Size; ++i)
        {
            const auto& a = vertices[i].pos;
            const auto& b = vertices[i + 1].pos;
            const auto& c = vertices[i + 2].pos;
            const auto& d = vertices[i + 3].pos;
            if (std::abs(a.y - bar_y) > 0.01f || std::abs(b.y - bar_y) > 0.01f || b.x <= a.x || c.x != b.x ||
                d.x != a.x || c.y != d.y || c.y <= a.y)
            {
                continue;
            }
            const float height = c.y - a.y;
            if (height > bar.GetHeight())
            {
                bar = ImRect(a, c);
            }
            else if (height == bar.GetHeight())
            {
                bar.Add(a);
                bar.Add(c);
            }
        }
        EXPECT_GT(bar.GetHeight(), 0.f);
        const Layout layout{ origin, bar, changed, expected_text ? ImGui::CalcTextSize(expected_text) : ImVec2() };
        ImGui::End();
        ImGui::Render();
        return layout;
    }

    void activate(ImGuiID id)
    {
        ImGui::ActivateItemByID(id);
        frame();
    }

    void typeAt(ImGuiID id, const char* text)
    {
        auto& io = ImGui::GetIO();
        ImGui::ActivateItemByID(id);
        ImGui::GetCurrentContext()->NavNextActivateFlags = ImGuiActivateFlags_PreferInput;
        frame();
        io.AddInputCharactersUTF8(text);
        frame();
        io.AddKeyEvent(ImGuiKey_Enter, true);
        frame();
        io.AddKeyEvent(ImGuiKey_Enter, false);
        frame();
    }

    ImGuiID add_id = 0;
    ImGuiID remove_id = 0;
    ImGuiID color_button_id = 0;
    std::array<ImGuiID, 4> color_input_ids{};

    std::vector<ColorRampPoint> points{
        { 0.f, { 2.f, 0.f, 0.f, 2.f } },
        { 1.f, { 0.f, 0.f, 4.f, 6.f } },
    };
    size_t capacity = 8;
    const char* author_hint = nullptr;
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

TEST_P(ColorRampRenderTest, PreviewClampsInterpolatedHdrRgbaAndPreservesItWhenAddingAStop)
{
    points = {
        { 0.f, { 4.f, -2.f, 3.f, 4.f } },
        { 1.f, { 0.f, 2.f, -1.f, 0.f } },
    };
    const std::array<float, 5> positions{ 0.125f, 0.375f, 0.5f, 0.625f, 0.875f };
    const auto preview = [&]()
    {
        frame();
        const auto layout = frame();
        const auto& vertices = ImGui::FindWindowByName("Controls")->DrawList->VtxBuffer;
        std::array<std::array<float, 4>, positions.size()> colors{};
        std::array<bool, positions.size()> found{};
        const std::array<int, 4> shifts{ IM_COL32_R_SHIFT, IM_COL32_G_SHIFT, IM_COL32_B_SHIFT, IM_COL32_A_SHIFT };
        for (int i = 0; i + 3 < vertices.Size; ++i)
        {
            const auto& left = vertices[i];
            const auto& right = vertices[i + 1];
            const auto& bottom = vertices[i + 2];
            if (std::abs(left.pos.y - layout.bar.Min.y) > 0.01f || std::abs(right.pos.y - layout.bar.Min.y) > 0.01f ||
                std::abs(bottom.pos.y - layout.bar.Max.y) > 0.01f || right.pos.x <= left.pos.x)
            {
                continue;
            }
            for (size_t sample = 0; sample < positions.size(); ++sample)
            {
                const float x = layout.bar.Min.x + layout.bar.GetWidth() * positions[sample];
                if (x < left.pos.x || x > right.pos.x)
                {
                    continue;
                }
                found[sample] = true;
                const float weight = (x - left.pos.x) / (right.pos.x - left.pos.x);
                for (size_t channel = 0; channel < shifts.size(); ++channel)
                {
                    const float a = static_cast<float>((left.col >> shifts[channel]) & 255u) / 255.f;
                    const float b = static_cast<float>((right.col >> shifts[channel]) & 255u) / 255.f;
                    colors[sample][channel] = a + weight * (b - a);
                }
            }
        }
        for (size_t sample = 0; sample < positions.size(); ++sample)
        {
            EXPECT_TRUE(found[sample]) << positions[sample];
        }
        return colors;
    };
    const auto before = preview();
    ASSERT_TRUE(addColorRampPoint(points, capacity, 0.5f));
    const auto after = preview();
    for (size_t sample = 0; sample < positions.size(); ++sample)
    {
        const auto expected = sampleColorRamp(points, positions[sample]);
        for (size_t channel = 0; channel < expected.size(); ++channel)
        {
            EXPECT_NEAR(before[sample][channel], std::clamp(expected[channel], 0.f, 1.f), 1.f / 255.f)
                << "position " << positions[sample] << ", channel " << channel;
            EXPECT_NEAR(after[sample][channel], before[sample][channel], 1.f / 255.f)
                << "position " << positions[sample] << ", channel " << channel;
        }
    }
}

TEST_P(ColorRampRenderTest, DraggingMovesTheSelectedSlotWithoutReorderingItsColor)
{
    points.push_back({ 0.f, { 0.f, 3.f, 0.f, 4.f } });
    const auto layout = frame();
    auto& io = ImGui::GetIO();
    io.AddMousePosEvent(layout.bar.Min.x, layout.bar.GetCenter().y);
    frame();
    io.AddMouseButtonEvent(0, true);
    frame();
    io.AddMousePosEvent(layout.bar.GetCenter().x, layout.bar.GetCenter().y);
    EXPECT_TRUE(frame().changed);
    EXPECT_NEAR(points[2].position, 0.5f, 1.f / layout.bar.GetWidth());
    EXPECT_EQ(points[2].color, (std::array<float, 4>{ 0.f, 3.f, 0.f, 4.f }));
    EXPECT_FLOAT_EQ(points[0].position, 0.f);
    EXPECT_FLOAT_EQ(points[1].position, 1.f);
    io.AddMousePosEvent(layout.bar.Min.x + layout.bar.GetWidth() * 2.f, layout.bar.GetCenter().y);
    frame();
    EXPECT_FLOAT_EQ(points[2].position, 1.f);
    io.AddMouseButtonEvent(0, false);
    frame();
}

TEST_P(ColorRampRenderTest, AddAndRemoveButtonsRespectCapacityAndMinimum)
{
    capacity = 3;
    frame();
    activate(add_id);
    ASSERT_EQ(points.size(), 3u);
    EXPECT_FLOAT_EQ(points[2].position, 0.5f);
    EXPECT_EQ(points[2].color, (std::array<float, 4>{ 1.f, 0.f, 2.f, 4.f }));
    activate(add_id);
    EXPECT_EQ(points.size(), 3u);
    activate(remove_id);
    ASSERT_EQ(points.size(), 2u);
    EXPECT_FLOAT_EQ(points[0].position, 0.f);
    EXPECT_FLOAT_EQ(points[1].position, 1.f);
    activate(remove_id);
    ASSERT_EQ(points.size(), 1u);
    activate(remove_id);
    EXPECT_EQ(points.size(), 1u);
}

TEST_P(ColorRampRenderTest, ColorInputsEditRgbAndAlphaAboveOneWithoutChangingOtherStops)
{
    const auto untouched = points[1];
    frame();
    typeAt(color_input_ids[0], "3.5");
    EXPECT_FLOAT_EQ(points[0].color[0], 3.5f);
    typeAt(color_input_ids[3], "5.5");
    EXPECT_FLOAT_EQ(points[0].color[3], 5.5f);
    EXPECT_EQ(points[1].color, untouched.color);
    EXPECT_FLOAT_EQ(points[1].position, untouched.position);
}

TEST_P(ColorRampRenderTest, HdrPreviewDoesNotOpenABoundedPicker)
{
    frame();
    const auto before = points[0].color;
    activate(color_button_id);
    EXPECT_TRUE(ImGui::GetCurrentContext()->OpenPopupStack.empty());
    EXPECT_EQ(points[0].color, before);
}

TEST_P(ColorRampRenderTest, AuthorHintDoesNotReplaceColorEditingGuidance)
{
    author_hint = "Density selects a ramp position.";
    const auto layout = frame();
    const auto hover = [&](const ImVec2& position, const char* expected)
    {
        ImGui::GetIO().AddMousePosEvent(position.x, position.y);
        frame();
        const auto hovered = frame(expected);
        ImGuiWindow* tooltip = nullptr;
        for (auto* window : ImGui::GetCurrentContext()->Windows)
        {
            if ((window->Flags & ImGuiWindowFlags_Tooltip) && window->Active && !window->Hidden)
            {
                ASSERT_EQ(tooltip, nullptr);
                tooltip = window;
            }
        }
        ASSERT_NE(tooltip, nullptr);
        EXPECT_NEAR(tooltip->ContentSize.x, hovered.expected_text_size.x, 1.f);
        EXPECT_NEAR(tooltip->ContentSize.y, hovered.expected_text_size.y, 1.f);
    };
    for (auto id : { color_input_ids[0], color_button_id })
    {
        activate(id);
        auto* context = ImGui::GetCurrentContext();
        ASSERT_EQ(context->NavId, id);
        auto bounds = context->NavWindow->NavRectRel[context->NavLayer];
        bounds.Translate(context->NavWindow->Pos);
        hover(bounds.GetCenter(),
              "Drag or type HDR RGBA values, including values above one.\nThe swatch does not open a picker.");
    }
    hover(layout.bar.GetCenter(), author_hint);
}

TEST_P(ColorRampRenderTest, PreviewAndMarkerInsetsScaleWithFontAndStyle)
{
    const auto before = frame();
    ImGui::GetIO().FontGlobalScale *= 2.f;
    ImGui::GetStyle().ScaleAllSizes(2.f);
    const auto after = frame();
    EXPECT_FLOAT_EQ(after.bar.GetHeight(), 2.f * before.bar.GetHeight());
    EXPECT_FLOAT_EQ(after.bar.Min.x - after.origin.x, 2.f * (before.bar.Min.x - before.origin.x));
}

INSTANTIATE_TEST_SUITE_P(KeyboardScaleAndStyle,
                         ColorRampRenderTest,
                         ::testing::Combine(::testing::Bool(), ::testing::Values(1.f, 2.f), ::testing::Bool()));

} // namespace
