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

namespace pnanovdb_editor
{

void CustomSceneParams::render()
{
    std::lock_guard<std::mutex> lock(m_data_mutex);
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

        if (field.is_string)
        {
            const std::string label = field.name + "##custom";
            char* committed = reinterpret_cast<char*>(m_data.data() + field.offset);
            const ImGuiInputTextFlags flags =
                field.commit_on_enter ? ImGuiInputTextFlags_EnterReturnsTrue : ImGuiInputTextFlags_None;
            const bool entered = ImGui::InputText(label.c_str(), committed, field.element_count, flags);
            if (entered && field.commit_on_enter && !field.submit_counter_field.empty())
            {
                // bump the sibling uint32 counter field so clients that poll it observe a change
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
            continue;
        }

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
        renderParamWidget(ui_spec);
    }
}

} // namespace pnanovdb_editor
