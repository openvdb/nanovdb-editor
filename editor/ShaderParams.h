// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/editor/ShaderParams.h

    \author Petra Hapalova

    \brief
*/

#include "nanovdb_editor/putil/Compute.h"

#include <nlohmann/json.hpp>

#ifndef IMGUI_DEFINE_MATH_OPERATORS
#    define IMGUI_DEFINE_MATH_OPERATORS
#endif // IMGUI_DEFINE_MATH_OPERATORS

#include <imgui.h>

#include <string>
#include <vector>
#include <map>
#include <cstring>
#include <optional>

#pragma once

namespace pnanovdb_editor
{
struct ShaderParam
{
    std::string name;
    ImGuiDataType type;
    size_t pool_index; // index into shader_params_pool_ (-1 if not allocated)
    size_t size;
    size_t num_elements;
    std::vector<char> min;
    std::vector<char> max;
    float step;
    bool is_slider = false; // use slider in UI, for integers only
    bool is_bool = false; // use checkbox in UI, for integers only
    bool is_hidden = false; // do not show in UI
    nlohmann::json pending_value; // store value to apply when pool array is allocated

    ShaderParam() : pool_index(SIZE_MAX), size(0), num_elements(0), step(0.0f)
    {
    }

    ShaderParam(const ShaderParam& other) = default;
    ShaderParam& operator=(const ShaderParam& other) = default;
    ShaderParam(ShaderParam&& other) noexcept = default;
    ShaderParam& operator=(ShaderParam&& other) noexcept = default;
    ~ShaderParam() = default;

    // compare shader params by name, type, min, max, step, and num_elements to determine if pool index is the same
    bool operator==(const ShaderParam& other) const
    {
        return name == other.name && type == other.type && min == other.min && max == other.max && step == other.step &&
               num_elements == other.num_elements;
    }

    void* getMin()
    {
        return min.empty() ? nullptr : min.data();
    }
    const void* getMin() const
    {
        return min.empty() ? nullptr : min.data();
    }

    void* getMax()
    {
        return max.empty() ? nullptr : max.data();
    }
    const void* getMax() const
    {
        return max.empty() ? nullptr : max.data();
    }

    void resizeData(size_t new_size, size_t new_num_elements)
    {
        size = new_size;
        num_elements = new_num_elements;
        size_t total_size = size * num_elements;
        min.resize(total_size);
        max.resize(total_size);
    }

    void clearData()
    {
        pool_index = SIZE_MAX;
        min.clear();
        max.clear();
        size = 0;
        num_elements = 0;
    }

    void setMin(const void* data, size_t data_size)
    {
        if (data && data_size > 0)
        {
            min.resize(data_size);
            std::memcpy(min.data(), data, data_size);
        }
        else
        {
            min.clear();
        }
    }

    void setMax(const void* data, size_t data_size)
    {
        if (data && data_size > 0)
        {
            max.resize(data_size);
            std::memcpy(max.data(), data, data_size);
        }
        else
        {
            max.clear();
        }
    }
};

class ShaderParams
{
public:
    ShaderParams() = default;
    ~ShaderParams();

    void create(const std::string& shader_name);
    void createGroup(const std::string& group_name);
    void render(const std::string& shader_name);
    void renderGroup(const std::string& group_file_path);
    bool isJsonLoaded(const std::string& shader_name, bool is_group_file = false);
    bool load(const std::string& shader_name, bool reload, bool load_group = false);
    bool loadGroup(const std::string& group_file, bool reload);

    std::vector<ShaderParam>* get(const std::string& shader_name)
    {
        if (params_map_.find(shader_name) == params_map_.end())
        {
            return nullptr;
        }
        return &params_map_.at(shader_name);
    }

    // returns index of the allocated array
    size_t allocatePoolArray(size_t total_size, const void* initial_data = nullptr);
    void deallocatePoolArray(size_t pool_index);
    bool getAllocatedPoolArray(ShaderParam& shader_param);
    size_t findEquivalentParamPoolIndex(const ShaderParam& new_param);

    const void* getValue(const ShaderParam& shader_param);
    void set_compute_array_for_shader(const std::string& shader_name, pnanovdb_compute_array_t* array);
    pnanovdb_compute_array_t* get_compute_array_for_shader(const std::string& shader_name,
                                                           const pnanovdb_compute_t* compute);

private:
    std::vector<std::vector<char>> shader_params_pool_; // each array corresponds to a shader parameter pool index
    std::map<std::string, std::vector<ShaderParam>> params_map_; // <shader_name, shader_params>
    std::map<size_t, std::pair<std::string, ShaderParam>> group_params_; // <pool_index, <shader_file, ShaderParam>>
    std::map<std::string, pnanovdb_compute_array_t*> pending_arrays_; // <shader_name, array>

    void* getValue(ShaderParam& shader_param);
    void createDefaultScalarNParam(const std::string& name,
                                   nlohmann::ordered_json& value,
                                   nlohmann::ordered_json& json_shader_params);
    void createScalarNParam(const std::string& name, const nlohmann::json& value, std::vector<ShaderParam>& params);
    void addToScalarNParam(const std::string& name, const nlohmann::json& value, std::vector<ShaderParam>& params);
    void createDefaultBoolParam(const std::string& name, nlohmann::ordered_json& json_shader_params);
    void createBoolParam(const std::string& name, const nlohmann::json& value, std::vector<ShaderParam>& params);
    void addToBoolParam(const std::string& name, const nlohmann::json& value, std::vector<ShaderParam>& params);

    void renderParams(const std::string& shader_name, ShaderParam& shader_param);
    void processPendingArrays(const std::string& shader_name);
};
}
