// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/editor/UserParams.cpp

    \author Petra Hapalova

    \brief
*/

#include "ShaderParams.h"
#include "ParamWidget.h"
#include "ColorRamp.h"

#include "Console.h"

#include "nanovdb_editor/putil/Shader.hpp"

#include <fstream>
#include <algorithm>
#include <filesystem>
#include <set>
#include <cmath>

namespace pnanovdb_editor
{
static nlohmann::ordered_json* getCompiledShaderParamsObject(nlohmann::ordered_json& json)
{
    if (json.contains(pnanovdb_shader::SHADER_PARAM_JSON))
    {
        return &json[pnanovdb_shader::SHADER_PARAM_JSON];
    }
    if (json.contains(pnanovdb_shader::GENERATED_SHADER_PARAM_JSON))
    {
        return &json[pnanovdb_shader::GENERATED_SHADER_PARAM_JSON];
    }
    return nullptr;
}

static std::optional<nlohmann::ordered_json> loadJsonFileFromShaderPath(const std::string& relFilePath,
                                                                        bool is_group_file = false)
{
    std::string json_filePath;
    if (is_group_file)
    {
        std::filesystem::path fsPath(relFilePath);
        json_filePath =
            (std::filesystem::path(pnanovdb_shader::getShaderDir()) / fsPath).string() + pnanovdb_shader::JSON_EXT;
    }
    else
    {
        json_filePath = pnanovdb_shader::getShaderParamsFilePath(relFilePath.c_str());
    }
    std::ifstream json_file(json_filePath);
    if (!json_file)
    {
        return std::nullopt;
    }

    if (!nlohmann::json::accept(json_file))
    {
        return std::nullopt;
    }

    json_file.clear();
    json_file.seekg(0);

    nlohmann::ordered_json json;
    try
    {
        json_file >> json;
    }
    catch (const nlohmann::json::parse_error& e)
    {
        printf("Error parsing file '%s': %s\n", json_filePath.c_str(), e.what());
        return std::nullopt;
    }

    return json;
}

static std::optional<nlohmann::ordered_json> loadAndParseJsonFile(const std::string& relFilePath,
                                                                  bool is_group_file = false)
{
    std::optional<nlohmann::ordered_json> json = loadJsonFileFromShaderPath(relFilePath, is_group_file);
    if (!json)
    {
        return std::nullopt;
    }

    if (!json->contains(pnanovdb_shader::SHADER_PARAM_JSON))
    {
        printf("Error: File should contain '%s'\n", pnanovdb_shader::SHADER_PARAM_JSON);
        return std::nullopt;
    }

    return json;
}

std::optional<nlohmann::ordered_json> loadParamHintsJson(const std::string& shader_base_name, const char* section_key)
{
    if (shader_base_name.empty() || !section_key || !section_key[0])
    {
        return std::nullopt;
    }
    const std::optional<nlohmann::ordered_json> json = loadJsonFileFromShaderPath(shader_base_name);
    if (!json || !json->contains(section_key))
    {
        return std::nullopt;
    }
    return (*json)[section_key];
}

std::optional<nlohmann::ordered_json> loadShaderParamsJson(const std::string& shader_base_name)
{
    if (shader_base_name.empty())
    {
        return std::nullopt;
    }
    return loadAndParseJsonFile(shader_base_name);
}

static std::optional<ShaderColorRamp> parseColorRamp(const nlohmann::json& value)
{
    if (!value.contains("widget") || value["widget"] != "colorRamp")
    {
        return std::nullopt;
    }
    ShaderColorRamp ramp;
    if ((value.contains("label") && !value["label"].is_string()) ||
        (value.contains("tooltip") && !value["tooltip"].is_string()))
    {
        return std::nullopt;
    }
    ramp.label = value.value("label", "Color ramp");
    ramp.tooltip = value.value("tooltip", "");
    std::set<std::string> names;
    auto read_names = [&](const char* key, std::vector<std::string>& output)
    {
        if (!value.contains(key) || !value[key].is_array() || value[key].empty())
        {
            return false;
        }
        for (const auto& entry : value[key])
        {
            if (!entry.is_string() || entry.get_ref<const std::string&>().empty())
            {
                return false;
            }
            const auto name = entry.get<std::string>();
            if (!names.insert(name).second)
            {
                return false;
            }
            output.push_back(name);
        }
        return true;
    };
    if (!read_names("positions", ramp.positions) || !read_names("colors", ramp.colors))
    {
        return std::nullopt;
    }
    return ramp;
}

ShaderParams::~ShaderParams()
{
    for (auto& [shader_name, shader_params] : params_map_)
    {
        for (auto& shader_param : shader_params)
        {
            shader_param.pool_index = SIZE_MAX;
        }
    }
    shader_params_pool_.clear();
    params_map_.clear();
    group_params_.clear();
    pending_arrays_data_.clear();
}

void ShaderParams::create(const std::string& shader_name)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    std::string json_filePath = pnanovdb_shader::getShaderParamsFilePath(shader_name.c_str());
    std::filesystem::path fsPath(json_filePath);
    if (std::filesystem::exists(fsPath))
    {
        pnanovdb_editor::Console::getInstance().addLog("Shader params file '%s' already exists", json_filePath.c_str());
        return;
    }

    auto json_shader_params = nlohmann::ordered_json::object();

