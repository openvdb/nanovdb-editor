// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/editor/RenderSettingsConfig.h

    \author Petra Hapalova

    \brief  Defines which fields are config-only, INI-persistent, or runtime-only
*/

#pragma once

#include "imgui/ImguiWindow.h"
#include "nanovdb_editor/putil/Editor.h"

#define PNANOVDB_C
#include <nanovdb/PNanoVDB.h>
#undef PNANOVDB_C

#include <string>
#include <optional>
#include <nlohmann/json.hpp>

namespace imgui_instance_user
{
struct RenderSettingsConfig
{
    // optionals are std::nullopt to indicate that the default value is used
    std::optional<std::string> server_address;
    std::optional<int> server_port;
    bool encode_to_file;
    std::optional<std::string> ui_profile_name;

    void load(const pnanovdb_editor_config_t& config);
    void applyToSettings(pnanovdb_imgui_settings_render_t& settings) const;
};

// Copy functions for selective field copying
void copyPersistentFields(pnanovdb_imgui_settings_render_t& dst, const pnanovdb_imgui_settings_render_t& src);
void copyConfigOnlyFields(pnanovdb_imgui_settings_render_t& dst, const pnanovdb_imgui_settings_render_t& src);
void copyRuntimeOnlyFields(pnanovdb_imgui_settings_render_t& dst, const pnanovdb_imgui_settings_render_t& src);

} // namespace imgui_instance_user
