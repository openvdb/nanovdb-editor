// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include "editor/CustomSceneParams.h"
#include "ImGuiTestSupport.h"

#include <imgui_internal.h>

#include <cstring>
#include <string>

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
        int text_color_vertices;
        float content_right;
        std::string text;
    };

    void SetUp() override
    {
        pnanovdb_editor_test::create_imgui_context();
    }

    void TearDown() override
    {
        ImGui::DestroyContext();
    }

    Item frame(bool prefix = false, float width = 600.f)
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(width, 400));
        ImGui::Begin("Controls", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        if (prefix)
            ImGui::Button("External control");
        const auto start = ImGui::GetCursorScreenPos();
        const float content_right = start.x + ImGui::GetContentRegionAvail().x;
        ImGui::LogToBuffer();
        params.render();
        const auto& last = ImGui::GetCurrentContext()->LastItemData;
        Item item{ last.ID, last.Rect, start, 0, 0, content_right, ImGui::GetCurrentContext()->LogBuffer.c_str() };
        ImGui::LogFinish();
        const ImU32 active_color = ImGui::GetColorU32(ImGuiCol_ButtonActive);
        const ImU32 text_color = ImGui::GetColorU32(ImGuiCol_Text);
        for (const auto& vertex : ImGui::GetWindowDrawList()->VtxBuffer)
        {
            if (vertex.col == active_color)
                ++item.active_color_vertices;
            if (vertex.col == text_color)
                ++item.text_color_vertices;
        }
        ImGui::End();
        ImGui::Render();
        return item;
    }

    Item click(Item item)
    {
        auto& io = ImGui::GetIO();
        const ImVec2 center = item.rect.GetCenter();
        io.AddMousePosEvent(center.x, center.y);
        frame();
        io.AddMouseButtonEvent(0, true);
        frame();
        io.AddMouseButtonEvent(0, false);
        frame();
        io.AddMousePosEvent(-100, -100);
        return frame();
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

TEST_F(CustomSceneParamsRenderTest, ButtonLatchesUntilConsumed)
{
    ASSERT_TRUE(params.loadFromJsonString(R"({"SceneParams": {
        "Other": {"type":"bool", "widget":"button"},
        "Step": {"type":"bool", "widget":"button", "tooltip":"Advance one frame"}
    }})"));
    auto* step = value("Step");
    auto* other = value("Other");
    ASSERT_NE(step, nullptr);
    ASSERT_NE(other, nullptr);
    click(frame());
    EXPECT_EQ(*step, PNANOVDB_TRUE);
    EXPECT_EQ(*other, PNANOVDB_FALSE);
    frame();
    EXPECT_EQ(*step, PNANOVDB_TRUE);
    *step = PNANOVDB_FALSE;
    frame();
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
    ASSERT_NE(inactive.id, 0u);
    EXPECT_EQ(inactive.active_color_vertices, 0);
    EXPECT_NE(inactive.text.find("Play"), std::string::npos);
    const Item active = click(inactive);
    EXPECT_EQ(*play, PNANOVDB_TRUE);
    EXPECT_EQ(active.id, inactive.id);
    EXPECT_GT(active.active_color_vertices, 0);
    EXPECT_NE(active.text.find("Stop"), std::string::npos);
    EXPECT_EQ(active.text.find("Play"), std::string::npos);
    const Item stopped = click(active);
    EXPECT_EQ(*play, PNANOVDB_FALSE);
    EXPECT_EQ(stopped.id, inactive.id);
    EXPECT_EQ(stopped.active_color_vertices, 0);
    EXPECT_NE(stopped.text.find("Play"), std::string::npos);
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

TEST_F(CustomSceneParamsRenderTest, ReadOnlyFieldsRejectInputAndShowMappedUpdates)
{
    const char* fields[] = {
        R"({"type":"uint", "value":12})",
        R"({"type":"string", "length":32, "value":"initial"})",
    };
    for (const char* field : fields)
    {
        SCOPED_TRACE(field);
        auto schema = nlohmann::ordered_json::parse(std::string("{\"SceneParams\":{\"Status\":") + field + "}}");
        for (const bool read_only : { false, true })
        {
            SCOPED_TRACE(read_only);
            ImGui::ClearActiveID();
            schema["SceneParams"]["Status"]["readOnly"] = read_only;
            ASSERT_TRUE(params.loadFromJsonString(schema.dump()));
            const std::string initial(static_cast<char*>(params.data()), params.dataSize());
            const Item item = frame();
            auto& io = ImGui::GetIO();
            const auto center = item.rect.GetCenter();
            io.AddMousePosEvent(center.x, center.y);
            frame();
            io.AddMouseButtonEvent(0, true);
            frame();
            io.AddMousePosEvent(center.x + 100, center.y);
            io.AddInputCharactersUTF8("9");
            frame();
            io.AddMouseButtonEvent(0, false);
            frame();
            const std::string edited(static_cast<char*>(params.data()), initial.size());
            if (!read_only)
            {
                EXPECT_NE(edited, initial);
                continue;
            }
            EXPECT_EQ(edited, initial);
            if (params.dataType()->child_reflect_datas[0].data_type->data_type == PNANOVDB_REFLECT_TYPE_UINT32)
            {
                *reinterpret_cast<pnanovdb_uint32_t*>(params.data()) = 13u;
                EXPECT_NE(frame().text.find("13"), std::string::npos);
            }
            else
            {
                std::strcpy(static_cast<char*>(params.data()), "updated");
                EXPECT_NE(frame().text.find("updated"), std::string::npos);
            }
        }
    }
}

TEST_F(CustomSceneParamsRenderTest, ReloadAsReadOnlyStopsActiveNumericEdit)
{
    for (const bool mac_behavior : { false, true })
    {
        SCOPED_TRACE(mac_behavior);
        ImGui::GetIO().ConfigMacOSXBehaviors = mac_behavior;
        for (const bool text_input : { false, true })
        {
            SCOPED_TRACE(text_input);
            ASSERT_TRUE(params.loadFromJsonString(R"({"SceneParams": {
                "Value": {"type":"uint", "value":12, "step":1}
            }})"));
            auto& io = ImGui::GetIO();
            const ImGuiKey shortcut_modifier = mac_behavior ? ImGuiMod_Super : ImGuiMod_Ctrl;
            const auto center = frame().rect.GetCenter();
            io.AddMousePosEvent(center.x, center.y);
            frame();
            if (text_input)
            {
                io.AddKeyEvent(shortcut_modifier, true);
                frame();
            }
            io.AddMouseButtonEvent(0, true);
            frame();
            ASSERT_NE(ImGui::GetCurrentContext()->ActiveId, 0u);
            if (text_input)
            {
                ASSERT_NE(ImGui::GetCurrentContext()->TempInputId, 0u);
                ASSERT_EQ(ImGui::GetCurrentContext()->TempInputId, ImGui::GetCurrentContext()->ActiveId);
                io.AddMouseButtonEvent(0, false);
                io.AddKeyEvent(shortcut_modifier, false);
                frame();
                io.AddInputCharactersUTF8("9");
                frame();
                frame();
                ASSERT_EQ(*static_cast<pnanovdb_uint32_t*>(params.data()), 9u);
                ASSERT_EQ(ImGui::GetCurrentContext()->TempInputId, ImGui::GetCurrentContext()->ActiveId);
            }
            ASSERT_TRUE(params.loadFromJsonString(R"({"SceneParams": {
                "Value": {"type":"uint", "value":42, "step":1, "readOnly":true}
            }})"));
            io.AddMousePosEvent(610, center.y);
            if (text_input)
            {
                io.AddInputCharactersUTF8("8");
            }
            frame();
            if (text_input)
            {
                io.AddKeyEvent(ImGuiKey_Enter, true);
            }
            else
            {
                io.AddMousePosEvent(630, center.y);
            }
            frame();
            EXPECT_EQ(*static_cast<pnanovdb_uint32_t*>(params.data()), 42u);
            io.AddMouseButtonEvent(0, false);
            io.AddKeyEvent(ImGuiKey_Enter, false);
            frame();
            EXPECT_EQ(*static_cast<pnanovdb_uint32_t*>(params.data()), 42u);
        }
    }
}

