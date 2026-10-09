// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include "editor/ColorRamp.h"

#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
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
        next_id = ImGui::GetID("next");
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

    ImGuiWindow* popup()
    {
        auto& stack = ImGui::GetCurrentContext()->OpenPopupStack;
        return stack.empty() ? nullptr : stack.back().Window;
    }

    void openPicker()
    {
        frame();
        activate(color_button_id);
        frame();
    }

    void closePicker()
    {
        ImGui::ClosePopupToLevel(0, true);
        frame();
    }

    ImRect pickerSquare()
    {
        auto* window = popup();
        if (!window)
        {
            return {};
        }
        const auto& vertices = window->DrawList->VtxBuffer;
        ImRect square;
        float area = 0.f;
        for (int i = 0; i + 3 < vertices.Size; ++i)
        {
            const auto& a = vertices[i].pos;
            const auto& b = vertices[i + 1].pos;
            const auto& c = vertices[i + 2].pos;
            const auto& d = vertices[i + 3].pos;
            if (a.y == b.y && b.x == c.x && c.y == d.y && d.x == a.x && b.x > a.x && c.y > a.y &&
                std::abs((b.x - a.x) - (c.y - a.y)) < 0.01f && (b.x - a.x) * (c.y - a.y) > area)
            {
                square = ImRect(a, c);
                area = square.GetWidth() * square.GetHeight();
            }
        }
        return square;
    }

    ImVec2 pickerPosition(const char* control, float x, float y)
    {
        const auto square = pickerSquare();
        if (std::strcmp(control, "sv") == 0)
        {
            return ImVec2(square.Min.x + x * (square.GetWidth() - 1.f), square.Min.y + y * (square.GetHeight() - 1.f));
        }
        const float width = popup()->FontRefSize + 2.f * ImGui::GetStyle().FramePadding.y;
        const float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
        const float offset = std::strcmp(control, "alpha") == 0 ? width + spacing : 0.f;
        return ImVec2(square.Max.x + spacing + width * 0.5f + offset, square.Min.y + y * (square.GetHeight() - 1.f));
    }

    void holdPicker(const char* control, float x, float y, const std::function<void()>& check)
    {
        ASSERT_NE(popup(), nullptr);
        ASSERT_GT(pickerSquare().GetWidth(), 0.f);
        const ImVec2 position = pickerPosition(control, x, y);
        const auto expected_id = ImHashStr(control, 0, popup()->GetID("##picker"));
        auto& io = ImGui::GetIO();
        io.AddMousePosEvent(position.x, position.y);
        frame();
        io.AddMouseButtonEvent(0, true);
        frame();
        ASSERT_EQ(ImGui::GetCurrentContext()->ActiveId, expected_id);
        for (int i = 0; i < 4; ++i)
        {
            frame();
            check();
        }
        io.AddMouseButtonEvent(0, false);
        frame();
        check();
    }

    ImGuiID popupInput(const char* label, int component, bool range = false)
    {
        const auto parent = popup()->GetID(label);
        if (range)
        {
            return ImHashData(&component, sizeof(component), parent);
        }
        const char* components[] = { "##X", "##Y", "##Z", "##W" };
        return ImHashStr(components[component], 0, parent);
    }

    ImGuiID add_id = 0;
    ImGuiID remove_id = 0;
    ImGuiID next_id = 0;
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

TEST_P(ColorRampRenderTest, OpeningHdrPickerPreservesRawRgba)
{
    frame();
    const auto before = points[0].color;
    activate(color_button_id);
    EXPECT_FALSE(ImGui::GetCurrentContext()->OpenPopupStack.empty());
    EXPECT_FALSE(frame().changed);
    EXPECT_EQ(points[0].color, before);
}

