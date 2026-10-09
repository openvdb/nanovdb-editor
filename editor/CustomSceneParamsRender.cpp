// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   editor/CustomSceneParamsRender.cpp

    \author Petra Hapalova

    \brief
*/

#include "CustomSceneParams.h"
#include "ParamWidget.h"

#include <cstring>
#include <imgui.h>
#include <imgui_internal.h>

namespace pnanovdb_editor
{

void CustomSceneParams::render()
{
    std::lock_guard<std::mutex> lock(m_data_mutex);
    bool rendered_any = false;
    for (auto& field : m_fields)
    {
        if (field.is_hidden)
        {
            continue;
        }
        if (field.offset + field.element_size * field.element_count > m_data.size())
        {
            continue;
        }

        bool read_only = field.is_read_only;
        if (field.read_only_field_index < m_fields.size())
        {
            const auto& gate = m_fields[field.read_only_field_index];
            read_only = read_only ||
                        *reinterpret_cast<const pnanovdb_bool_t*>(m_data.data() + gate.offset) != PNANOVDB_FALSE;
        }
        const bool numeric = !field.is_string && !field.is_bool && !field.is_native_bool;
        const bool fit_width = field.same_line && rendered_any && field.widget == Widget::Default &&
                               (field.is_string || numeric);
        if (field.same_line && rendered_any)
        {
            ImGui::SameLine();
        }
        if (fit_width)
        {
            const float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
            const float label_width = ImGui::CalcTextSize(field.name.c_str(), nullptr, true).x + spacing;
            const size_t components = field.is_string ? 1 : (field.element_count == 16 ? 4 : field.element_count);
            const float min_width = ImGui::GetFrameHeight() * components + spacing * (components - 1);
            if (ImGui::GetContentRegionAvail().x < label_width + min_width)
            {
                ImGui::NewLine();
            }
            ImGui::PushItemWidth(ImMax(1.f, ImMin(ImGui::CalcItemWidth(), ImGui::GetContentRegionAvail().x - label_width)));
        }
        ImGui::PushStyleVar(ImGuiStyleVar_DisabledAlpha, numeric ? 1.f : ImGui::GetStyle().DisabledAlpha);
        ImGui::BeginDisabled(read_only && !field.is_string);
        if (read_only)
        {
            ImGui::PushItemFlag(ImGuiItemFlags_ReadOnly, true);
        }
        if (field.widget != Widget::Default)
        {
            auto* value = reinterpret_cast<pnanovdb_bool_t*>(m_data.data() + field.offset);
            const bool active = *value != PNANOVDB_FALSE;
            const bool toggle = field.widget == Widget::ToggleButton;
            const bool highlighted = toggle && active;
            if (highlighted)
            {
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            }
            ImGui::PushID(field.name.c_str());
            const std::string label = (highlighted ? field.active_label : field.name) + "###custom";
            if (ImGui::Button(label.c_str()))
            {
                *value = toggle && active ? PNANOVDB_FALSE : PNANOVDB_TRUE;
            }
            ImGui::PopID();
            if (highlighted)
            {
                ImGui::PopStyleColor();
            }
        }
        else if (field.is_string)
        {
            const std::string label = field.name + "##custom";
            const ImGuiID id = ImGui::GetID(label.c_str());
            auto& context = *ImGui::GetCurrentContext();
            const auto& input_state = context.InputTextState;
            const bool was_read_only = (input_state.Flags & ImGuiInputTextFlags_ReadOnly) != 0;
            if (context.ActiveId == id && input_state.ID == id && was_read_only != read_only)
            {
                // Discard the edit buffer when access changes. Mapped values remain authoritative.
                ImGui::ClearActiveID();
                context.InputTextDeactivatedState.ID = 0;
            }
            char* committed = reinterpret_cast<char*>(m_data.data() + field.offset);
            ImGuiInputTextFlags flags =
                field.commit_on_enter ? ImGuiInputTextFlags_EnterReturnsTrue : ImGuiInputTextFlags_None;
            if (read_only)
            {
                flags |= ImGuiInputTextFlags_ReadOnly;
            }
            const bool entered = ImGui::InputText(label.c_str(), committed, field.element_count, flags);
            if (entered && !read_only && field.commit_on_enter && !field.submit_counter_field.empty())
            {
                // Publish Enter presses through the sibling counter.
                for (auto& counter_field : m_fields)
                {
                    if (counter_field.name != field.submit_counter_field)
                    {
                        continue;
                    }
                    if (counter_field.reflect_type != PNANOVDB_REFLECT_TYPE_UINT32 || counter_field.element_count != 1 ||
                        counter_field.offset + sizeof(pnanovdb_uint32_t) > m_data.size())
                    {
                        break;
                    }
                    auto* counter = reinterpret_cast<pnanovdb_uint32_t*>(m_data.data() + counter_field.offset);
                    *counter = *counter + 1u;
                    break;
                }
            }
        }
        else
        {
            ParamWidgetSpec ui_spec;
            ui_spec.display_name = field.name;
            ui_spec.label = field.name + "##custom";
            ui_spec.type = field.imgui_type;
            ui_spec.value = m_data.data() + field.offset;
            ui_spec.element_size = field.element_size;
            ui_spec.element_count = field.element_count;
            ui_spec.min_value = field.min_value.empty() ? nullptr : field.min_value.data();
            ui_spec.max_value = field.max_value.empty() ? nullptr : field.max_value.data();
            ui_spec.step = field.step;
            ui_spec.is_slider = field.is_slider;
            ui_spec.is_bool = field.is_bool;
            ui_spec.is_hidden = field.is_hidden;
            ui_spec.is_native_bool = field.is_native_bool;
            ImGui::BeginGroup();
            renderParamWidget(ui_spec);
            ImGui::EndGroup();
        }
        if (read_only)
        {
            ImGui::PopItemFlag();
        }
        ImGui::EndDisabled();
        ImGui::PopStyleVar();
        if (fit_width)
        {
            ImGui::PopItemWidth();
        }
        rendered_any = true;
        if (!field.tooltip.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        {
            ImGui::SetTooltip("%s", field.tooltip.c_str());
        }
    }
}

} // namespace pnanovdb_editor