TEST_F(CustomSceneParamsRenderTest, ReadOnlyReloadDoesNotSubmitActiveText)
{
    auto schema = nlohmann::ordered_json::parse(R"({"SceneParams": {
        "Counter": {"type":"uint", "hidden":true},
        "Text": {"type":"string", "length":32, "value":"initial",
                 "commitOnEnter":true, "submitCounterField":"Counter"}
    }})");
    for (const bool read_only : { false, true })
    {
        SCOPED_TRACE(read_only);
        schema["SceneParams"]["Text"]["readOnly"] = false;
        ASSERT_TRUE(params.loadFromJsonString(schema.dump()));
        click(frame());
        ASSERT_NE(ImGui::GetCurrentContext()->ActiveId, 0u);
        schema["SceneParams"]["Text"]["readOnly"] = read_only;
        ASSERT_TRUE(params.loadFromJsonString(schema.dump()));
        ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, true);
        frame();
        const auto* counter = value("Counter");
        ASSERT_NE(counter, nullptr);
        EXPECT_EQ(*counter, read_only ? 0u : 1u);
        ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, false);
        frame();
    }
}

TEST_F(CustomSceneParamsRenderTest, TooltipAppearsForReadOnlyControl)
{
    ASSERT_TRUE(params.loadFromJsonString(R"({"SceneParams": {
        "Step": {"type":"bool", "widget":"button", "readOnly":true, "tooltip":"Advance 100% of a frame"}
    }})"));
    const Item item = frame();
    const auto center = item.rect.GetCenter();
    ImGui::GetIO().AddMousePosEvent(center.x, center.y);
    frame();
    EXPECT_NE(frame().text.find("Advance 100% of a frame"), std::string::npos);
    bool visible_tooltip = false;
    for (const auto* window : ImGui::GetCurrentContext()->Windows)
    {
        if ((window->Flags & ImGuiWindowFlags_Tooltip) && window->Active && !window->Hidden)
        {
            visible_tooltip = true;
        }
    }
    EXPECT_TRUE(visible_tooltip);
}