TEST_P(ColorRampRenderTest, HueAndSaturationValueDragsKeepTheHdrRangeStable)
{
    points[0].color = { 4.f, 2.f, 0.f, 6.f };
    const auto untouched = points[1];
    openPicker();
    ASSERT_NE(popup(), nullptr);
    const float hue_tolerance = 24.f / pickerSquare().GetHeight();
    holdPicker("hue", 0.5f, 0.5f,
               [&]()
               {
                   EXPECT_NEAR(points[0].color[0], 0.f, hue_tolerance);
                   EXPECT_NEAR(points[0].color[1], 4.f, hue_tolerance);
                   EXPECT_NEAR(points[0].color[2], 4.f, hue_tolerance);
                   EXPECT_FLOAT_EQ(points[0].color[3], 6.f);
               });
    const float sv_tolerance = 48.f / pickerSquare().GetHeight();
    holdPicker("sv", 0.5f, 0.25f,
               [&]()
               {
                   EXPECT_NEAR(points[0].color[0], 1.5f, sv_tolerance);
                   EXPECT_NEAR(points[0].color[1], 3.f, sv_tolerance);
                   EXPECT_NEAR(points[0].color[2], 3.f, sv_tolerance);
                   EXPECT_FLOAT_EQ(points[0].color[3], 6.f);
               });
    EXPECT_EQ(points[1].color, untouched.color);
    EXPECT_FLOAT_EQ(points[1].position, untouched.position);
}

TEST_P(ColorRampRenderTest, AlphaDragKeepsRgbExactlyAndCanRemainAboveOne)
{
    points[0].color = { 3.5f, 1.25f, -0.5f, 6.f };
    const auto before = points[0].color;
    openPicker();
    ASSERT_NE(popup(), nullptr);
    const float tolerance = 12.f / pickerSquare().GetHeight();
    holdPicker("alpha", 0.5f, 0.25f,
               [&]()
               {
                   for (size_t channel = 0; channel < 3; ++channel)
                   {
                       EXPECT_EQ(points[0].color[channel], before[channel]);
                   }
                   EXPECT_NEAR(points[0].color[3], 4.5f, tolerance);
                   EXPECT_GT(points[0].color[3], 1.f);
               });
}

TEST_P(ColorRampRenderTest, RawPopupInputExpandsRangeAndRangeEditsDoNotChangeRgba)
{
    openPicker();
    ASSERT_NE(popup(), nullptr);
    typeAt(popupInput("RGBA", 0), "8");
    EXPECT_FLOAT_EQ(points[0].color[0], 8.f);
    const float tolerance = 24.f / pickerSquare().GetHeight();
    holdPicker("sv", 1.f, 0.25f, [&]() { EXPECT_NEAR(points[0].color[0], 6.f, tolerance); });
    typeAt(popupInput("RGBA", 3), "8");
    EXPECT_FLOAT_EQ(points[0].color[3], 8.f);
    holdPicker("alpha", 0.5f, 0.25f, [&]() { EXPECT_NEAR(points[0].color[3], 6.f, tolerance); });
    const auto before = points[0].color;
    typeAt(popupInput("RGB range", 1, true), "12");
    EXPECT_EQ(points[0].color, before);
    typeAt(popupInput("Alpha range", 1, true), "10");
    EXPECT_EQ(points[0].color, before);
    typeAt(popupInput("RGB range", 1, true), "1");
    EXPECT_EQ(points[0].color, before);
    typeAt(popupInput("RGB range", 1, true), "-12");
    EXPECT_EQ(points[0].color, before);
    typeAt(popupInput("RGB range", 1, true), "1e39");
    EXPECT_EQ(points[0].color, before);
    holdPicker("sv", 1.f, 0.5f,
               [&]()
               {
                   EXPECT_NEAR(points[0].color[0], 6.f, tolerance);
                   EXPECT_FLOAT_EQ(points[0].color[3], before[3]);
               });
}

TEST_P(ColorRampRenderTest, ReopeningPickerUsesTheNewSelectedStopsRange)
{
    points[0].color = { 4.f, 2.f, 0.f, 6.f };
    points[1].color = { 10.f, 0.f, 0.f, 8.f };
    openPicker();
    ASSERT_NE(popup(), nullptr);
    closePicker();
    activate(next_id);
    const auto first = points[0].color;
    const auto second = points[1].color;
    openPicker();
    ASSERT_NE(popup(), nullptr);
    EXPECT_EQ(points[1].color, second);
    const float tolerance = 20.f / pickerSquare().GetHeight();
    holdPicker("sv", 1.f, 0.5f, [&]() { EXPECT_NEAR(points[1].color[0], 5.f, tolerance); });
    EXPECT_FLOAT_EQ(points[1].color[3], 8.f);
    EXPECT_EQ(points[0].color, first);
}

