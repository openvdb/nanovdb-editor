// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   editor/ParamWidget.cpp

    \author Petra Hapalova

    \brief  ImGui widgets and shared helpers for shader and pipeline parameters
*/

#include "ParamWidget.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstring>
#include <limits>
#include <type_traits>
#include <vector>

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

    return imgui_read_scalar(spec.type, ptr) != 0.0;
}

static void set_element_from_bool(const ParamWidgetSpec& spec, size_t index, bool value)
{
    char* ptr = static_cast<char*>(spec.value) + index * spec.element_size;
    if (spec.is_native_bool || spec.type == ImGuiDataType_Bool)
    {
        *reinterpret_cast<pnanovdb_bool_t*>(ptr) = value ? PNANOVDB_TRUE : PNANOVDB_FALSE;
        return;
    }

    imgui_write_scalar(spec.type, ptr, value ? 1.0 : 0.0);
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

    const bool is_half = spec.type == ImGuiDataType_Float && spec.element_size == sizeof(uint16_t);
    if (!is_half && (spec.is_native_bool || spec.type == ImGuiDataType_Bool || spec.is_bool))
    {
        render_bool_array(spec);
        return;
    }

    // Dear ImGui has no half-float scalar type. Edit through a temporary float
    // array and convert back so it never reads/writes four bytes through the
    // two-byte shader layout.
    if (is_half)
    {
        std::vector<float> values(spec.element_count);
        for (size_t i = 0; i < spec.element_count; ++i)
        {
            uint16_t bits = 0;
            std::memcpy(&bits, static_cast<const char*>(spec.value) + i * sizeof(bits), sizeof(bits));
            values[i] = half_bits_to_float(bits);
        }
        const std::vector<float> original_values = values;
        float min_value = 0.0f;
        float max_value = 0.0f;
        const void* min_ptr = nullptr;
        const void* max_ptr = nullptr;
        if (spec.min_value)
        {
            uint16_t bits = 0;
            std::memcpy(&bits, spec.min_value, sizeof(bits));
            min_value = half_bits_to_float(bits);
            min_ptr = &min_value;
        }
        if (spec.max_value)
        {
            uint16_t bits = 0;
            std::memcpy(&bits, spec.max_value, sizeof(bits));
            max_value = half_bits_to_float(bits);
            max_ptr = &max_value;
        }
        ParamWidgetSpec float_spec = spec;
        float_spec.value = values.data();
        float_spec.element_size = sizeof(float);
        float_spec.min_value = min_ptr;
        float_spec.max_value = max_ptr;
        renderParamWidget(float_spec);
        if (std::memcmp(values.data(), original_values.data(), values.size() * sizeof(float)) == 0)
        {
            return;
        }
        for (size_t i = 0; i < spec.element_count; ++i)
        {
            const uint16_t bits = float_to_half_bits(values[i]);
            std::memcpy(static_cast<char*>(spec.value) + i * sizeof(bits), &bits, sizeof(bits));
        }
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

void parseParamWidgetHints(const nlohmann::json& field, ParamWidgetHints& out)
{
    if (!field.is_object())
    {
        return;
    }
    if (field.contains("label") && field["label"].is_string())
    {
        out.label = field["label"].get<std::string>();
    }
    if (field.contains("tooltip") && field["tooltip"].is_string())
    {
        out.tooltip = field["tooltip"].get<std::string>();
    }
    if (field.contains("min") && field["min"].is_number())
    {
        out.has_min = true;
        out.min_value = field["min"].get<float>();
    }
    if (field.contains("max") && field["max"].is_number())
    {
        out.has_max = true;
        out.max_value = field["max"].get<float>();
    }
    if (field.contains("step") && field["step"].is_number())
    {
        out.has_step = true;
        out.step = field["step"].get<float>();
    }
    if (field.contains("isBool") && field["isBool"].is_boolean())
    {
        out.is_bool = field["isBool"].get<bool>();
    }
    if (field.contains("useSlider") && field["useSlider"].is_boolean())
    {
        out.use_slider = field["useSlider"].get<bool>();
    }
    if (field.contains("hidden") && field["hidden"].is_boolean())
    {
        out.hidden = field["hidden"].get<bool>();
    }
    if (field.contains("components") && field["components"].is_array())
    {
        out.components.clear();
        for (const auto& c : field["components"])
        {
            if (c.is_string())
            {
                out.components.push_back(c.get<std::string>());
            }
        }
    }
}

bool param_widget_uses_bool_control(ImGuiDataType type, const ParamWidgetHints* hints)
{
    return type == ImGuiDataType_Bool || (hints && hints->is_bool);
}

size_t reflect_scalar_field_size(pnanovdb_reflect_type_t rt)
{
    switch (rt)
    {
    case PNANOVDB_REFLECT_TYPE_FLOAT:
        return sizeof(float);
    case PNANOVDB_REFLECT_TYPE_DOUBLE:
        return sizeof(double);
    case PNANOVDB_REFLECT_TYPE_INT32:
    case PNANOVDB_REFLECT_TYPE_UINT32:
    case PNANOVDB_REFLECT_TYPE_BOOL32:
        return 4;
    case PNANOVDB_REFLECT_TYPE_INT64:
    case PNANOVDB_REFLECT_TYPE_UINT64:
        return 8;
    case PNANOVDB_REFLECT_TYPE_UINT8:
    case PNANOVDB_REFLECT_TYPE_CHAR:
        return 1;
    case PNANOVDB_REFLECT_TYPE_UINT16:
        return 2;
    default:
        return 0;
    }
}

bool reflect_type_to_imgui_type(pnanovdb_reflect_type_t rt, ImGuiDataType& out_type, size_t& out_size)
{
    switch (rt)
    {
    case PNANOVDB_REFLECT_TYPE_FLOAT:
        out_type = ImGuiDataType_Float;
        out_size = sizeof(float);
        return true;
    case PNANOVDB_REFLECT_TYPE_DOUBLE:
        out_type = ImGuiDataType_Double;
        out_size = sizeof(double);
        return true;
    case PNANOVDB_REFLECT_TYPE_INT32:
        out_type = ImGuiDataType_S32;
        out_size = sizeof(int32_t);
        return true;
    case PNANOVDB_REFLECT_TYPE_UINT32:
        out_type = ImGuiDataType_U32;
        out_size = sizeof(uint32_t);
        return true;
    case PNANOVDB_REFLECT_TYPE_INT64:
        out_type = ImGuiDataType_S64;
        out_size = sizeof(int64_t);
        return true;
    case PNANOVDB_REFLECT_TYPE_UINT64:
        out_type = ImGuiDataType_U64;
        out_size = sizeof(uint64_t);
        return true;
    case PNANOVDB_REFLECT_TYPE_BOOL32:
        out_type = ImGuiDataType_Bool;
        out_size = sizeof(uint32_t);
        return true;
    case PNANOVDB_REFLECT_TYPE_UINT8:
        out_type = ImGuiDataType_U8;
        out_size = sizeof(uint8_t);
        return true;
    case PNANOVDB_REFLECT_TYPE_UINT16:
        out_type = ImGuiDataType_U16;
        out_size = sizeof(uint16_t);
        return true;
    case PNANOVDB_REFLECT_TYPE_CHAR:
        out_type = ImGuiDataType_S8;
        out_size = sizeof(int8_t);
        return true;
    default:
        return false;
    }
}

double imgui_read_scalar(ImGuiDataType type, const void* src)
{
    switch (type)
    {
    case ImGuiDataType_Float:
        return (double)*static_cast<const float*>(src);
    case ImGuiDataType_Double:
        return *static_cast<const double*>(src);
    case ImGuiDataType_S32:
        return (double)*static_cast<const int32_t*>(src);
    case ImGuiDataType_U32:
        return (double)*static_cast<const uint32_t*>(src);
    case ImGuiDataType_S8:
        return (double)*static_cast<const int8_t*>(src);
    case ImGuiDataType_U8:
        return (double)*static_cast<const uint8_t*>(src);
    case ImGuiDataType_U16:
        return (double)*static_cast<const uint16_t*>(src);
    case ImGuiDataType_S64:
        return (double)*static_cast<const int64_t*>(src);
    case ImGuiDataType_U64:
        return (double)*static_cast<const uint64_t*>(src);
    case ImGuiDataType_Bool:
        return (double)(*static_cast<const uint32_t*>(src) != 0u);
    default:
        return 0.0;
    }
}

void imgui_write_scalar(ImGuiDataType type, void* dst, double v)
{
    switch (type)
    {
    case ImGuiDataType_Float:
        *static_cast<float*>(dst) = (float)v;
        break;
    case ImGuiDataType_Double:
        *static_cast<double*>(dst) = v;
        break;
    case ImGuiDataType_S32:
        *static_cast<int32_t*>(dst) = (int32_t)v;
        break;
    case ImGuiDataType_U32:
        *static_cast<uint32_t*>(dst) = (uint32_t)v;
        break;
    case ImGuiDataType_S8:
        *static_cast<int8_t*>(dst) = (int8_t)v;
        break;
    case ImGuiDataType_U8:
        *static_cast<uint8_t*>(dst) = (uint8_t)v;
        break;
    case ImGuiDataType_U16:
        *static_cast<uint16_t*>(dst) = (uint16_t)v;
        break;
    case ImGuiDataType_S64:
        *static_cast<int64_t*>(dst) = (int64_t)v;
        break;
    case ImGuiDataType_U64:
        *static_cast<uint64_t*>(dst) = (uint64_t)v;
        break;
    case ImGuiDataType_Bool:
        *static_cast<uint32_t*>(dst) = (v != 0.0) ? 1u : 0u;
        break;
    default:
        break;
    }
}

float half_bits_to_float(uint16_t bits)
{
    const uint32_t sign = static_cast<uint32_t>(bits & 0x8000u) << 16u;
    uint32_t exponent = (bits >> 10u) & 0x1fu;
    uint32_t mantissa = bits & 0x03ffu;
    uint32_t out = 0;
    if (exponent == 0u)
    {
        if (mantissa == 0u)
        {
            out = sign;
        }
        else
        {
            int shift = 0;
            while ((mantissa & 0x0400u) == 0u)
            {
                mantissa <<= 1u;
                ++shift;
            }
            mantissa &= 0x03ffu;
            out = sign | static_cast<uint32_t>(127 - 14 - shift) << 23u | mantissa << 13u;
        }
    }
    else if (exponent == 0x1fu)
    {
        out = sign | 0x7f800000u | mantissa << 13u;
    }
    else
    {
        out = sign | (exponent + (127u - 15u)) << 23u | mantissa << 13u;
    }
    float value = 0.0f;
    std::memcpy(&value, &out, sizeof(value));
    return value;
}

uint16_t float_to_half_bits(float value)
{
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const uint16_t sign = static_cast<uint16_t>((bits >> 16u) & 0x8000u);
    const uint32_t exponent = (bits >> 23u) & 0xffu;
    uint32_t mantissa = bits & 0x007fffffu;
    if (exponent == 0xffu)
    {
        return static_cast<uint16_t>(sign | 0x7c00u | (mantissa ? 0x0200u : 0u));
    }
    const int half_exponent = static_cast<int>(exponent) - 127 + 15;
    if (half_exponent >= 31)
    {
        return static_cast<uint16_t>(sign | 0x7c00u);
    }
    if (half_exponent <= 0)
    {
        if (half_exponent < -10)
        {
            return sign;
        }
        mantissa |= 0x00800000u;
        const int shift = 14 - half_exponent;
        uint32_t rounded = mantissa >> shift;
        const uint32_t remainder = mantissa & ((1u << shift) - 1u);
        const uint32_t halfway = 1u << (shift - 1);
        if (remainder > halfway || (remainder == halfway && (rounded & 1u)))
            ++rounded;
        return static_cast<uint16_t>(sign | rounded);
    }
    uint32_t rounded = mantissa >> 13u;
    const uint32_t remainder = mantissa & 0x1fffu;
    if (remainder > 0x1000u || (remainder == 0x1000u && (rounded & 1u)))
    {
        ++rounded;
        if (rounded == 0x0400u)
        {
            rounded = 0u;
            if (half_exponent + 1 >= 31)
                return static_cast<uint16_t>(sign | 0x7c00u);
            return static_cast<uint16_t>(sign | static_cast<uint16_t>(half_exponent + 1) << 10u);
        }
    }
    return static_cast<uint16_t>(sign | static_cast<uint16_t>(half_exponent) << 10u | static_cast<uint16_t>(rounded));
}

bool reflect_next_renderable_field(const pnanovdb_reflect_data_type_t* data_type,
                                   size_t blob_size,
                                   pnanovdb_uint64_t& index,
                                   ReflectRenderableField& out)
{
    if (!data_type)
    {
        return false;
    }
    for (; index < data_type->child_reflect_data_count; ++index)
    {
        const pnanovdb_reflect_data_t& f = data_type->child_reflect_datas[index];
        if (!f.data_type || f.reflect_mode != PNANOVDB_REFLECT_MODE_VALUE || !f.name)
        {
            continue;
        }
        ImGuiDataType imgui_type = ImGuiDataType_Float;
        size_t elem_size = 0;
        if (!reflect_type_to_imgui_type(f.data_type->data_type, imgui_type, elem_size))
        {
            continue;
        }
        if ((size_t)f.data_offset + elem_size > blob_size)
        {
            continue;
        }
        out.field = &f;
        out.imgui_type = imgui_type;
        out.elem_size = elem_size;
        ++index;
        return true;
    }
    return false;
}

bool reflect_read_scalar_json(pnanovdb_reflect_type_t rt, const unsigned char* p, nlohmann::ordered_json& out)
{
    switch (rt)
    {
    case PNANOVDB_REFLECT_TYPE_FLOAT:
    {
        float v;
        std::memcpy(&v, p, sizeof(v));
        out = v;
        return true;
    }
    case PNANOVDB_REFLECT_TYPE_DOUBLE:
    {
        double v;
        std::memcpy(&v, p, sizeof(v));
        out = v;
        return true;
    }
    case PNANOVDB_REFLECT_TYPE_INT32:
    {
        int32_t v;
        std::memcpy(&v, p, sizeof(v));
        out = v;
        return true;
    }
    case PNANOVDB_REFLECT_TYPE_UINT32:
    {
        uint32_t v;
        std::memcpy(&v, p, sizeof(v));
        out = v;
        return true;
    }
    case PNANOVDB_REFLECT_TYPE_INT64:
    {
        int64_t v;
        std::memcpy(&v, p, sizeof(v));
        out = v;
        return true;
    }
    case PNANOVDB_REFLECT_TYPE_UINT64:
    {
        uint64_t v;
        std::memcpy(&v, p, sizeof(v));
        out = v;
        return true;
    }
    case PNANOVDB_REFLECT_TYPE_BOOL32:
    {
        uint32_t v;
        std::memcpy(&v, p, sizeof(v));
        out = (v != 0u);
        return true;
    }
    case PNANOVDB_REFLECT_TYPE_UINT8:
    {
        out = (uint32_t)p[0];
        return true;
    }
    case PNANOVDB_REFLECT_TYPE_UINT16:
    {
        uint16_t v;
        std::memcpy(&v, p, sizeof(v));
        out = (uint32_t)v;
        return true;
    }
    case PNANOVDB_REFLECT_TYPE_CHAR:
    {
        out = (int32_t)(int8_t)p[0];
        return true;
    }
    default:
        return false;
    }
}

bool reflect_write_scalar_json(pnanovdb_reflect_type_t rt, unsigned char* p, const nlohmann::json& in)
{
    if (!p)
    {
        return false;
    }

    const auto read_float = [&in](auto& value)
    {
        using T = std::decay_t<decltype(value)>;
        if (!in.is_number())
        {
            return false;
        }
        const double decoded = in.get<double>();
        if (!std::isfinite(decoded) || decoded < -(double)std::numeric_limits<T>::max() ||
            decoded > (double)std::numeric_limits<T>::max())
        {
            return false;
        }
        value = static_cast<T>(decoded);
        return true;
    };
    const auto read_integer = [&in](auto& value)
    {
        using T = std::decay_t<decltype(value)>;
        if (in.is_number_unsigned())
        {
            const uint64_t decoded = in.get<uint64_t>();
            if (decoded > static_cast<uint64_t>(std::numeric_limits<T>::max()))
            {
                return false;
            }
            value = static_cast<T>(decoded);
            return true;
        }
        if (!in.is_number_integer())
        {
            return false;
        }
        const int64_t decoded = in.get<int64_t>();
        if constexpr (std::is_unsigned_v<T>)
        {
            if (decoded < 0 || static_cast<uint64_t>(decoded) > static_cast<uint64_t>(std::numeric_limits<T>::max()))
            {
                return false;
            }
        }
        else if (decoded < static_cast<int64_t>(std::numeric_limits<T>::min()) ||
                 decoded > static_cast<int64_t>(std::numeric_limits<T>::max()))
        {
            return false;
        }
        value = static_cast<T>(decoded);
        return true;
    };

    switch (rt)
    {
    case PNANOVDB_REFLECT_TYPE_FLOAT:
    {
        float v;
        if (!read_float(v))
            return false;
        std::memcpy(p, &v, sizeof(v));
        return true;
    }
    case PNANOVDB_REFLECT_TYPE_DOUBLE:
    {
        double v;
        if (!read_float(v))
            return false;
        std::memcpy(p, &v, sizeof(v));
        return true;
    }
    case PNANOVDB_REFLECT_TYPE_INT32:
    {
        int32_t v;
        if (!read_integer(v))
            return false;
        std::memcpy(p, &v, sizeof(v));
        return true;
    }
    case PNANOVDB_REFLECT_TYPE_UINT32:
    {
        uint32_t v;
        if (!read_integer(v))
            return false;
        std::memcpy(p, &v, sizeof(v));
        return true;
    }
    case PNANOVDB_REFLECT_TYPE_INT64:
    {
        int64_t v;
        if (!read_integer(v))
            return false;
        std::memcpy(p, &v, sizeof(v));
        return true;
    }
    case PNANOVDB_REFLECT_TYPE_UINT64:
    {
        uint64_t v;
        if (!read_integer(v))
            return false;
        std::memcpy(p, &v, sizeof(v));
        return true;
    }
    case PNANOVDB_REFLECT_TYPE_BOOL32:
    {
        uint32_t v = 0u;
        if (in.is_boolean())
        {
            v = in.get<bool>() ? 1u : 0u;
        }
        else if (in.is_number_unsigned())
        {
            v = in.get<uint64_t>() != 0 ? 1u : 0u;
        }
        else if (in.is_number_integer())
        {
            v = in.get<int64_t>() != 0 ? 1u : 0u;
        }
        else
            return false;
        std::memcpy(p, &v, sizeof(v));
        return true;
    }
    case PNANOVDB_REFLECT_TYPE_UINT8:
    {
        uint8_t v;
        if (!read_integer(v))
            return false;
        p[0] = v;
        return true;
    }
    case PNANOVDB_REFLECT_TYPE_UINT16:
    {
        uint16_t v;
        if (!read_integer(v))
            return false;
        std::memcpy(p, &v, sizeof(v));
        return true;
    }
    case PNANOVDB_REFLECT_TYPE_CHAR:
    {
        int8_t v;
        if (!read_integer(v))
            return false;
        std::memcpy(p, &v, sizeof(v));
        return true;
    }
    default:
        return false;
    }
}

} // namespace pnanovdb_editor
