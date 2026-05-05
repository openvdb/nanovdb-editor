// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   editor/CustomSceneParams.cpp

    \author Petra Hapalova

    \brief
*/

#include "CustomSceneParams.h"

#include "nanovdb_editor/putil/Shader.hpp"

#include <cstring>
#include <fstream>
#include <filesystem>
#include <unordered_map>

namespace pnanovdb_editor
{
namespace
{

struct ParsedType
{
    ImGuiDataType imgui_type = ImGuiDataType_Float;
    pnanovdb_uint32_t reflect_type = PNANOVDB_REFLECT_TYPE_FLOAT;
    size_t element_size = sizeof(float);
    bool is_native_bool = false;
};

static constexpr const char* kSceneParamsJsonKey = "SceneParams";

static nlohmann::ordered_json* get_scene_params_object(nlohmann::ordered_json& json)
{
    if (json.contains(kSceneParamsJsonKey))
    {
        return &json[kSceneParamsJsonKey];
    }
    return nullptr;
}

static size_t align_up(size_t value, size_t alignment)
{
    if (alignment == 0)
    {
        return value;
    }
    const size_t mask = alignment - 1;
    return (value + mask) & ~mask;
}

static bool try_parse_type_name(const std::string& type_name, ParsedType* out_type)
{
    static const std::unordered_map<std::string, ParsedType> kTypeMap = {
        { "bool", { ImGuiDataType_S32, PNANOVDB_REFLECT_TYPE_BOOL32, sizeof(pnanovdb_bool_t), true } },
        { "bool32", { ImGuiDataType_S32, PNANOVDB_REFLECT_TYPE_BOOL32, sizeof(pnanovdb_bool_t), true } },
        { "int", { ImGuiDataType_S32, PNANOVDB_REFLECT_TYPE_INT32, sizeof(pnanovdb_int32_t), false } },
        { "int32", { ImGuiDataType_S32, PNANOVDB_REFLECT_TYPE_INT32, sizeof(pnanovdb_int32_t), false } },
        { "uint", { ImGuiDataType_U32, PNANOVDB_REFLECT_TYPE_UINT32, sizeof(pnanovdb_uint32_t), false } },
        { "uint32", { ImGuiDataType_U32, PNANOVDB_REFLECT_TYPE_UINT32, sizeof(pnanovdb_uint32_t), false } },
        { "int64", { ImGuiDataType_S64, PNANOVDB_REFLECT_TYPE_INT64, sizeof(int64_t), false } },
        { "uint64", { ImGuiDataType_U64, PNANOVDB_REFLECT_TYPE_UINT64, sizeof(uint64_t), false } },
        { "double", { ImGuiDataType_Double, PNANOVDB_REFLECT_TYPE_DOUBLE, sizeof(double), false } },
        { "float", { ImGuiDataType_Float, PNANOVDB_REFLECT_TYPE_FLOAT, sizeof(float), false } },
    };

    auto it = kTypeMap.find(type_name);
    if (it != kTypeMap.end())
    {
        if (out_type)
        {
            *out_type = it->second;
        }
        return true;
    }
    return false;
}

static const pnanovdb_reflect_data_type_t* get_builtin_reflect_data_type(const CustomSceneParams::Field& field)
{
    if (field.element_count != 1)
    {
        return nullptr;
    }

    switch (field.reflect_type)
    {
    case PNANOVDB_REFLECT_TYPE_BOOL32:
        return PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_bool_t);
    case PNANOVDB_REFLECT_TYPE_INT32:
        return PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_int32_t);
    case PNANOVDB_REFLECT_TYPE_UINT32:
        return PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_uint32_t);
    case PNANOVDB_REFLECT_TYPE_INT64:
        return PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_int64_t);
    case PNANOVDB_REFLECT_TYPE_UINT64:
        return PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_uint64_t);
    case PNANOVDB_REFLECT_TYPE_DOUBLE:
        return PNANOVDB_REFLECT_DATA_TYPE(double);
    case PNANOVDB_REFLECT_TYPE_FLOAT:
        return PNANOVDB_REFLECT_DATA_TYPE(float);
    default:
        return nullptr;
    }
}

template <typename T>
static void write_numeric_scalar(void* dst, const nlohmann::json& value, T default_value = T(0))
{
    T resolved = default_value;
    if (value.is_number())
    {
        resolved = value.get<T>();
    }
    else if (value.is_boolean())
    {
        resolved = value.get<bool>() ? T(1) : T(0);
    }
    *reinterpret_cast<T*>(dst) = resolved;
}