    // load parameters from compiled shader
    std::string shader_json_path = pnanovdb_shader::getCompiledShaderParamsFilePath(shader_name.c_str());
    std::ifstream shader_json_file(shader_json_path);
    if (shader_json_file)
    {
        nlohmann::ordered_json shader_json;
        shader_json_file >> shader_json;
        if (nlohmann::ordered_json* shader_params = getCompiledShaderParamsObject(shader_json))
        {
            for (auto& [key, value] : shader_params->items())
            {
                if (key.find("_pad") != std::string::npos)
                {
                    continue;
                }
                assert(value.contains("type"));
                if (value["type"] == "bool")
                {
                    createDefaultBoolParam(key, json_shader_params);
                }
                else
                {
                    createDefaultScalarNParam(key, value, json_shader_params);
                }
            }
        }
        shader_json_file.close();
    }

    nlohmann::ordered_json json;
    json[pnanovdb_shader::SHADER_PARAM_JSON] = json_shader_params;

    std::ofstream json_file(json_filePath);
    json_file << json.dump(4);
    json_file << '\n';
    json_file.close();

    pnanovdb_editor::Console::getInstance().addLog("Shader params file '%s' created", json_filePath.c_str());
}

void ShaderParams::createGroup(const std::string& group_name)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    std::string json_filePath = pnanovdb_shader::getShaderParamsFilePath(group_name.c_str());
    std::filesystem::path fsPath(json_filePath);
    if (std::filesystem::exists(fsPath))
    {
        pnanovdb_editor::Console::getInstance().addLog("Group params file '%s' already exists", json_filePath.c_str());
        return;
    }

    nlohmann::ordered_json json = nlohmann::ordered_json::array();

    std::ofstream json_file(json_filePath);
    json_file << json.dump(4);
    json_file.close();

    pnanovdb_editor::Console::getInstance().addLog("Group params file '%s' created", json_filePath.c_str());
}

bool ShaderParams::isJsonLoaded(const std::string& shader_name, bool is_group_file)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return loadAndParseJsonFile(shader_name, is_group_file) != std::nullopt;
}

bool ShaderParams::load(const std::string& shader_name, bool reload, bool load_group)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    // lazy load
    if (params_map_.find(shader_name) != params_map_.end() && !reload)
    {
        return true;
    }

    std::string shader_json_path = pnanovdb_shader::getCompiledShaderParamsFilePath(shader_name.c_str());
    std::ifstream shader_json_file(shader_json_path);
    if (!shader_json_file)
    {
        return false;
    }

    nlohmann::ordered_json shader_json;
    try
    {
        shader_json_file >> shader_json;
    }
    catch (const nlohmann::json::parse_error& e)
    {
        // jsong migt be incomplete if being written to
        shader_json_file.close();
        return false;
    }
    shader_json_file.close();
    if (!shader_json.is_object())
    {
        return false;
    }
    nlohmann::ordered_json* shader_params = getCompiledShaderParamsObject(shader_json);
    if (shader_params && !shader_params->is_null() && !shader_params->is_object())
    {
        return false;
    }

    params_map_[shader_name].clear();
    if (!shader_params || shader_params->is_null())
    {
        return true;
    }

    for (auto& [key, value] : shader_params->items())
    {
        if (key.find("_pad") != std::string::npos)
        {
            continue;
        }

        assert(value.contains("type"));
        if (value["type"] == "bool")
        {
            createBoolParam(key, value, params_map_.at(shader_name));
        }
        else
        {
            createScalarNParam(key, value, params_map_.at(shader_name));
        }
    }

    // Load user JSON file if it exists (contains customizations like defaults, min/max, hidden flags)
    // If it doesn't exist, we'll still use the parameters from the compiled shader
    auto json_optional = loadAndParseJsonFile(shader_name);
    if (json_optional)
    {
        nlohmann::ordered_json json = *json_optional;
        auto& json_shader_params = json.at(pnanovdb_shader::SHADER_PARAM_JSON);
        for (auto& shader_param : params_map_[shader_name])
        {
            if (!json_shader_params.contains(shader_param.name))
            {
                continue;
            }

            auto& value = json_shader_params.at(shader_param.name);
            if (shader_param.type == ImGuiDataType_Bool)
            {
                addToBoolParam(shader_param.name, value, params_map_[shader_name]);
            }
            else
            {
                addToScalarNParam(shader_param.name, value, params_map_[shader_name]);
            }

            shader_param.color_ramp = parseColorRamp(value);

            // Allocate pool array now that pending values are set (for group loading)
            if (load_group)
            {
                getAllocatedPoolArray(shader_param);
            }
        }
    }

    processPendingArrays(shader_name);

    return true;
}

bool ShaderParams::loadGroup(const std::string& group_file, bool reload)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!reload && !group_params_.empty())
    {
        return true;
    }

    auto groups_json_optional = loadAndParseJsonFile(group_file, true);
    if (!groups_json_optional)
    {
        return false;
    }

    group_params_.clear();
    nlohmann::ordered_json groups_json = *groups_json_optional;

    auto& json_shader_params = groups_json.at(pnanovdb_shader::SHADER_PARAM_JSON);
    for (auto& shader_name_json : json_shader_params)
    {
        if (!shader_name_json.is_string())
        {
            continue;
        }

        std::string shader_name = shader_name_json.get<std::string>();

        // load the shader to get its parameters and pool indices
        if (load(shader_name, false, true))
        {
            auto* shader_params = get(shader_name);
            if (shader_params)
            {
                for (auto& param : *shader_params)
                {
                    if (getAllocatedPoolArray(param) && param.pool_index != SIZE_MAX)
                    {
                        // Keep ramp metadata when shaders share a parameter pool.
                        const auto existing = group_params_.find(param.pool_index);
                        if (existing == group_params_.end() || (!existing->second.second.color_ramp && param.color_ramp))
                        {
                            group_params_[param.pool_index] = std::make_pair(shader_name, param);
                        }
                    }
                }
            }
        }
        else
        {
            return false;
        }
    }

    return true;
}

