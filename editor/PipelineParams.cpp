// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   editor/PipelineParams.cpp

    \author Petra Hapalova

    \brief
*/

#include "PipelineParams.h"
#include "ParamWidget.h"
#include "ShaderParams.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace pnanovdb_editor
{
namespace
{

bool next_visible_field(const pnanovdb_reflect_data_type_t* data_type,
                        size_t size,
                        pnanovdb_uint64_t& field_index,
                        const std::map<std::string, ParamWidgetHints>& hints,
                        ReflectRenderableField& field,
                        const ParamWidgetHints*& hint)
{
    while (reflect_next_renderable_field(data_type, size, field_index, field))
    {
        const auto hint_it = hints.find(field.field->name);
        hint = (hint_it != hints.end()) ? &hint_it->second : nullptr;
        if (!hint || !hint->hidden)
        {
            return true;
        }
    }
    return false;
}

} // namespace

const std::map<std::string, ParamWidgetHints>& PipelineParams::hints_for(const char* hints_name)
{
    static const std::map<std::string, ParamWidgetHints> empty;
    if (!hints_name || !hints_name[0])
    {
        return empty;
    }
    const std::string key(hints_name);
    auto it = hints_cache_.find(key);
    if (it != hints_cache_.end())
    {
        return it->second;
    }

    std::map<std::string, ParamWidgetHints> hints;
    const std::optional<nlohmann::ordered_json> section = loadParamHintsJson(key, PIPELINE_PARAM_JSON);
    if (section && section->is_object())
    {
        for (auto& [name, value] : section->items())
        {
            ParamWidgetHints hint;
            parseParamWidgetHints(value, hint);
            hints.emplace(name, std::move(hint));
        }
    }
    return hints_cache_.emplace(key, std::move(hints)).first->second;
}

PipelineParams::EditResult PipelineParams::render(const pnanovdb_reflect_data_type_t* data_type,
                                                  const char* hints_name,
                                                  unsigned char* data,
                                                  size_t size,
                                                  const char* id_suffix)
{
    EditResult result;
    if (!data_type || !data || size == 0)
    {
        return result;
    }

    const std::map<std::string, ParamWidgetHints>& hints = hints_for(hints_name);
    const std::string id = (id_suffix && id_suffix[0]) ? std::string(id_suffix) : std::string("pipeline_params");

    pnanovdb_uint64_t field_index = 0;
    ReflectRenderableField field;
    const ParamWidgetHints* hint = nullptr;
    while (next_visible_field(data_type, size, field_index, hints, field, hint))
    {
        const pnanovdb_reflect_data_t& f = *field.field;
        void* value_ptr = data + f.data_offset;

        const bool treat_as_bool = param_widget_uses_bool_control(field.imgui_type, hint);

        std::string label = (hint && !hint->label.empty()) ? hint->label : f.name;

        std::vector<char> min_buf(field.elem_size, 0);
        std::vector<char> max_buf(field.elem_size, 0);
        const void* min_ptr = nullptr;
        const void* max_ptr = nullptr;
        if (hint && (hint->has_min || hint->has_max))
        {
            if (hint->has_min)
            {
                imgui_write_scalar(field.imgui_type, min_buf.data(), (double)hint->min_value);
                min_ptr = min_buf.data();
            }
            if (hint->has_max)
            {
                imgui_write_scalar(field.imgui_type, max_buf.data(), (double)hint->max_value);
                max_ptr = max_buf.data();
            }
        }

        const float step = (hint && hint->has_step) ? hint->step : 0.01f;
        const bool use_slider = hint && hint->use_slider && min_ptr && max_ptr;

        ImGui::PushID(id.c_str());
        ImGui::PushID(f.name);

        ParamWidgetSpec spec;
        spec.display_name = label;
        spec.label = label;
        spec.type = field.imgui_type;
        spec.value = value_ptr;
        spec.element_size = field.elem_size;
        spec.element_count = 1;
        spec.min_value = min_ptr;
        spec.max_value = max_ptr;
        spec.step = step;
        spec.is_slider = use_slider;
        spec.is_bool = treat_as_bool;
        spec.is_native_bool = false;
        renderParamWidget(spec);

        if (hint && !hint->tooltip.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        {
            ImGui::SetTooltip("%s", hint->tooltip.c_str());
        }

        result.any_edited |= ImGui::IsItemEdited();
        result.any_active |= ImGui::IsItemActive();
        result.any_committed |= ImGui::IsItemDeactivatedAfterEdit();

        ImGui::PopID();
        ImGui::PopID();
    }

    return result;
}

bool PipelineParams::primary_field(const pnanovdb_reflect_data_type_t* data_type,
                                   const char* hints_name,
                                   const unsigned char* data,
                                   size_t size,
                                   std::string& out_label,
                                   double& out_value)
{
    if (!data_type || !data || size == 0)
    {
        return false;
    }

    const std::map<std::string, ParamWidgetHints>& hints = hints_for(hints_name);
    pnanovdb_uint64_t field_index = 0;
    ReflectRenderableField field;
    const ParamWidgetHints* hint = nullptr;
    if (!next_visible_field(data_type, size, field_index, hints, field, hint))
    {
        return false;
    }

    const pnanovdb_reflect_data_t& f = *field.field;
    out_label = (hint && !hint->label.empty()) ? hint->label : f.name;
    out_value = imgui_read_scalar(field.imgui_type, data + f.data_offset);
    return true;
}

void PipelineParams::clear_cache()
{
    hints_cache_.clear();
}

} // namespace pnanovdb_editor