static void write_typed_scalar(ImGuiDataType imgui_type, void* dst, const nlohmann::json& value, bool is_native_bool = false)
{
    if (is_native_bool)
    {
        pnanovdb_bool_t resolved = PNANOVDB_FALSE;
        if (value.is_boolean())
        {
            resolved = value.get<bool>() ? PNANOVDB_TRUE : PNANOVDB_FALSE;
        }
        else if (value.is_number_integer() || value.is_number_unsigned())
        {
            resolved = value.get<int>() != 0 ? PNANOVDB_TRUE : PNANOVDB_FALSE;
        }
        *reinterpret_cast<pnanovdb_bool_t*>(dst) = resolved;
        return;
    }

    switch (imgui_type)
    {
    case ImGuiDataType_S32:
        write_numeric_scalar<pnanovdb_int32_t>(dst, value);
        break;
    case ImGuiDataType_U32:
        write_numeric_scalar<pnanovdb_uint32_t>(dst, value);
        break;
    case ImGuiDataType_S64:
        write_numeric_scalar<int64_t>(dst, value);
        break;
    case ImGuiDataType_U64:
        write_numeric_scalar<uint64_t>(dst, value);
        break;
    case ImGuiDataType_Double:
        write_numeric_scalar<double>(dst, value);
        break;
    case ImGuiDataType_Float:
    default:
        write_numeric_scalar<float>(dst, value);
        break;
    }
}

static void write_typed_array(ImGuiDataType imgui_type,
                              void* dst,
                              size_t element_size,
                              size_t element_count,
                              const nlohmann::json& value,
                              bool is_native_bool = false)
{
    for (size_t i = 0; i < element_count; ++i)
    {
        const nlohmann::json* element_value = nullptr;
        if (value.is_array() && i < value.size())
        {
            element_value = &value[i];
        }
        else if (!value.is_array() && i == 0)
        {
            element_value = &value;
        }

        write_typed_scalar(imgui_type, static_cast<char*>(dst) + i * element_size,
                           element_value ? *element_value : nlohmann::json(), is_native_bool);
    }
}

static std::vector<char> make_bound_from_json(const CustomSceneParams::Field& field, const nlohmann::json& value)
{
    std::vector<char> bytes(field.element_size * field.element_count, 0);
    write_typed_array(
        field.imgui_type, bytes.data(), field.element_size, field.element_count, value, field.is_native_bool);
    return bytes;
}

static std::string make_display_type_name(const std::string& base_name, size_t element_count)
{
    if (element_count <= 1)
    {
        return base_name;
    }
    return base_name + "[" + std::to_string(element_count) + "]";
}

} // namespace