void* ShaderParams::getValue(ShaderParam& shader_param)
{
    if (shader_param.pool_index >= shader_params_pool_.size() || shader_params_pool_[shader_param.pool_index].empty())
    {
        return nullptr;
    }
    return shader_params_pool_[shader_param.pool_index].data();
}

const void* ShaderParams::getValue(const ShaderParam& shader_param)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (shader_param.pool_index >= shader_params_pool_.size() || shader_params_pool_[shader_param.pool_index].empty())
    {
        return nullptr;
    }
    return shader_params_pool_[shader_param.pool_index].data();
}

void ShaderParams::set_compute_array_for_shader(const std::string& shader_name, pnanovdb_compute_array_t* array)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!array)
    {
        return;
    }

    bool hasParams = load(shader_name, false);
    if (!hasParams)
    {
        // Compiled layout not available yet; do not enqueue a pending blob of unknown layout.
        // When the layout becomes available, JSON defaults (pending_value) will be applied on first allocation.
        return;
    }
    std::vector<ShaderParam>& shader_params = *get(shader_name);

    pending_arrays_data_.erase(shader_name);

    char* shader_param_ptr = reinterpret_cast<char*>(array->data);
    const size_t capacity = static_cast<size_t>(array->element_size * array->element_count);
    size_t remaining = capacity;
    size_t total_size = 0;

    for (auto& shader_param : shader_params)
    {
        getAllocatedPoolArray(shader_param);
        assert(shader_param.pool_index != SIZE_MAX && shader_param.pool_index < shader_params_pool_.size());

        auto& pool_array = shader_params_pool_[shader_param.pool_index];
        if (!pool_array.empty())
        {
            size_t shader_param_size = shader_param.num_elements * shader_param.size;
            if (remaining == 0)
            {
                break;
            }
            size_t to_copy = shader_param_size <= remaining ? shader_param_size : remaining;
            std::memcpy(pool_array.data(), shader_param_ptr, to_copy);
            shader_param_ptr += to_copy;
            total_size += to_copy;
            remaining -= to_copy;
            if (to_copy < shader_param_size)
            {
                // Source blob shorter than declared params; stop to avoid OOB
                break;
            }
        }
    }

    if (total_size > PNANOVDB_COMPUTE_CONSTANT_BUFFER_MAX_SIZE || total_size > capacity)
    {
        printf("Error: Shader params size %zu exceeds buffer capacity (cap=%zu, maxCB=%u)\n", total_size, capacity,
               PNANOVDB_COMPUTE_CONSTANT_BUFFER_MAX_SIZE);
    }
}

void ShaderParams::clear_pending_array_for_shader(const std::string& shader_name)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    pending_arrays_data_.erase(shader_name);
}

size_t ShaderParams::copy_params_to_buffer(const std::string& shader_name, void* dst, size_t dst_size)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!dst || dst_size == 0)
    {
        return 0;
    }

    bool hasParams = load(shader_name, false);
    if (!hasParams)
    {
        return 0;
    }
    std::vector<ShaderParam>& shader_params = *get(shader_name);

    char* write_ptr = reinterpret_cast<char*>(dst);
    size_t write_offset = 0;

    // build the combined data structure using pool arrays
    for (auto& shader_param : shader_params)
    {
        getAllocatedPoolArray(shader_param);
        assert(shader_param.pool_index != SIZE_MAX && shader_param.pool_index < shader_params_pool_.size());

        auto& pool_array = shader_params_pool_[shader_param.pool_index];
        if (!pool_array.empty())
        {
            size_t shader_param_size = shader_param.num_elements * shader_param.size;
            if (write_offset + shader_param_size <= dst_size)
            {
                std::memcpy(write_ptr + write_offset, pool_array.data(), shader_param_size);
                write_offset += shader_param_size;
            }
            else
            {
                break;
            }
        }
    }

    return write_offset;
}

pnanovdb_compute_array_t* ShaderParams::get_compute_array_for_shader(const std::string& shader_name,
                                                                     const pnanovdb_compute_t* compute)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!compute)
    {
        return nullptr;
    }

    bool hasParams = load(shader_name, false);
    if (!hasParams)
    {
        return nullptr;
    }

    pnanovdb_compute_array_t* constant_array =
        compute->create_array(sizeof(char), PNANOVDB_COMPUTE_CONSTANT_BUFFER_MAX_SIZE, nullptr);

    copy_params_to_buffer(shader_name, constant_array->data, PNANOVDB_COMPUTE_CONSTANT_BUFFER_MAX_SIZE);

    return constant_array;
}

size_t ShaderParams::allocatePoolArray(size_t total_size, const void* initial_data)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    std::vector<char> pool_array(total_size);

    if (initial_data)
    {
        std::memcpy(pool_array.data(), initial_data, total_size);
    }
    else
    {
        std::memset(pool_array.data(), 0, total_size);
    }

    shader_params_pool_.push_back(std::move(pool_array));
    return shader_params_pool_.size() - 1;
}

void ShaderParams::deallocatePoolArray(size_t pool_index)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (pool_index >= shader_params_pool_.size())
    {
        return;
    }

    // don't remove from the vector to avoid invalidating indices, just clear the data
    shader_params_pool_[pool_index].clear();
}

