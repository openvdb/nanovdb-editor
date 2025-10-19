// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/editor/UserParams.cpp

    \author Petra Hapalova

    \brief
*/

#include "ShaderParams.h"

#include "Console.h"

#include "nanovdb_editor/putil/Shader.hpp"

#include <fstream>
#include <algorithm>
#include <filesystem>

namespace imgui_instance_user
{
static std::optional<nlohmann::ordered_json> loadAndParseJsonFile(const std::string& relFilePath,
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

    bool is_valid = nlohmann::json::accept(json_file);
    if (!is_valid)
    {
        json_file.close();
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
        json_file.close();
        return std::nullopt;
    }

    json_file.close();

    if (!json.contains(pnanovdb_shader::SHADER_PARAM_JSON))
    {
        printf("Error: File '%s' should contain '%s'\n", json_filePath.c_str(), pnanovdb_shader::SHADER_PARAM_JSON);
        return std::nullopt;
    }

    return json;
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
    pending_arrays_.clear();
}

void ShaderParams::create(const std::string& shader_name)
{
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
        if (shader_json.contains("shaderParams"))
        {
            auto& shader_params = shader_json["shaderParams"];
            for (auto& [key, value] : shader_params.items())
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
    return loadAndParseJsonFile(shader_name, is_group_file) != std::nullopt;
}

bool ShaderParams::load(const std::string& shader_name, bool reload, bool load_group)
{
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
    if (!shader_json.contains("shaderParams"))
    {
        // compiled shader has no parameters defined
        return false;
    }

    // inserts and/or clears the existing map
    params_map_[shader_name].clear();

    auto& shader_params = shader_json["shaderParams"];
    for (auto& [key, value] : shader_params.items())
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

    auto json_optional = loadAndParseJsonFile(shader_name);
    if (!json_optional)
    {
        return false;
    }

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

        // Allocate pool array now that pending values are set (for group loading)
        if (load_group)
        {
            getAllocatedPoolArray(shader_param);
        }
    }

    if (params_map_[shader_name].empty())
    {
        pnanovdb_editor::Console::getInstance().addLog("No struct %s with parameters found in shader '%s'",
                                                       pnanovdb_shader::SHADER_PARAM_SLANG, shader_name.c_str());
        return false;
    }

    processPendingArrays(shader_name);

    return true;
}

bool ShaderParams::loadGroup(const std::string& group_file, bool reload)
{
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
                    if (param.pool_index != SIZE_MAX)
                    {
                        // only add if this pool index isn't already represented
                        if (group_params_.find(param.pool_index) == group_params_.end())
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
    if (shader_param.pool_index >= shader_params_pool_.size() || shader_params_pool_[shader_param.pool_index].empty())
    {
        return nullptr;
    }
    return shader_params_pool_[shader_param.pool_index].data();
}

void ShaderParams::set_compute_array_for_shader(const std::string& shader_name, pnanovdb_compute_array_t* array)
{
    if (!array)
    {
        return;
    }

    bool hasParams = load(shader_name, false);
    if (!hasParams)
    {
        pending_arrays_[shader_name] = array;
        return;
    }
    std::vector<ShaderParam>& shader_params = *get(shader_name);

    pending_arrays_.erase(shader_name);

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

pnanovdb_compute_array_t* ShaderParams::get_compute_array_for_shader(const std::string& shader_name,
                                                                     const pnanovdb_compute_t* compute)
{
    if (!compute)
    {
        return nullptr;
    }

    bool hasParams = load(shader_name, false);
    if (!hasParams)
    {
        return nullptr;
    }
    std::vector<ShaderParam>& shader_params = *get(shader_name);

    // make constant array at 64KB limit of constant buffer
    pnanovdb_compute_array_t* constant_array =
        compute->create_array(sizeof(char), PNANOVDB_COMPUTE_CONSTANT_BUFFER_MAX_SIZE, nullptr);

    char* shader_param_write_ptr = reinterpret_cast<char*>(constant_array->data);
    size_t shader_param_write_offset = 0;

    // build the combined data structure using pool arrays
    for (auto& shader_param : shader_params)
    {
        getAllocatedPoolArray(shader_param);
        assert(shader_param.pool_index != SIZE_MAX && shader_param.pool_index < shader_params_pool_.size());

        auto& pool_array = shader_params_pool_[shader_param.pool_index];
        if (!pool_array.empty())
        {
            size_t shader_param_size = shader_param.num_elements * shader_param.size;
            if (shader_param_write_offset + shader_param_size <= PNANOVDB_COMPUTE_CONSTANT_BUFFER_MAX_SIZE)
            {
                std::memcpy(shader_param_write_ptr + shader_param_write_offset, pool_array.data(), shader_param_size);
            }
            shader_param_write_offset += shader_param_size;
        }
    }

    return constant_array;
}

size_t ShaderParams::allocatePoolArray(size_t total_size, const void* initial_data)
{
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
    if (pool_index >= shader_params_pool_.size())
    {
        return;
    }

    // don't remove from the vector to avoid invalidating indices, just clear the data
    shader_params_pool_[pool_index].clear();
}

size_t ShaderParams::findEquivalentParamPoolIndex(const ShaderParam& new_param)
{
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

void assignTypedValueOnIndex(ImGuiDataType type, void* target, const nlohmann::json& source, int index)
{
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
                      void* target,
                      const nlohmann::ordered_json& source,
                      const nlohmann::json& defaultValue = nlohmann::json(0))
{
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
    case ImGuiDataType_Float:
    default:
        assignValue<float>(target, source, defaultValue.is_number() ? defaultValue.get<float>() : 0.f);
        break;
    }
}

bool ShaderParams::getAllocatedPoolArray(ShaderParam& shader_param)
{
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
                ((bool*)getValue(shader_param))[0] = value.get<bool>();
            }
        }
        else
        {
            if (value.is_array())
            {
                for (int i = 0; i < shader_param.num_elements; i++)
                {
                    assignTypedValueOnIndex(shader_param.type, getValue(shader_param), value, i);
                }
            }
            else
            {
                assignTypedValue(shader_param.type, getValue(shader_param), value, nlohmann::json(0));
            }
        }
    }
    shader_param.pending_value = nlohmann::json();

