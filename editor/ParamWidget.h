// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   editor/ParamWidget.h

    \author Petra Hapalova

    \brief  ImGui widgets and shared helpers for shader and pipeline parameters
*/

#pragma once

#include "nanovdb_editor/putil/Editor.h"
#include "nanovdb_editor/putil/Reflect.h"

#include <imgui.h>
#include <nlohmann/json_fwd.hpp>

#include <cstddef>
#include <string>
#include <vector>

namespace pnanovdb_editor
{

struct ParamWidgetSpec
{
    std::string display_name;
    std::string label;
    ImGuiDataType type = ImGuiDataType_Float;
    void* value = nullptr;
    size_t element_size = sizeof(float);
    size_t element_count = 1;
    const void* min_value = nullptr;
    const void* max_value = nullptr;
    float step = 0.01f;
    bool is_slider = false;
    bool is_bool = false;
    bool is_hidden = false;
    bool is_native_bool = false;
};

struct ParamWidgetHints
{
    std::string label;
    std::string tooltip;
    bool has_min = false;
    bool has_max = false;
    float min_value = 0.0f;
    float max_value = 0.0f;
    bool has_step = false;
    float step = 0.01f;
    bool is_bool = false;
    bool use_slider = false;
    bool hidden = false;
    std::vector<std::string> components;
};

struct ReflectRenderableField
{
    const pnanovdb_reflect_data_t* field = nullptr;
    ImGuiDataType imgui_type = ImGuiDataType_Float;
    size_t elem_size = 0;
};

void renderParamWidget(const ParamWidgetSpec& spec);

void parseParamWidgetHints(const nlohmann::json& field, ParamWidgetHints& out);

bool param_widget_uses_bool_control(ImGuiDataType type, const ParamWidgetHints* hints);

size_t reflect_scalar_field_size(pnanovdb_reflect_type_t rt);

bool reflect_type_to_imgui_type(pnanovdb_reflect_type_t rt, ImGuiDataType& out_type, size_t& out_size);

double imgui_read_scalar(ImGuiDataType type, const void* src);
void imgui_write_scalar(ImGuiDataType type, void* dst, double v);

float half_bits_to_float(uint16_t bits);
uint16_t float_to_half_bits(float value);

bool reflect_next_renderable_field(const pnanovdb_reflect_data_type_t* data_type,
                                   size_t blob_size,
                                   pnanovdb_uint64_t& index,
                                   ReflectRenderableField& out);

bool reflect_read_scalar_json(pnanovdb_reflect_type_t rt, const unsigned char* p, nlohmann::ordered_json& out);

bool reflect_write_scalar_json(pnanovdb_reflect_type_t rt, unsigned char* p, const nlohmann::json& in);

} // namespace pnanovdb_editor