size_t ShaderParams::findEquivalentParamPoolIndex(const ShaderParam& new_param)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    for (const auto& [shader_name, shader_params] : params_map_)
    {
        for (const auto& existing_param : shader_params)
        {
            if (existing_param == new_param && existing_param.pool_index != SIZE_MAX)
            {
                return existing_param.pool_index;
            }
        }
    }
    return SIZE_MAX;
}

template <typename T>
void assignValueOnIndex(void* target, const nlohmann::json& source, int index)
{
    nlohmann::basic_json json_val;
    try
    {
        json_val = source.at(index);
    }
    catch (const nlohmann::json::out_of_range&)
    {
        json_val = nlohmann::json(T(0));
    }

    T val = json_val.get<T>();
    memcpy(static_cast<char*>(target) + index * sizeof(T), &val, sizeof(T));
}

void assignTypedValueOnIndex(ImGuiDataType type, size_t element_size, void* target, const nlohmann::json& source, int index)
{
    if (type == ImGuiDataType_Float && element_size == sizeof(uint16_t))
    {
        float value = 0.0f;
        try
        {
            value = source.at(index).get<float>();
        }
        catch (const nlohmann::json::out_of_range&)
        {
        }
        const uint16_t bits = float_to_half_bits(value);
        std::memcpy(static_cast<char*>(target) + static_cast<size_t>(index) * element_size, &bits, sizeof(bits));
        return;
    }
    switch (type)
    {
    case ImGuiDataType_S32:
        assignValueOnIndex<int>(target, source, index);
        break;
    case ImGuiDataType_U32:
        assignValueOnIndex<unsigned int>(target, source, index);
        break;
    case ImGuiDataType_S64:
        assignValueOnIndex<long long>(target, source, index);
        break;
    case ImGuiDataType_U64:
        assignValueOnIndex<unsigned long long>(target, source, index);
        break;
    case ImGuiDataType_Double:
        assignValueOnIndex<double>(target, source, index);
        break;
    case ImGuiDataType_Float:
    default:
        assignValueOnIndex<float>(target, source, index);
        break;
    }
}

template <typename T>
void assignValue(void* target, const nlohmann::ordered_json& source, T defaultValue = T(0))
{
    T val = source.is_number() ? source.get<T>() : defaultValue;
    memcpy(target, &val, sizeof(T));
}

void assignTypedValue(ImGuiDataType type,
                      size_t element_size,
                      void* target,
                      const nlohmann::ordered_json& source,
                      const nlohmann::json& defaultValue = nlohmann::json(0))
{
    if (type == ImGuiDataType_Float && element_size == sizeof(uint16_t))
    {
        const float value =
            source.is_number() ? source.get<float>() : (defaultValue.is_number() ? defaultValue.get<float>() : 0.0f);
        const uint16_t bits = float_to_half_bits(value);
        std::memcpy(target, &bits, sizeof(bits));
        return;
    }
    switch (type)
    {
    case ImGuiDataType_S32:
        assignValue<int>(target, source, defaultValue.is_number() ? defaultValue.get<int>() : 0);
        break;
    case ImGuiDataType_U32:
        assignValue<unsigned int>(target, source, defaultValue.is_number() ? defaultValue.get<unsigned int>() : 0u);
        break;
    case ImGuiDataType_S64:
        assignValue<long long>(target, source, defaultValue.is_number() ? defaultValue.get<long long>() : 0LL);
        break;
    case ImGuiDataType_U64:
        assignValue<unsigned long long>(
            target, source, defaultValue.is_number() ? defaultValue.get<unsigned long long>() : 0ULL);
        break;
    case ImGuiDataType_Double:
        assignValue<double>(target, source, defaultValue.is_number() ? defaultValue.get<double>() : 0.0);
        break;
    case ImGuiDataType_Float:
    default:
        assignValue<float>(target, source, defaultValue.is_number() ? defaultValue.get<float>() : 0.f);
        break;
    }
}

size_t ShaderParams::copy_default_params_to_buffer(const std::string& shader_name, void* dst, size_t dst_size)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!dst || dst_size == 0 || !load(shader_name, false))
    {
        return 0;
    }
    const std::vector<ShaderParam>* shader_params = get(shader_name);
    if (!shader_params)
    {
        return 0;
    }

    char* write_ptr = static_cast<char*>(dst);
    size_t write_offset = 0;
    for (const ShaderParam& shader_param : *shader_params)
    {
        const size_t field_size = shader_param.size * shader_param.num_elements;
        if (write_offset > dst_size || field_size > dst_size - write_offset)
        {
            break;
        }

        void* field = write_ptr + write_offset;
        std::memset(field, 0, field_size);
        const nlohmann::json& value = shader_param.default_value;
        if (shader_param.type == ImGuiDataType_Bool)
        {
            if (value.is_boolean())
            {
                static_cast<pnanovdb_bool_t*>(field)[0] = value.get<bool>() ? PNANOVDB_TRUE : PNANOVDB_FALSE;
            }
            else if (value.is_number())
            {
                static_cast<pnanovdb_bool_t*>(field)[0] = value.get<int>() != 0 ? PNANOVDB_TRUE : PNANOVDB_FALSE;
            }
        }
        else if (value.is_array())
        {
            for (size_t i = 0; i < shader_param.num_elements; ++i)
            {
                assignTypedValueOnIndex(shader_param.type, shader_param.size, field, value, static_cast<int>(i));
            }
        }
        else if (!value.is_null())
        {
            assignTypedValue(shader_param.type, shader_param.size, field, value, nlohmann::json(0));
        }
        write_offset += field_size;
    }
    return write_offset;
}

