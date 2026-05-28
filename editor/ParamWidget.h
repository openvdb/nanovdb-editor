// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   editor/ParamWidget.h

    \author Petra Hapalova

    \brief
*/

#pragma once

#include "nanovdb_editor/putil/Editor.h"

#include <imgui.h>

#include <string>

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

void renderParamWidget(const ParamWidgetSpec& spec);

} // namespace pnanovdb_editor
