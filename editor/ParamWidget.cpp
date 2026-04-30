// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   editor/ParamWidget.cpp

    \author Petra Hapalova

    \brief  Helper functions for rendering shader and custom scene parameters in the UI
*/

#include "ParamWidget.h"

namespace pnanovdb_editor
{
namespace
{

static bool get_element_as_bool(const ParamWidgetSpec& spec, size_t index)
{
    const char* ptr = static_cast<const char*>(spec.value) + index * spec.element_size;
    if (spec.is_native_bool || spec.type == ImGuiDataType_Bool)
    {
        return *reinterpret_cast<const pnanovdb_bool_t*>(ptr) != PNANOVDB_FALSE;
    }

    switch (spec.type)
    {
    case ImGuiDataType_S32:
        return *reinterpret_cast<const pnanovdb_int32_t*>(ptr) != 0;
    case ImGuiDataType_U32:
        return *reinterpret_cast<const pnanovdb_uint32_t*>(ptr) != 0u;
    case ImGuiDataType_S64:
        return *reinterpret_cast<const int64_t*>(ptr) != 0;
    case ImGuiDataType_U64:
        return *reinterpret_cast<const uint64_t*>(ptr) != 0u;
    case ImGuiDataType_Double:
        return *reinterpret_cast<const double*>(ptr) != 0.0;
    case ImGuiDataType_Float:
    default:
        return *reinterpret_cast<const float*>(ptr) != 0.0f;
    }
}

static void set_element_from_bool(const ParamWidgetSpec& spec, size_t index, bool value)
{
    char* ptr = static_cast<char*>(spec.value) + index * spec.element_size;
    if (spec.is_native_bool || spec.type == ImGuiDataType_Bool)
    {
        *reinterpret_cast<pnanovdb_bool_t*>(ptr) = value ? PNANOVDB_TRUE : PNANOVDB_FALSE;
        return;
    }

    switch (spec.type)
    {
    case ImGuiDataType_S32:
        *reinterpret_cast<pnanovdb_int32_t*>(ptr) = value ? 1 : 0;
        break;
    case ImGuiDataType_U32:
        *reinterpret_cast<pnanovdb_uint32_t*>(ptr) = value ? 1u : 0u;
        break;
    case ImGuiDataType_S64:
        *reinterpret_cast<int64_t*>(ptr) = value ? 1 : 0;
        break;
    case ImGuiDataType_U64:
        *reinterpret_cast<uint64_t*>(ptr) = value ? 1u : 0u;
        break;
    case ImGuiDataType_Double:
        *reinterpret_cast<double*>(ptr) = value ? 1.0 : 0.0;
        break;
    case ImGuiDataType_Float:
    default:
        *reinterpret_cast<float*>(ptr) = value ? 1.0f : 0.0f;
        break;
    }
}

static void render_bool_array(const ParamWidgetSpec& spec)
{
    if (spec.element_count == 1)
    {
        bool value = get_element_as_bool(spec, 0);
        if (ImGui::Checkbox(spec.label.c_str(), &value))
        {
            set_element_from_bool(spec, 0, value);
        }
        return;
    }

    ImGui::Text("%s", spec.display_name.c_str());
    ImGui::PushID(spec.label.c_str());
    for (size_t i = 0; i < spec.element_count; ++i)
    {
        bool value = get_element_as_bool(spec, i);
        const std::string element_label = "[" + std::to_string(i) + "]";
        if (ImGui::Checkbox(element_label.c_str(), &value))
        {
            set_element_from_bool(spec, i, value);
        }
        if (i + 1 < spec.element_count)
        {
            ImGui::SameLine();
        }
    }
    ImGui::PopID();
}

} // namespace

void renderParamWidget(const ParamWidgetSpec& spec)
{
    if (spec.is_hidden || !spec.value || spec.element_count == 0 || spec.element_size == 0)
    {
        return;
    }

    if (spec.is_native_bool || spec.type == ImGuiDataType_Bool || spec.is_bool)
    {
        render_bool_array(spec);
        return;
    }

    if (spec.element_count == 16)
    {
        ImGui::Text("%s", spec.display_name.c_str());
        ImGui::PushID(spec.label.c_str());
        const char* row_names[] = { "x", "y", "z", "w" };
        for (size_t row = 0; row < 4; ++row)
        {
            void* row_ptr = static_cast<char*>(spec.value) + row * 4 * spec.element_size;
            ImGui::DragScalarN(
                row_names[row], spec.type, row_ptr, 4, spec.step, spec.min_value, spec.max_value, nullptr, 0);
        }
        ImGui::PopID();
        return;
    }

    if (spec.is_slider && spec.min_value && spec.max_value)
    {
        const char* format = (spec.type == ImGuiDataType_Float || spec.type == ImGuiDataType_Double) ? "%.3f" : nullptr;
        ImGui::SliderScalarN(spec.label.c_str(), spec.type, spec.value, static_cast<int>(spec.element_count),
                             spec.min_value, spec.max_value, format, ImGuiSliderFlags_AlwaysClamp);
        return;
    }

    ImGui::DragScalarN(spec.label.c_str(), spec.type, spec.value, static_cast<int>(spec.element_count), spec.step,
                       spec.min_value, spec.max_value);
}

} // namespace pnanovdb_editor