bool ShaderParams::getAllocatedPoolArray(ShaderParam& shader_param)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    // lazy allocate
    if (shader_param.pool_index != SIZE_MAX)
    {
        return true;
    }

    size_t total_size = shader_param.size * shader_param.num_elements;

    // initialize with default values
    std::vector<char> default_data(total_size);
    std::memset(default_data.data(), 0, total_size);

    shader_param.pool_index = allocatePoolArray(total_size, default_data.data());

    // apply pending json value
    if (!shader_param.pending_value.is_null())
    {
        auto& value = shader_param.pending_value;
        if (shader_param.type == ImGuiDataType_Bool)
        {
            if (value.is_boolean())
            {
                ((pnanovdb_bool_t*)getValue(shader_param))[0] = value.get<bool>() ? PNANOVDB_TRUE : PNANOVDB_FALSE;
            }
        }
        else
        {
            if (value.is_array())
            {
                for (int i = 0; i < shader_param.num_elements; i++)
                {
                    assignTypedValueOnIndex(shader_param.type, shader_param.size, getValue(shader_param), value, i);
                }
            }
            else
            {
                assignTypedValue(shader_param.type, shader_param.size, getValue(shader_param), value, nlohmann::json(0));
            }
        }
    }
    shader_param.pending_value = nlohmann::json();

    return true;
}

bool ShaderParams::resetToDefaults(const std::string& shader_name)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!load(shader_name, false))
    {
        return false;
    }

    auto* params = get(shader_name);
    if (!params)
    {
        return false;
    }

    for (auto& shader_param : *params)
    {
        if (!getAllocatedPoolArray(shader_param))
        {
            continue;
        }
        void* pool_data = getValue(shader_param);
        if (!pool_data)
        {
            continue;
        }

        const size_t total_size = shader_param.size * shader_param.num_elements;
        std::memset(pool_data, 0, total_size);

        const auto& value = shader_param.default_value;
        if (value.is_null())
        {
            continue;
        }

        if (shader_param.type == ImGuiDataType_Bool)
        {
            if (value.is_boolean())
            {
                ((pnanovdb_bool_t*)pool_data)[0] = value.get<bool>() ? PNANOVDB_TRUE : PNANOVDB_FALSE;
            }
            else if (value.is_number())
            {
                ((pnanovdb_bool_t*)pool_data)[0] = value.get<int>() != 0 ? PNANOVDB_TRUE : PNANOVDB_FALSE;
            }
        }
        else if (value.is_array())
        {
            for (int i = 0; i < shader_param.num_elements; i++)
            {
                assignTypedValueOnIndex(shader_param.type, shader_param.size, pool_data, value, i);
            }
        }
        else
        {
            assignTypedValue(shader_param.type, shader_param.size, pool_data, value, nlohmann::json(0));
        }
    }
    return true;
}

bool ShaderParams::resetGroupToDefaults(const std::string& group_file_path)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!loadGroup(group_file_path, false))
    {
        return false;
    }

    std::set<std::string> seen_shaders;
    bool any_reset = false;
    for (const auto& [pool_index, shader_param_pair] : group_params_)
    {
        if (seen_shaders.insert(shader_param_pair.first).second)
        {
            any_reset |= resetToDefaults(shader_param_pair.first);
        }
    }
    return any_reset;
}

static std::pair<ImGuiDataType, size_t> getScalarTypeAndSize(const std::string& type)
{
    static const std::unordered_map<std::string, std::pair<ImGuiDataType, size_t>> typeMap = {
        { "", { ImGuiDataType_Float, sizeof(float) } },
        { "void", { ImGuiDataType_Float, sizeof(float) } },
        { "int", { ImGuiDataType_S32, sizeof(int32_t) } },
        { "uint", { ImGuiDataType_U32, sizeof(uint32_t) } },
        { "int64", { ImGuiDataType_S64, sizeof(int64_t) } },
        { "uint64", { ImGuiDataType_U64, sizeof(uint64_t) } },
        { "float16", { ImGuiDataType_Float, sizeof(uint16_t) } },
        { "half", { ImGuiDataType_Float, sizeof(uint16_t) } },
        { "float", { ImGuiDataType_Float, sizeof(float) } },
        { "double", { ImGuiDataType_Double, sizeof(double) } }
    };

    auto it = typeMap.find(type);
    if (it != typeMap.end())
    {
        return it->second;
    }
    return { ImGuiDataType_Float, sizeof(float) };
}

void ShaderParams::createDefaultScalarNParam(const std::string& name,
                                             nlohmann::ordered_json& value,
                                             nlohmann::ordered_json& json_shader_params)
{
    nlohmann::ordered_json param;
    assert(value.contains("elementCount"));
    size_t num_elements = value["elementCount"];
    if (num_elements > 1)
    {
        nlohmann::ordered_json array = nlohmann::json::array();
        for (size_t i = 0; i < num_elements; i++)
        {
            array.push_back(0);
        }
        param["value"] = array;
    }
    else
    {
        param["value"] = 0;
    }
    param["min"] = 0;
    param["max"] = 1;
    param["step"] = 0.01;
    param["useSlider"] = false;
    param["isBool"] = false;
    param["hidden"] = false;
    json_shader_params[name] = param;
}