    return true;
}

// Helpers to interpret non-bool typed memory as boolean and write back
static bool getElementAsBool(ImGuiDataType type, const void* base_ptr, size_t elem_size, int index)
{
    const char* ptr = static_cast<const char*>(base_ptr) + static_cast<size_t>(index) * elem_size;
    switch (type)
    {
    case ImGuiDataType_S32:
        return *reinterpret_cast<const int32_t*>(ptr) != 0;
    case ImGuiDataType_U32:
        return *reinterpret_cast<const uint32_t*>(ptr) != 0u;
    case ImGuiDataType_S64:
        return *reinterpret_cast<const int64_t*>(ptr) != 0LL;
    case ImGuiDataType_U64:
        return *reinterpret_cast<const uint64_t*>(ptr) != 0ULL;
    case ImGuiDataType_Float:
    default:
        return *reinterpret_cast<const float*>(ptr) != 0.0f;
    }
}

static void setElementFromBool(ImGuiDataType type, void* base_ptr, size_t elem_size, int index, bool value)
{
    char* ptr = static_cast<char*>(base_ptr) + static_cast<size_t>(index) * elem_size;
    switch (type)
    {
    case ImGuiDataType_S32:
        *reinterpret_cast<int32_t*>(ptr) = value ? 1 : 0;
        break;
    case ImGuiDataType_U32:
        *reinterpret_cast<uint32_t*>(ptr) = value ? 1u : 0u;
        break;
    case ImGuiDataType_S64:
        *reinterpret_cast<int64_t*>(ptr) = value ? 1LL : 0LL;
        break;
    case ImGuiDataType_U64:
        *reinterpret_cast<uint64_t*>(ptr) = value ? 1ULL : 0ULL;
        break;
    case ImGuiDataType_Float:
    default:
        *reinterpret_cast<float*>(ptr) = value ? 1.0f : 0.0f;
        break;
    }
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
        { "float16", { ImGuiDataType_Float, sizeof(float) / 2u } },
        { "float", { ImGuiDataType_Float, sizeof(float) } },
        { "double", { ImGuiDataType_Float, sizeof(double) } }
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
    assignTypedValue(shader_param.type, shader_param.getMin(), nlohmann::json(0));
    assignTypedValue(shader_param.type, shader_param.getMax(), nlohmann::json(1));

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

    shader_param.pending_value = value.contains("value") ? value["value"] : nlohmann::json(0);

    assignTypedValue(shader_param.type, shader_param.getMin(), value.contains("min") ? value["min"] : nlohmann::json(0));
    assignTypedValue(shader_param.type, shader_param.getMax(), value.contains("max") ? value["max"] : nlohmann::json(1));

    shader_param.step = value.contains("step") ? value["step"].get<float>() : 0.01f;

    if (shader_param.type != ImGuiDataType_Float)
    {
        if (value.contains("isBool") && value["isBool"].is_boolean())
        {
            shader_param.is_bool = value["isBool"].get<bool>();
        }
    }

    if (shader_param.type != ImGuiDataType_Bool)
    {
        if (value.contains("useSlider") && value["useSlider"].is_boolean())
        {
            shader_param.is_slider = value["useSlider"].get<bool>();
        }
    }

    if (value.contains("hidden") && value["hidden"].is_boolean())
    {
        shader_param.is_hidden = value["hidden"].get<bool>();
    }
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
        shader_param.pending_value = value["value"];
    }

    if (value.contains("hidden") && value["hidden"].is_boolean())
    {
        shader_param.is_hidden = value["hidden"].get<bool>();
    }
}

