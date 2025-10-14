// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/editor/RenderSettingsConfig.cpp

    \author Petra Hapalova

    \brief  Defines which fields are config-only, INI-persistent, or runtime-only
*/

#include "RenderSettingsConfig.h"

#include <fstream>
#include <cstring>

namespace imgui_instance_user
{

void RenderSettingsConfig::load(const pnanovdb_editor_config_t& config)
{
    if (config.ip_address)
    {
        server_address = std::string(config.ip_address);
    }
    else
    {
        server_address = std::nullopt;
    }

    if (config.port > 0)
    {
        server_port = static_cast<int>(config.port);
    }
    else
    {
        server_port = std::nullopt;
    }

    encode_to_file = config.stream_to_file;

    if (config.ui_profile_name)
    {
        ui_profile_name = std::string(config.ui_profile_name);
    }
    else
    {
        ui_profile_name = std::nullopt;
    }
}

void RenderSettingsConfig::applyToSettings(pnanovdb_imgui_settings_render_t& settings) const
{
    // Apply CONFIG_ONLY fields
    if (server_address.has_value())
    {
        strncpy(settings.server_address, server_address->c_str(), sizeof(settings.server_address) - 1);
        settings.server_address[sizeof(settings.server_address) - 1] = '\0';
    }

    if (server_port.has_value())
    {
        settings.server_port = server_port.value();
    }

    settings.encode_to_file = encode_to_file;

    if (ui_profile_name.has_value())
    {
        strncpy(settings.ui_profile_name, ui_profile_name->c_str(), sizeof(settings.ui_profile_name) - 1);
        settings.ui_profile_name[sizeof(settings.ui_profile_name) - 1] = '\0';
    }
}

// INI_PERSISTENT fields - user preferences that should be saved/loaded
void copyPersistentFields(pnanovdb_imgui_settings_render_t& dst, const pnanovdb_imgui_settings_render_t& src)
{
    // Projection and camera preferences
    dst.is_projection_rh = src.is_projection_rh;
    dst.is_orthographic = src.is_orthographic;
    dst.is_reverse_z = src.is_reverse_z;
    dst.is_y_up = src.is_y_up;
    dst.is_upside_down = src.is_upside_down;
    dst.camera_speed_multiplier = src.camera_speed_multiplier;

    // Rendering preferences
    dst.vsync = src.vsync;

    // UI profile
    strncpy(dst.ui_profile_name, src.ui_profile_name, sizeof(dst.ui_profile_name) - 1);
    dst.ui_profile_name[sizeof(dst.ui_profile_name) - 1] = '\0';

    // Camera config
    dst.camera_config.is_projection_rh = src.is_projection_rh;
    dst.camera_config.is_orthographic = src.is_orthographic;
    dst.camera_config.is_reverse_z = src.is_reverse_z;
}

// CONFIG_ONLY fields - from editor's config file, never saved to INI
void copyConfigOnlyFields(pnanovdb_imgui_settings_render_t& dst, const pnanovdb_imgui_settings_render_t& src)
{
    strncpy(dst.server_address, src.server_address, sizeof(dst.server_address) - 1);
    dst.server_address[sizeof(dst.server_address) - 1] = '\0';
    dst.server_port = src.server_port;
    dst.encode_to_file = src.encode_to_file;
}

// RUNTIME_ONLY fields - transient state, never saved
void copyRuntimeOnlyFields(pnanovdb_imgui_settings_render_t& dst, const pnanovdb_imgui_settings_render_t& src)
{
    dst.camera_config.near_plane = src.camera_config.near_plane;
    dst.camera_config.far_plane = src.camera_config.far_plane;

    dst.sync_camera = src.sync_camera;
}
} // namespace imgui_instance_user