void ShaderParams::createScalarNParam(const std::string& name, const nlohmann::json& value, std::vector<ShaderParam>& params)
{
    ShaderParam shader_param;
    shader_param.name = name;
    auto [type, size] = getScalarTypeAndSize(value["type"]);
    shader_param.type = type;
    shader_param.size = size;
    assert(value.contains("elementCount"));
    shader_param.num_elements = value["elementCount"];
    shader_param.resizeData(size, shader_param.num_elements);

    // min/max values are set up for UI controls
    assignTypedValue(shader_param.type, shader_param.size, shader_param.getMin(), nlohmann::json(0));
    assignTypedValue(shader_param.type, shader_param.size, shader_param.getMax(), nlohmann::json(1));

    size_t existing_pool_index = findEquivalentParamPoolIndex(shader_param);
    if (existing_pool_index != SIZE_MAX)
    {
        shader_param.pool_index = existing_pool_index;
        // printf("Reusing pool index %zu for duplicate parameter '%s'\n", existing_pool_index, name.c_str());
    }

    params.emplace_back(std::move(shader_param));
}

void ShaderParams::addToScalarNParam(const std::string& name, const nlohmann::json& value, std::vector<ShaderParam>& params)
{
    auto it =
        std::find_if(params.begin(), params.end(), [&name](const ShaderParam& param) { return param.name == name; });

    if (it == params.end())
    {
        return;
    }

    ShaderParam& shader_param = *it;

    shader_param.default_value = value.contains("value") ? value["value"] : nlohmann::json(0);
    shader_param.pending_value = shader_param.default_value;

    assignTypedValue(shader_param.type, shader_param.size, shader_param.getMin(),
                     value.contains("min") ? value["min"] : nlohmann::json(0));
    assignTypedValue(shader_param.type, shader_param.size, shader_param.getMax(),
                     value.contains("max") ? value["max"] : nlohmann::json(1));

    ParamWidgetHints hints;
    parseParamWidgetHints(value, hints);
    shader_param.step = hints.has_step ? hints.step : 0.01f;
    if (shader_param.type != ImGuiDataType_Float)
    {
        shader_param.is_bool = hints.is_bool;
    }
    if (shader_param.type != ImGuiDataType_Bool)
    {
        shader_param.is_slider = hints.use_slider;
    }
    shader_param.is_hidden = hints.hidden;
}

void ShaderParams::createDefaultBoolParam(const std::string& name, nlohmann::ordered_json& json_shader_params)
{
    nlohmann::json param;
    param["value"] = false;
    json_shader_params[name] = param;
}

void ShaderParams::createBoolParam(const std::string& name, const nlohmann::json& value, std::vector<ShaderParam>& params)
{
    static const size_t slang_sizeof_bool = sizeof(uint32_t);

    ShaderParam shader_param;
    shader_param.name = name;
    shader_param.num_elements = 1;
    shader_param.type = ImGuiDataType_Bool;
    shader_param.size = slang_sizeof_bool;
    shader_param.is_native_bool = true;

    shader_param.resizeData(slang_sizeof_bool, 1);

    // check if an equivalent parameter already exists and reuse its pool index
    size_t existing_pool_index = findEquivalentParamPoolIndex(shader_param);
    if (existing_pool_index != SIZE_MAX)
    {
        shader_param.pool_index = existing_pool_index;
        // printf("Reusing pool index %zu for duplicate parameter '%s'\n", existing_pool_index, name.c_str());
    }

    params.emplace_back(std::move(shader_param));
}

void ShaderParams::addToBoolParam(const std::string& name, const nlohmann::json& value, std::vector<ShaderParam>& params)
{
    auto it =
        std::find_if(params.begin(), params.end(), [&name](const ShaderParam& param) { return param.name == name; });

    if (it == params.end())
    {
        return;
    }

    ShaderParam& shader_param = *it;

    // store pending values for when compute array is allocated
    if (value.contains("value") && value["value"].is_boolean())
    {
        shader_param.default_value = value["value"];
        shader_param.pending_value = value["value"];
    }

    ParamWidgetHints hints;
    parseParamWidgetHints(value, hints);
    shader_param.is_hidden = hints.hidden;
}

void ShaderParams::render(const std::string& shader_name)
{
    std::vector<RenderableParamSnapshot> snapshots;
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        bool hasParams = load(shader_name, false);
        if (!hasParams)
        {
            pnanovdb_editor::Console::getInstance().addLog(
                Console::LogLevel::Debug, "ShaderParams::render() - Failed to load params for shader '%s'",
                shader_name.c_str());
            ImGui::TextDisabled("Shader parameters not available");
            return;
        }

        std::vector<ShaderParam>& shader_params = *get(shader_name);
        pnanovdb_editor::Console::getInstance().addLog(Console::LogLevel::Trace,
                                                       "ShaderParams::render() - Rendering %zu params for shader '%s'",
                                                       shader_params.size(), shader_name.c_str());
        buildRenderSnapshots(shader_name, shader_params, snapshots);
    }

    renderSnapshotsAndWriteBack(snapshots);
}

void ShaderParams::renderGroup(const std::string& group_file_path)
{
    std::vector<RenderableParamSnapshot> snapshots;
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        bool hasGroup = loadGroup(group_file_path, false);
        if (!hasGroup)
        {
            ImGui::TextDisabled("Shader params not loaded");
            return;
        }

        // render unique parameters by pool index (avoids duplicates)
        std::set<size_t> rendered_pools;
        for (auto& [pool_index, shader_param_pair] : group_params_)
        {
            auto* shader_params = get(shader_param_pair.first);
            if (!shader_params)
            {
                continue;
            }
            auto current =
                std::find_if(shader_params->begin(), shader_params->end(),
                             [&](const ShaderParam& field) { return field.name == shader_param_pair.second.name; });
            if (current == shader_params->end() || !getAllocatedPoolArray(*current) ||
                !rendered_pools.insert(current->pool_index).second)
            {
                continue;
            }
            std::vector<ShaderParam> single{ *current };
            buildRenderSnapshots(shader_param_pair.first, single, snapshots);
        }
    }

    renderSnapshotsAndWriteBack(snapshots);
}