TEST_P(ColorRampRenderTest, LdrPickerUsesTheUnitRange)
{
    ImGui::SetColorEditOptions(ImGuiColorEditFlags_InputHSV | ImGuiColorEditFlags_DisplayHSV |
                               ImGuiColorEditFlags_Float | ImGuiColorEditFlags_PickerHueWheel);
    points[0].color = { 0.8f, 0.4f, 0.f, 0.8f };
    openPicker();
    ASSERT_NE(popup(), nullptr);
    const float tolerance = 2.f / pickerSquare().GetHeight();
    holdPicker("sv", 1.f, 0.25f,
               [&]()
               {
                   EXPECT_NEAR(points[0].color[0], 0.75f, tolerance);
                   EXPECT_NEAR(points[0].color[1], 0.375f, tolerance);
                   EXPECT_NEAR(points[0].color[2], 0.f, tolerance);
                   EXPECT_FLOAT_EQ(points[0].color[3], 0.8f);
               });
}

TEST_P(ColorRampRenderTest, LargeNegativeRangesReachTheirUpperEndpointExactly)
{
    points[0].color = { -1e20f, -1e20f, -1e20f, -1e20f };
    openPicker();
    ASSERT_NE(popup(), nullptr);
    holdPicker("sv", 1.f, 0.f,
               [&]()
               {
                   EXPECT_FLOAT_EQ(points[0].color[0], 1.f);
                   EXPECT_FLOAT_EQ(points[0].color[1], -1e20f);
                   EXPECT_FLOAT_EQ(points[0].color[2], -1e20f);
                   EXPECT_FLOAT_EQ(points[0].color[3], -1e20f);
               });
    holdPicker("alpha", 0.5f, 0.f,
               [&]()
               {
                   EXPECT_FLOAT_EQ(points[0].color[0], 1.f);
                   EXPECT_FLOAT_EQ(points[0].color[1], -1e20f);
                   EXPECT_FLOAT_EQ(points[0].color[2], -1e20f);
                   EXPECT_FLOAT_EQ(points[0].color[3], 1.f);
               });
}

TEST_P(ColorRampRenderTest, NegativeAndExtremeFiniteValuesRemainFiniteInPicker)
{
    const float maximum = std::numeric_limits<float>::max();
    points[0].color = { -maximum, maximum, 0.f, maximum };
    const auto before = points[0].color;
    openPicker();
    ASSERT_NE(popup(), nullptr);
    EXPECT_EQ(points[0].color, before);
    holdPicker("sv", 0.5f, 0.25f,
               [&]()
               {
                   for (const auto value : points[0].color)
                   {
                       EXPECT_TRUE(std::isfinite(value));
                   }
                   EXPECT_GT(points[0].color[1], 1.f);
                   EXPECT_LT(points[0].color[0], 0.f);
                   EXPECT_FLOAT_EQ(points[0].color[3], maximum);
               });
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
    const char* guidance =
        "Drag or type HDR RGBA values, including values above one.\nClick the swatch to open the HDR picker.";
    activate(color_input_ids[0]);
    auto* context = ImGui::GetCurrentContext();
    ASSERT_EQ(context->NavId, color_input_ids[0]);
    auto first = context->NavWindow->NavRectRel[context->NavLayer];
    first.Translate(context->NavWindow->Pos);
    hover(first.GetCenter(), guidance);
    activate(color_input_ids[3]);
    ASSERT_EQ(context->NavId, color_input_ids[3]);
    auto last = context->NavWindow->NavRectRel[context->NavLayer];
    last.Translate(context->NavWindow->Pos);
    const float width = first.GetHeight();
    const float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
    const float swatch_x = ImGui::GetStyle().ColorButtonPosition == ImGuiDir_Left ? first.Min.x - spacing - width * 0.5f :
                                                                                    last.Max.x + spacing + width * 0.5f;
    activate(color_button_id);
    ASSERT_NE(popup(), nullptr);
    closePicker();
    hover(ImVec2(swatch_x, first.GetCenter().y), guidance);
    EXPECT_EQ(context->HoveredId, color_button_id);
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