bool CustomSceneParams::loadFromJsonString(const std::string& json_string,
                                           const std::string& source_name,
                                           std::string* error_message)
{
    std::lock_guard<std::mutex> lock(m_data_mutex);
    m_source_label.clear();
    m_fields.clear();
    m_data.clear();
    m_element_names.clear();
    m_element_type_names.clear();
    m_element_offsets.clear();

    nlohmann::ordered_json json;
    try
    {
        json = nlohmann::ordered_json::parse(json_string);
    }
    catch (const nlohmann::json::parse_error& e)
    {
        if (error_message)
        {
            *error_message = e.what();
        }
        return false;
    }

    nlohmann::ordered_json* scene_params = get_scene_params_object(json);
    if (!scene_params || !scene_params->is_object())
    {
        if (error_message)
        {
            *error_message = "JSON string must contain an object-valued 'SceneParams' entry";
        }
        return false;
    }

    std::vector<const nlohmann::ordered_json*> field_value_jsons;
    field_value_jsons.reserve(scene_params->size());

    constexpr size_t kMaxStringLength = 64 * 1024;
    constexpr size_t kDefaultStringLength = 256;

    size_t next_offset = 0;
    for (auto& [field_name, field_json] : scene_params->items())
    {
        if (!field_json.is_object())
        {
            if (error_message)
            {
                *error_message = "field '" + field_name + "' must be an object";
            }
            return false;
        }
        if (!field_json.contains("type") || !field_json["type"].is_string())
        {
            if (error_message)
            {
                *error_message = "field '" + field_name + "' is missing string 'type'";
            }
            return false;
        }

        const std::string parsed_type_name = field_json["type"].get<std::string>();

        Field field;
        field.name = field_name;
        field.is_hidden = field_json.value("hidden", false);

        if (parsed_type_name == "string")
        {
            if (field_json.contains("min") || field_json.contains("max") || field_json.contains("useSlider") ||
                field_json.contains("isBool") || field_json.contains("elementCount"))
            {
                if (error_message)
                {
                    *error_message = "field '" + field_name +
                                     "' is a string; 'min'/'max'/'useSlider'/'isBool'/'elementCount' are not supported";
                }
                return false;
            }

            bool commit_on_enter = false;
            if (field_json.contains("commitOnEnter"))
            {
                const auto& flag_json = field_json["commitOnEnter"];
                if (!flag_json.is_boolean())
                {
                    if (error_message)
                    {
                        *error_message = "field '" + field_name + "' has invalid 'commitOnEnter'; expected bool";
                    }
                    return false;
                }
                commit_on_enter = flag_json.get<bool>();
            }
            std::string submit_counter_field;
            if (field_json.contains("submitCounterField"))
            {
                const auto& counter_json = field_json["submitCounterField"];
                if (!counter_json.is_string())
                {
                    if (error_message)
                    {
                        *error_message = "field '" + field_name + "' has invalid 'submitCounterField'; expected string";
                    }
                    return false;
                }
                submit_counter_field = counter_json.get<std::string>();
            }

            size_t length = kDefaultStringLength;
            if (field_json.contains("length"))
            {
                const auto& length_json = field_json["length"];
                if (!length_json.is_number_unsigned() &&
                    !(length_json.is_number_integer() && length_json.get<int64_t>() > 0))
                {
                    if (error_message)
                    {
                        *error_message = "field '" + field_name + "' has invalid 'length'; expected positive integer";
                    }
                    return false;
                }
                length = static_cast<size_t>(length_json.get<pnanovdb_uint64_t>());
            }
            if (length < 1 || length > kMaxStringLength)
            {
                if (error_message)
                {
                    *error_message = "field '" + field_name + "' has out-of-range 'length' (must be 1.." +
                                     std::to_string(kMaxStringLength) + ")";
                }
                return false;
            }
            if (field_json.contains("value") && !field_json["value"].is_string())
            {
                if (error_message)
                {
                    *error_message = "field '" + field_name + "' string 'value' must be a string";
                }
                return false;
            }

            field.is_string = true;
            field.imgui_type = ImGuiDataType_S8;
            field.reflect_type = PNANOVDB_REFLECT_TYPE_CHAR;
            field.element_size = 1;
            field.element_count = length;
            field.is_native_bool = false;
            field.type_name = "char[" + std::to_string(length) + "]";
            field.step = 0.0f;
            field.is_slider = false;
            field.is_bool = false;
            field.commit_on_enter = commit_on_enter;
            field.submit_counter_field = std::move(submit_counter_field);
            field.offset = next_offset;
            next_offset = field.offset + length;

            field_value_jsons.push_back(field_json.contains("value") ? &field_json["value"] : nullptr);
            m_fields.push_back(std::move(field));
            continue;
        }

        ParsedType parsed_type;
        if (!try_parse_type_name(parsed_type_name, &parsed_type))
        {
            if (error_message)
            {
                *error_message = "field '" + field_name + "' has unsupported type '" + parsed_type_name + "'";
            }
            return false;
        }

        field.imgui_type = parsed_type.imgui_type;
        field.reflect_type = parsed_type.reflect_type;
        field.element_size = parsed_type.element_size;
        field.is_native_bool = parsed_type.is_native_bool;

        if (field_json.contains("elementCount") && field_json["elementCount"].is_number_unsigned())
        {
            field.element_count = static_cast<size_t>(field_json["elementCount"].get<pnanovdb_uint64_t>());
        }
        else if (field_json.contains("value") && field_json["value"].is_array())
        {
            field.element_count = field_json["value"].size();
        }
        else
        {
            field.element_count = 1;
        }

        if (field.element_count == 0)
        {
            if (error_message)
            {
                *error_message = "field '" + field_name + "' must have elementCount >= 1";
            }
            return false;
        }

        field.type_name = make_display_type_name(parsed_type_name, field.element_count);
        field.step = field_json.value("step", 0.01f);
        field.is_slider = field_json.value("useSlider", false);
        field.is_bool = field_json.value("isBool", false);
        if (field.is_native_bool)
        {
            field.is_bool = true;
        }

        const size_t alignment = field.element_size >= 8 ? 8 : field.element_size;
        field.offset = align_up(next_offset, alignment);
        next_offset = field.offset + field.element_size * field.element_count;

        field.min_value.clear();
        field.max_value.clear();
        if (field_json.contains("min"))
        {
            field.min_value = make_bound_from_json(field, field_json["min"]);
        }
        if (field_json.contains("max"))
        {
            field.max_value = make_bound_from_json(field, field_json["max"]);
        }

        const bool has_bounds = !field.min_value.empty() && !field.max_value.empty();
        if (field.is_slider && !has_bounds && !field.is_bool && !field.is_native_bool)
        {
            field.is_slider = false;
        }

        field_value_jsons.push_back(field_json.contains("value") ? &field_json["value"] : nullptr);
        m_fields.push_back(std::move(field));
    }

    m_data.assign(next_offset, 0);
    for (size_t i = 0; i < m_fields.size(); ++i)
    {
        auto& field = m_fields[i];
        if (!field_value_jsons[i])
        {
            continue;
        }
        if (field.is_string)
        {
            const std::string& value = field_value_jsons[i]->get_ref<const std::string&>();
            char* dst = reinterpret_cast<char*>(m_data.data() + field.offset);
            const size_t capacity = field.element_count;
            const size_t copy_bytes = std::min(value.size(), capacity - 1);
            std::memcpy(dst, value.data(), copy_bytes);
            dst[copy_bytes] = '\0';
        }
        else
        {
            write_typed_array(field.imgui_type, m_data.data() + field.offset, field.element_size, field.element_count,
                              *field_value_jsons[i], field.is_native_bool);
        }
    }

    m_source_label = source_name;
    rebuildDescriptorViews();
    return true;
}