TEST_F(CustomSceneParamsRenderTest, TooltipCoversFirstPartOfMultiItemField)
{
    const char* fields[] = {
        R"({"type":"bool", "elementCount":3, "tooltip":"Whole field"})",
        R"({"type":"float", "elementCount":16, "tooltip":"Whole field", "readOnly":true})",
    };
    for (const char* field : fields)
    {
        SCOPED_TRACE(field);
        ASSERT_TRUE(params.loadFromJsonString(std::string("{\"SceneParams\":{\"Values\":") + field + "}}"));
        const Item item = frame();
        const auto& style = ImGui::GetStyle();
        ImGui::GetIO().AddMousePosEvent(item.start.x + ImGui::GetFrameHeight() * 0.5f,
                                      item.start.y + ImGui::GetTextLineHeight() + style.ItemSpacing.y +
                                          ImGui::GetFrameHeight() * 0.5f);
        frame();
        EXPECT_NE(frame().text.find("Whole field"), std::string::npos);
    }
}

TEST_F(CustomSceneParamsRenderTest, SameLineStartsAfterFirstVisibleFieldInThisCall)
{
    ASSERT_TRUE(params.loadFromJsonString(R"({"SceneParams": {
        "Hidden": {"type":"bool", "widget":"button", "hidden":true},
        "Step": {"type":"bool", "widget":"button", "sameLine":true},
        "Restart": {"type":"bool", "widget":"button", "sameLine":true}
    }})"));
    const Item last = frame(true);
    EXPECT_FLOAT_EQ(last.rect.Min.y, last.start.y);
    EXPECT_GT(last.rect.Min.x, last.start.x);
}