void ShaderParams::buildRenderSnapshots(const std::string& shader_name,
                                        std::vector<ShaderParam>& params,
                                        std::vector<RenderableParamSnapshot>& out)
{
    // NOTE: caller holds m_mutex.
    for (auto& shader_param : params)
    {
        if (shader_param.name.find("_pad") != std::string::npos || shader_param.is_hidden)
        {
            continue;
        }
        if (!getAllocatedPoolArray(shader_param))
        {
            printf("Error: Failed to allocate UI array for parameter '%s'\n", shader_param.name.c_str());
            continue;
        }
        if (shader_param.pool_index >= shader_params_pool_.size())
        {
            continue;
        }
        const std::vector<char>& pool_array = shader_params_pool_[shader_param.pool_index];

        RenderableParamSnapshot snap;
        snap.shader_name = shader_name;
        snap.name = shader_param.name;
        snap.type = (ImGuiDataType)shader_param.type;
        snap.size = shader_param.size;
        snap.num_elements = shader_param.num_elements;
        snap.pool_index = shader_param.pool_index;
        snap.value = pool_array;
        snap.min = shader_param.min;
        snap.max = shader_param.max;
        snap.step = shader_param.step;
        snap.is_slider = shader_param.is_slider;
        snap.is_bool = shader_param.is_bool;
        snap.is_hidden = shader_param.is_hidden;
        snap.is_native_bool = shader_param.is_native_bool;
        snap.color_ramp = shader_param.color_ramp;
        out.push_back(std::move(snap));
    }
}