void ShaderParams::render(const std::string& shader_name)
{
    bool hasParams = load(shader_name, false);
    if (!hasParams)
    {
        return;
    }

    std::vector<ShaderParam>& shader_params = *get(shader_name);
    for (auto& shader_param : shader_params)
    {
        renderParams(shader_name, shader_param);
    }
    return;
}

void ShaderParams::renderGroup(const std::string& group_file_path)
{
    bool hasGroup = loadGroup(group_file_path, false);
    if (!hasGroup)
    {
        ImGui::TextDisabled("Shader params not loaded");
        return;
    }

    // render unique parameters by pool index (avoids duplicates)
    for (auto& [pool_index, shader_param_pair] : group_params_)
    {
        renderParams(shader_param_pair.first, shader_param_pair.second);
    }

    return;
}

void ShaderParams::renderParams(const std::string& shader_name, ShaderParam& shader_param)
{
    if (shader_param.name.find("_pad") != std::string::npos || shader_param.is_hidden)
    {
        return;
    }

    if (!getAllocatedPoolArray(shader_param))
    {
        printf("Error: Failed to allocate UI array for parameter '%s'\n", shader_param.name.c_str());
        return;
    }

    const std::string label = shader_param.name + "##" + shader_name;

    if ((ImGuiDataType)shader_param.type == ImGuiDataType_Bool)
    {
        ImGui::Checkbox(label.c_str(), (bool*)getValue(shader_param));
    }
    else
    {
        if (shader_param.num_elements == 16)
        {
            ImGui::Text("%s", label.c_str());
            size_t num_rows = 4;
            size_t num_cols = 4;
            const char* names[] = { "x", "y", "z", "w" };
            for (int i = 0; i < num_rows; i++)
            {
                void* row_ptr = (char*)getValue(shader_param) + i * num_cols * shader_param.size;
                ImGui::DragScalarN(names[i], (ImGuiDataType)shader_param.type, row_ptr, num_cols, shader_param.step,
                                   shader_param.getMin(), shader_param.getMax());
            }
        }
        else
        {
            if (shader_param.is_bool)
            {
                if (shader_param.num_elements == 1)
                {
                    bool b =
                        getElementAsBool((ImGuiDataType)shader_param.type, getValue(shader_param), shader_param.size, 0);
                    if (ImGui::Checkbox(label.c_str(), &b))
                    {
                        setElementFromBool(
                            (ImGuiDataType)shader_param.type, getValue(shader_param), shader_param.size, 0, b);
                    }
                }
                else
                {
                    printf("Not implemented for num_elements > 1\n");
                }
            }
            else if (shader_param.is_slider)
            {
                const ImGuiDataType dtype = (ImGuiDataType)shader_param.type;
                const char* fmt = nullptr; // let ImGui pick correct integer format
                if (dtype == ImGuiDataType_Float)
                {
                    ImGui::SliderFloat(label.c_str(), (float*)getValue(shader_param), *(float*)shader_param.getMin(),
                                       *(float*)shader_param.getMax(), "%.3f", ImGuiSliderFlags_AlwaysClamp);
                }
                else
                {
                    ImGui::SliderScalarN(label.c_str(), dtype, getValue(shader_param), shader_param.num_elements,
                                         shader_param.getMin(), shader_param.getMax(), fmt, ImGuiSliderFlags_AlwaysClamp);
                }
            }
            else
            {
                ImGui::DragScalarN(label.c_str(), (ImGuiDataType)shader_param.type, getValue(shader_param),
                                   shader_param.num_elements, shader_param.step, shader_param.getMin(),
                                   shader_param.getMax());
            }
        }
    }
    // if (ImGui::IsItemHovered())
    // {
    //     ImGui::SetTooltip("%s", shader_name.c_str());
    // }
}

void ShaderParams::processPendingArrays(const std::string& shader_name)
{
    auto it = pending_arrays_.find(shader_name);
    if (it != pending_arrays_.end())
    {
        // Process the pending array now that parameters are loaded
        pnanovdb_compute_array_t* array = it->second;
        std::vector<ShaderParam>& shader_params = *get(shader_name);

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

        // Remove from pending arrays since it's now processed
        pending_arrays_.erase(it);
    }
}
}