TEST_F(CustomSceneParamsRenderTest, ReadOnlyTextAllowsSelectionAndCopy)
{
    ASSERT_TRUE(params.loadFromJsonString(R"({"SceneParams": {
        "Status": {"type":"string", "length":32, "value":"Paused", "readOnly":true}
    }})"));
    const Item item = frame();
    EXPECT_GT(item.text_color_vertices, 0);
    click(item);
    ASSERT_EQ(ImGui::GetCurrentContext()->ActiveId, item.id);
    auto& io = ImGui::GetIO();
    const ImGuiKey shortcut_modifier = io.ConfigMacOSXBehaviors ? ImGuiMod_Super : ImGuiMod_Ctrl;
    io.AddKeyEvent(shortcut_modifier, true);
    io.AddKeyEvent(ImGuiKey_A, true);
    frame();
    io.AddKeyEvent(ImGuiKey_A, false);
    frame();
    const auto& state = ImGui::GetCurrentContext()->InputTextState;
    ASSERT_EQ(state.ID, item.id);
    ASSERT_TRUE(state.HasSelection());
    ImGui::SetClipboardText("");
    io.AddKeyEvent(ImGuiKey_C, true);
    frame();
    EXPECT_STREQ(ImGui::GetClipboardText(), "Paused");
    io.AddKeyEvent(ImGuiKey_C, false);
    io.AddKeyEvent(shortcut_modifier, false);
    frame();
    io.AddInputCharactersUTF8("changed");
    io.AddKeyEvent(ImGuiKey_Backspace, true);
    frame();
    EXPECT_STREQ(static_cast<char*>(params.data()), "Paused");
    io.AddKeyEvent(ImGuiKey_Backspace, false);
    frame();
}

TEST_F(CustomSceneParamsRenderTest, ReadOnlyNumericValuesKeepNormalTextBrightness)
{
    ASSERT_TRUE(params.loadFromJsonString(R"({"SceneParams": {
        "Frame": {"type":"uint", "value":42, "readOnly":true}
    }})"));
    EXPECT_GT(frame().text_color_vertices, 0);
}

TEST_F(CustomSceneParamsRenderTest, ReadOnlyTogglePreservesBothStates)
{
    auto schema = nlohmann::ordered_json::parse(R"({"SceneParams": {
        "Play": {"type":"bool32", "widget":"toggleButton", "activeLabel":"Pause", "readOnly":true}
    }})");
    for (bool active : { false, true })
    {
        schema["SceneParams"]["Play"]["value"] = active;
        ASSERT_TRUE(params.loadFromJsonString(schema.dump()));
        const Item item = click(frame());
        EXPECT_EQ(*value("Play"), active ? PNANOVDB_TRUE : PNANOVDB_FALSE);
        EXPECT_NE(item.text.find(active ? "Pause" : "Play"), std::string::npos);
    }
}

TEST_F(CustomSceneParamsRenderTest, SameLineInputsFitTheRemainingRow)
{
    const char* fields[] = {
        R"({"type":"uint", "value":12})",
        R"({"type":"string", "length":32, "value":"Paused"})",
        R"({"type":"float", "elementCount":3})",
        R"({"type":"float", "elementCount":16})",
    };
    for (const char* field : fields)
    {
        SCOPED_TRACE(field);
        auto schema = nlohmann::ordered_json::parse(std::string(R"({"SceneParams": {
            "Previous": {"type":"uint"}, "Status":)") + field + "}}");
        schema["SceneParams"]["Status"]["sameLine"] = true;
        ASSERT_TRUE(params.loadFromJsonString(schema.dump()));
        const Item item = frame();
        EXPECT_LE(item.rect.Max.x, item.content_right);
        EXPECT_GE(item.rect.Min.x, item.start.x);
    }
}

TEST_F(CustomSceneParamsRenderTest, SameLineInputsWrapWhenTheRowIsFull)
{
    ASSERT_TRUE(params.loadFromJsonString(R"({"SceneParams": {
        "Previous control": {"type":"bool", "widget":"button"},
        "Status": {"type":"string", "length":32, "sameLine":true}
    }})"));
    const Item item = frame(false, 180.f);
    EXPECT_GT(item.rect.Min.y, item.start.y);
    EXPECT_LE(item.rect.Max.x, item.content_right);
}

