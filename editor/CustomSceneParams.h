// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   editor/CustomSceneParams.h

    \author Petra Hapalova

    \brief
*/

#pragma once

#include "nanovdb_editor/putil/Editor.h"

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace pnanovdb_editor
{

class CustomSceneParams
{
public:
    struct Field
    {
        std::string name;
        std::string type_name;
        ImGuiDataType imgui_type = ImGuiDataType_Float;
        pnanovdb_uint32_t reflect_type = PNANOVDB_REFLECT_TYPE_FLOAT;
        size_t offset = 0;
        size_t element_size = sizeof(float);
        size_t element_count = 1;
        std::vector<char> min_value;
        std::vector<char> max_value;
        float step = 0.01f;
        bool is_slider = false;
        bool is_bool = false;
        bool is_hidden = false;
        bool is_native_bool = false;
        bool is_string = false;
    };

    bool loadFromJsonString(const std::string& json_string,
                            const std::string& source_name = "<custom>",
                            std::string* error_message = nullptr);
    bool loadFromJsonFile(const std::string& json_file_path, std::string* error_message = nullptr);

    bool empty() const
    {
        return m_fields.empty();
    }

    const std::string& sourceLabel() const
    {
        return m_source_label;
    }

    size_t dataSize() const
    {
        return m_data.size();
    }

    void* data()
    {
        return m_data.empty() ? nullptr : m_data.data();
    }

    const void* data() const
    {
        return m_data.empty() ? nullptr : m_data.data();
    }

    const pnanovdb_reflect_data_type_t* dataType() const
    {
        return m_fields.empty() ? nullptr : &m_data_type;
    }

    void render();
    bool fillDesc(const char* shader_name, pnanovdb_shader_params_desc_t* out_desc) const;

    std::mutex& dataMutex() const
    {
        return m_data_mutex;
    }

private:
    mutable std::mutex m_data_mutex;
    std::string m_source_label;
    std::vector<Field> m_fields;
    std::vector<char> m_data;

    std::vector<const char*> m_element_names;
    std::vector<const char*> m_element_type_names;
    std::vector<pnanovdb_uint64_t> m_element_offsets;
    std::vector<std::string> m_reflect_field_type_names;
    std::vector<pnanovdb_reflect_data_type_t> m_reflect_field_types;
    std::vector<pnanovdb_reflect_data_t> m_reflect_fields;
    pnanovdb_reflect_data_type_t m_data_type = {
        PNANOVDB_REFLECT_TYPE_STRUCT, 0u, "CustomSceneParams", nullptr, 0u, nullptr
    };

    void rebuildDescriptorViews();
};

} // namespace pnanovdb_editor