void ShaderParams::renderSnapshotsAndWriteBack(std::vector<RenderableParamSnapshot>& snapshots)
{
    if (snapshots.empty())
    {
        return;
    }

    std::vector<std::vector<char>> originals(snapshots.size());
    for (size_t i = 0; i < snapshots.size(); ++i)
    {
        originals[i] = snapshots[i].value;
    }

    auto finite_point = [](const ColorRampPoint& point)
    {
        return std::isfinite(point.position) &&
               std::all_of(point.color.begin(), point.color.end(), [](float value) { return std::isfinite(value); });
    };
    std::vector<bool> consumed(snapshots.size(), false);
    std::vector<std::vector<size_t>> ramp_edits;
    for (size_t count_index = 0; count_index < snapshots.size(); ++count_index)
    {
        auto& count = snapshots[count_index];
        if (!count.color_ramp || consumed[count_index] || count.type != ImGuiDataType_U32 ||
            count.size != sizeof(uint32_t) || count.num_elements != 1 || count.value.size() != sizeof(uint32_t) ||
            count.is_bool || count.is_native_bool)
        {
            continue;
        }
        const auto& metadata = *count.color_ramp;
        std::vector<size_t> bound{ count_index };
        std::vector<std::pair<size_t, size_t>> positions;
        std::vector<size_t> colors;
        auto find_field = [&](const std::string& name) -> size_t
        {
            std::lock_guard<std::recursive_mutex> lock(m_mutex);
            auto shader = params_map_.find(count.shader_name);
            if (shader == params_map_.end())
            {
                return SIZE_MAX;
            }
            auto field = std::find_if(shader->second.begin(), shader->second.end(),
                                      [&](const ShaderParam& param) { return param.name == name; });
            if (field == shader->second.end() || field->is_hidden)
            {
                return SIZE_MAX;
            }
            for (size_t i = 0; i < snapshots.size(); ++i)
            {
                const auto& snap = snapshots[i];
                if (snap.name == name && snap.pool_index == field->pool_index && !consumed[i] &&
                    snap.type == ImGuiDataType_Float && snap.size == sizeof(float) && !snap.is_bool &&
                    snap.num_elements > 0 && snap.value.size() == sizeof(float) * snap.num_elements &&
                    std::find(bound.begin(), bound.end(), i) == bound.end())
                {
                    return i;
                }
            }
            return SIZE_MAX;
        };
        bool valid = true;
        for (const auto& name : metadata.positions)
        {
            const size_t index = find_field(name);
            if (index == SIZE_MAX)
            {
                valid = false;
                break;
            }
            bound.push_back(index);
            for (size_t element = 0; element < snapshots[index].num_elements; ++element)
            {
                positions.emplace_back(index, element);
            }
        }
        for (const auto& name : metadata.colors)
        {
            const size_t index = find_field(name);
            if (index == SIZE_MAX || snapshots[index].num_elements != 4)
            {
                valid = false;
                break;
            }
            bound.push_back(index);
            colors.push_back(index);
        }
        uint32_t point_count;
        std::memcpy(&point_count, count.value.data(), sizeof(point_count));
        if (!valid || positions.size() != colors.size() || point_count == 0 || point_count > colors.size())
        {
            continue;
        }
        std::vector<ColorRampPoint> points;
        for (size_t i = 0; i < colors.size(); ++i)
        {
            ColorRampPoint point;
            const auto [index, element] = positions[i];
            std::memcpy(&point.position, snapshots[index].value.data() + element * sizeof(float), sizeof(float));
            std::memcpy(point.color.data(), snapshots[colors[i]].value.data(), sizeof(float) * 4);
            valid &= finite_point(point);
            if (i < point_count)
            {
                points.push_back(point);
            }
        }
        if (!valid)
        {
            continue;
        }
        for (const size_t index : bound)
        {
            consumed[index] = true;
        }
        const std::string label = metadata.label + "###" + count.shader_name + "/" + count.name;
        if (renderColorRamp(label.c_str(), points, colors.size()) && !points.empty() &&
            points.size() <= colors.size() && std::all_of(points.begin(), points.end(), finite_point))
        {
            point_count = static_cast<uint32_t>(points.size());
            std::memcpy(count.value.data(), &point_count, sizeof(point_count));
            for (size_t i = 0; i < points.size(); ++i)
            {
                const auto [index, element] = positions[i];
                std::memcpy(snapshots[index].value.data() + element * sizeof(float), &points[i].position, sizeof(float));
                std::memcpy(snapshots[colors[i]].value.data(), points[i].color.data(), sizeof(float) * 4);
            }
            ramp_edits.push_back(std::move(bound));
        }
        if (!metadata.tooltip.empty() && ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("%s", metadata.tooltip.c_str());
        }
    }

    for (size_t i = 0; i < snapshots.size(); ++i)
    {
        if (consumed[i])
        {
            continue;
        }
        RenderableParamSnapshot& snap = snapshots[i];
        ParamWidgetSpec ui_spec;
        ui_spec.display_name = snap.name;
        ui_spec.label = snap.name + "##" + snap.shader_name;
        ui_spec.type = snap.type;
        ui_spec.value = snap.value.empty() ? nullptr : snap.value.data();
        ui_spec.element_size = snap.size;
        ui_spec.element_count = snap.num_elements;
        ui_spec.min_value = snap.min.empty() ? nullptr : snap.min.data();
        ui_spec.max_value = snap.max.empty() ? nullptr : snap.max.data();
        ui_spec.step = snap.step;
        ui_spec.is_slider = snap.is_slider;
        ui_spec.is_bool = snap.is_bool;
        ui_spec.is_hidden = snap.is_hidden;
        ui_spec.is_native_bool = snap.is_native_bool;
        renderParamWidget(ui_spec);
    }

    // Write any edited bytes back into the pool under the lock.
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    for (const auto& bound : ramp_edits)
    {
        // Reject the whole edit if a reload or another writer changed a bound field.
        const bool current = std::all_of(
            bound.begin(), bound.end(),
            [&](size_t index)
            {
                const auto& snap = snapshots[index];
                auto shader = params_map_.find(snapshots[bound.front()].shader_name);
                if (shader == params_map_.end() || snap.pool_index >= shader_params_pool_.size() ||
                    shader_params_pool_[snap.pool_index] != originals[index])
                {
                    return false;
                }
                return std::any_of(shader->second.begin(), shader->second.end(),
                                   [&](const ShaderParam& field)
                                   {
                                       return field.name == snap.name && field.pool_index == snap.pool_index &&
                                              field.type == snap.type && field.size == snap.size &&
                                              field.num_elements == snap.num_elements && !field.is_hidden &&
                                              field.color_ramp == snap.color_ramp;
                                   });
            });
        if (current)
        {
            for (const size_t index : bound)
            {
                const auto& snap = snapshots[index];
                std::memcpy(shader_params_pool_[snap.pool_index].data(), snap.value.data(), snap.value.size());
            }
        }
    }
    for (size_t i = 0; i < snapshots.size(); ++i)
    {
        if (consumed[i])
        {
            continue;
        }
        const RenderableParamSnapshot& snap = snapshots[i];
        if (snap.pool_index >= shader_params_pool_.size())
        {
            continue;
        }
        if (snap.value == originals[i])
        {
            continue;
        }
        std::vector<char>& pool_array = shader_params_pool_[snap.pool_index];
        if (pool_array.size() == snap.value.size() && !snap.value.empty())
        {
            std::memcpy(pool_array.data(), snap.value.data(), snap.value.size());
        }
    }
}

void ShaderParams::processPendingArrays(const std::string& shader_name)
{
    auto it = pending_arrays_data_.find(shader_name);
    if (it != pending_arrays_data_.end())
    {
        // Process the pending raw bytes now that parameters are loaded
        std::vector<char>& blob = it->second;
        std::vector<ShaderParam>& shader_params = *get(shader_name);

        const char* shader_param_ptr = blob.empty() ? nullptr : blob.data();
        const size_t capacity = blob.size();
        size_t remaining = capacity;
        size_t total_size = 0;

        for (auto& shader_param : shader_params)
        {
            getAllocatedPoolArray(shader_param);
            assert(shader_param.pool_index != SIZE_MAX && shader_param.pool_index < shader_params_pool_.size());

            auto& pool_array = shader_params_pool_[shader_param.pool_index];
            if (!pool_array.empty())
            {
                size_t shader_param_size = shader_param.num_elements * shader_param.size;
                if (remaining == 0)
                {
                    break;
                }
                const size_t to_copy = shader_param_size <= remaining ? shader_param_size : remaining;
                if (shader_param_ptr)
                {
                    std::memcpy(pool_array.data(), shader_param_ptr, to_copy);
                    shader_param_ptr += to_copy;
                }
                total_size += to_copy;
                remaining -= to_copy;
                if (to_copy < shader_param_size)
                {
                    // Source blob shorter than declared params; stop to avoid OOB
                    break;
                }
            }
        }

        // Remove from pending arrays since it's now processed
        pending_arrays_data_.erase(it);
    }
}
}