TEST_F(CustomSceneParamsRenderTest, EmptyActiveLabelKeepsTheFieldName)
{
    ASSERT_TRUE(params.loadFromJsonString(R"({"SceneParams": {
        "Play": {"type":"bool", "widget":"toggleButton", "activeLabel":""}
    }})"));
    const Item inactive = frame();
    const Item active = click(inactive);
    EXPECT_EQ(*value("Play"), PNANOVDB_TRUE);
    EXPECT_EQ(active.id, inactive.id);
    EXPECT_NE(active.text.find("Play"), std::string::npos);
    EXPECT_FLOAT_EQ(active.rect.GetWidth(), inactive.rect.GetWidth());
}

TEST_F(CustomSceneParamsRenderTest, MappedReadOnlyFieldLocksAndUnlocksWithoutReload)
{
    ASSERT_TRUE(params.loadFromJsonString(R"({"SceneParams": {
        "Playing": {"type":"bool32", "hidden":true},
        "Frame": {"type":"uint", "value":42, "hidden":true},
        "Step": {"type":"bool", "widget":"button", "readOnlyField":"Playing"}
    }})"));
    const auto* type = params.dataType();
    auto* playing = value("Playing");
    auto* step = value("Step");
    click(frame());
    ASSERT_EQ(*step, PNANOVDB_TRUE);
    *playing = PNANOVDB_TRUE;
    frame();
    EXPECT_EQ(*step, PNANOVDB_TRUE);
    *step = PNANOVDB_FALSE;
    click(frame());
    EXPECT_EQ(*step, PNANOVDB_FALSE);
    *playing = PNANOVDB_FALSE;
    click(frame());
    EXPECT_EQ(*step, PNANOVDB_TRUE);
    EXPECT_EQ(*value("Frame"), 42u);
    EXPECT_EQ(params.dataType(), type);
}

TEST_F(CustomSceneParamsRenderTest, MappedReadOnlyFieldStopsActiveTextAndSubmission)
{
    ASSERT_TRUE(params.loadFromJsonString(R"({"SceneParams": {
        "Locked": {"type":"bool", "hidden":true},
        "Counter": {"type":"uint", "hidden":true},
        "Text": {"type":"string", "length":32, "value":"initial", "commitOnEnter":true,
                 "submitCounterField":"Counter", "readOnlyField":"Locked"}
    }})"));
    click(frame());
    ASSERT_NE(ImGui::GetCurrentContext()->ActiveId, 0u);
    *value("Locked") = PNANOVDB_TRUE;
    auto& io = ImGui::GetIO();
    io.AddInputCharactersUTF8("changed");
    frame();
    io.AddKeyEvent(ImGuiKey_Enter, true);
    frame();
    EXPECT_STREQ(reinterpret_cast<char*>(value("Text")), "initial");
    EXPECT_EQ(*value("Counter"), 0u);
    io.AddKeyEvent(ImGuiKey_Enter, false);
    frame();
    *value("Locked") = PNANOVDB_FALSE;
    click(frame());
    io.AddInputCharactersUTF8("changed");
    frame();
    io.AddKeyEvent(ImGuiKey_Enter, true);
    frame();
    EXPECT_STRNE(reinterpret_cast<char*>(value("Text")), "initial");
    EXPECT_EQ(*value("Counter"), 1u);
    io.AddKeyEvent(ImGuiKey_Enter, false);
    frame();
}

TEST_F(CustomSceneParamsRenderTest, MappedReadOnlyFieldPreservesFocusedTextAcrossUnlock)
{
    ASSERT_TRUE(params.loadFromJsonString(R"({"SceneParams": {
        "Locked": {"type":"bool", "hidden":true},
        "Text": {"type":"string", "length":32, "value":"initial", "readOnlyField":"Locked"}
    }})"));
    const Item item = click(frame());
    ASSERT_EQ(ImGui::GetCurrentContext()->ActiveId, item.id);
    auto* locked = value("Locked");
    auto* text = reinterpret_cast<char*>(value("Text"));
    ASSERT_NE(locked, nullptr);
    ASSERT_NE(text, nullptr);

    auto& io = ImGui::GetIO();
    const ImGuiKey shortcut_modifier = io.ConfigMacOSXBehaviors ? ImGuiMod_Super : ImGuiMod_Ctrl;
    io.AddKeyEvent(shortcut_modifier, true);
    io.AddKeyEvent(ImGuiKey_A, true);
    frame();
    io.AddKeyEvent(ImGuiKey_A, false);
    io.AddKeyEvent(shortcut_modifier, false);
    frame();
    io.AddInputCharactersUTF8("draft");
    frame();
    ASSERT_STREQ(text, "draft");

    *locked = PNANOVDB_TRUE;
    std::strcpy(text, "mapped application value");
    frame();
    ASSERT_STREQ(text, "mapped application value");

    *locked = PNANOVDB_FALSE;
    frame();
    EXPECT_STREQ(text, "mapped application value");
}

