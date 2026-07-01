// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   editor/PipelineParams.h

    \author Petra Hapalova

    \brief
*/

#pragma once

#include "ParamWidget.h"
#include "nanovdb_editor/putil/Reflect.h"

#include <cstddef>
#include <map>
#include <string>

namespace pnanovdb_editor
{
inline constexpr const char* PIPELINE_PARAM_JSON = "PipelineParams";

class PipelineParams
{
public:
    struct EditResult
    {
        bool any_edited = false;
        bool any_active = false;
        bool any_committed = false;
    };

    EditResult render(const pnanovdb_reflect_data_type_t* data_type,
                      const char* hints_name,
                      unsigned char* data,
                      size_t size,
                      const char* id_suffix);

    bool primary_field(const pnanovdb_reflect_data_type_t* data_type,
                       const char* hints_name,
                       const unsigned char* data,
                       size_t size,
                       std::string& out_label,
                       double& out_value);

    void clear_cache();

private:
    const std::map<std::string, ParamWidgetHints>& hints_for(const char* hints_name);

    std::map<std::string, std::map<std::string, ParamWidgetHints>> hints_cache_;
};

} // namespace pnanovdb_editor