bool CustomSceneParams::loadFromJsonFile(const std::string& json_file_path, std::string* error_message)
{
    std::ifstream file(json_file_path);
    if (!file)
    {
        if (error_message)
        {
            *error_message = "failed to open JSON file";
        }
        return false;
    }

    std::string json_string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return loadFromJsonString(json_string, json_file_path, error_message);
}

bool CustomSceneParams::fillDesc(const char* shader_name, pnanovdb_shader_params_desc_t* out_desc) const
{
    if (!out_desc || m_fields.empty() || m_data.empty())
    {
        return false;
    }

    out_desc->data = const_cast<void*>(data());
    out_desc->data_size = m_data.size();
    out_desc->shader_name = shader_name ? shader_name : (m_source_label.empty() ? "<custom>" : m_source_label.c_str());
    out_desc->element_names = m_element_names.empty() ? nullptr : const_cast<const char**>(m_element_names.data());
    out_desc->element_typenames =
        m_element_type_names.empty() ? nullptr : const_cast<const char**>(m_element_type_names.data());
    out_desc->element_offsets =
        m_element_offsets.empty() ? nullptr : const_cast<pnanovdb_uint64_t*>(m_element_offsets.data());
    out_desc->element_count = m_fields.size();
    return true;
}

void CustomSceneParams::rebuildDescriptorViews()
{
    m_element_names.clear();
    m_element_type_names.clear();
    m_element_offsets.clear();
    m_reflect_field_type_names.clear();
    m_reflect_field_types.clear();
    m_reflect_fields.clear();

    m_element_names.reserve(m_fields.size());
    m_element_type_names.reserve(m_fields.size());
    m_element_offsets.reserve(m_fields.size());
    m_reflect_field_type_names.reserve(m_fields.size());
    m_reflect_field_types.reserve(m_fields.size());
    m_reflect_fields.reserve(m_fields.size());
    for (const auto& field : m_fields)
    {
        m_element_names.push_back(field.name.c_str());
        m_element_type_names.push_back(field.type_name.c_str());
        m_element_offsets.push_back(field.offset);

        const pnanovdb_reflect_data_type_t* child_type = get_builtin_reflect_data_type(field);
        if (!child_type)
        {
            std::string type_name = field.type_name;
            m_reflect_field_type_names.push_back(std::move(type_name));
            m_reflect_field_types.push_back({ field.reflect_type, field.element_size * field.element_count,
                                              m_reflect_field_type_names.back().c_str(), nullptr, 0u, nullptr });
            child_type = &m_reflect_field_types.back();
        }

        m_reflect_fields.push_back(
            { 0u, PNANOVDB_REFLECT_MODE_VALUE, child_type, field.name.c_str(), field.offset, 0u, 0u, nullptr });
    }

    m_data_type.data_type = PNANOVDB_REFLECT_TYPE_STRUCT;
    m_data_type.element_size = m_data.size();
    m_data_type.struct_typename = "CustomSceneParams";
    m_data_type.child_reflect_datas = m_reflect_fields.empty() ? nullptr : m_reflect_fields.data();
    m_data_type.child_reflect_data_count = m_reflect_fields.size();
    m_data_type.default_value = nullptr;
}

} // namespace pnanovdb_editor