TEST_F(CustomSceneParamsRenderTest, MappedReadOnlyFieldPreservesTextFocusedWhileLocked)
{
    ASSERT_TRUE(params.loadFromJsonString(R"({"SceneParams": {
        "Locked": {"type":"bool", "value":true, "hidden":true},
        "Text": {"type":"string", "length":32, "value":"initial", "readOnlyField":"Locked"}
    }})"));
    const Item item = click(frame());
    ASSERT_EQ(ImGui::GetCurrentContext()->ActiveId, item.id);

    auto* locked = value("Locked");
    auto* text = reinterpret_cast<char*>(value("Text"));
    ASSERT_NE(locked, nullptr);
    ASSERT_NE(text, nullptr);
    std::strcpy(text, "mapped while locked");
    frame();
    ASSERT_STREQ(text, "mapped while locked");

    *locked = PNANOVDB_FALSE;
    frame();
    EXPECT_STREQ(text, "mapped while locked");
}

TEST_F(CustomSceneParamsRenderTest, MappedReadOnlyFieldPreservesFocusedNumericTextAcrossUnlock)
{
    ASSERT_TRUE(params.loadFromJsonString(R"({"SceneParams": {
        "Locked": {"type":"bool", "hidden":true},
        "Value": {"type":"uint", "value":12, "readOnlyField":"Locked"}
    }})"));
    auto& io = ImGui::GetIO();
    const ImGuiKey shortcut_modifier = io.ConfigMacOSXBehaviors ? ImGuiMod_Super : ImGuiMod_Ctrl;
    const Item item = frame();
    const ImVec2 center = item.rect.GetCenter();
    io.AddMousePosEvent(center.x, center.y);
    frame();
    io.AddKeyEvent(shortcut_modifier, true);
    frame();
    io.AddMouseButtonEvent(0, true);
    frame();
    ASSERT_NE(ImGui::GetCurrentContext()->TempInputId, 0u);
    ASSERT_EQ(ImGui::GetCurrentContext()->TempInputId, ImGui::GetCurrentContext()->ActiveId);
    io.AddMouseButtonEvent(0, false);
    io.AddKeyEvent(shortcut_modifier, false);
    io.AddMousePosEvent(-100, -100);
    frame();
    ASSERT_NE(ImGui::GetCurrentContext()->TempInputId, 0u);
    ASSERT_EQ(ImGui::GetCurrentContext()->TempInputId, ImGui::GetCurrentContext()->ActiveId);

    auto* locked = value("Locked");
    auto* number = reinterpret_cast<pnanovdb_uint32_t*>(value("Value"));
    ASSERT_NE(locked, nullptr);
    ASSERT_NE(number, nullptr);
    ASSERT_EQ(*number, 12u);

    *locked = PNANOVDB_TRUE;
    *number = 42u;
    frame();
    ASSERT_EQ(*number, 42u);

    *locked = PNANOVDB_FALSE;
    frame();
    EXPECT_EQ(*number, 42u);
}

TEST_F(CustomSceneParamsRenderTest, StaticReadOnlyKeepsControlLockedWhenGateIsFalse)
{
    ASSERT_TRUE(params.loadFromJsonString(R"({"SceneParams": {
        "Locked": {"type":"bool", "hidden":true},
        "Play": {"type":"bool", "widget":"toggleButton", "readOnly":true, "readOnlyField":"Locked"}
    }})"));
    click(frame());
    EXPECT_EQ(*value("Play"), PNANOVDB_FALSE);
}

} // namespace
