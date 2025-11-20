
// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   ImguiWindow.h

    \author Andrew Reidmeyer

    \brief  This file is part of the PNanoVDB Compute Vulkan reference implementation.
*/

#pragma once

#include "nanovdb_editor/putil/Compute.h"
#include "nanovdb_editor/putil/Camera.h"

struct pnanovdb_imgui_window_t;
typedef struct pnanovdb_imgui_window_t pnanovdb_imgui_window_t;

struct pnanovdb_imgui_instance_t;
typedef struct pnanovdb_imgui_instance_t pnanovdb_imgui_instance_t;

struct pnanovdb_imgui_settings_render_t;
typedef struct pnanovdb_imgui_settings_render_t pnanovdb_imgui_settings_render_t;

struct ImGuiStyle;
typedef struct ImGuiStyle ImGuiStyle;

struct ImGuiIO;
typedef struct ImGuiIO ImGuiIO;

struct ImDrawData;
typedef struct ImDrawData ImDrawData;

#define PNANOVDB_REFLECT_TYPE pnanovdb_vec3_t
PNANOVDB_REFLECT_BEGIN()
PNANOVDB_REFLECT_VALUE(float, x, 0, 0)
PNANOVDB_REFLECT_VALUE(float, y, 0, 0)
PNANOVDB_REFLECT_VALUE(float, z, 0, 0)
PNANOVDB_REFLECT_END(0)
#undef PNANOVDB_REFLECT_TYPE

#define PNANOVDB_REFLECT_TYPE pnanovdb_camera_state_t
PNANOVDB_REFLECT_BEGIN()
PNANOVDB_REFLECT_VALUE(pnanovdb_vec3_t, position, 0, 0)
PNANOVDB_REFLECT_VALUE(pnanovdb_vec3_t, eye_direction, 0, 0)
PNANOVDB_REFLECT_VALUE(pnanovdb_vec3_t, eye_up, 0, 0)
PNANOVDB_REFLECT_VALUE(float, eye_distance_from_position, 0, 0)
PNANOVDB_REFLECT_VALUE(float, orthographic_scale, 0, 0)
PNANOVDB_REFLECT_END(0)
#undef PNANOVDB_REFLECT_TYPE

#define PNANOVDB_REFLECT_TYPE pnanovdb_camera_config_t
PNANOVDB_REFLECT_BEGIN()
PNANOVDB_REFLECT_VALUE(pnanovdb_bool_t, is_projection_rh, 0, 0)
PNANOVDB_REFLECT_VALUE(pnanovdb_bool_t, is_orthographic, 0, 0)
PNANOVDB_REFLECT_VALUE(pnanovdb_bool_t, is_reverse_z, 0, 0)
PNANOVDB_REFLECT_VALUE(float, near_plane, 0, 0)
PNANOVDB_REFLECT_VALUE(float, far_plane, 0, 0)
PNANOVDB_REFLECT_VALUE(float, fov_angle_y, 0, 0)
PNANOVDB_REFLECT_VALUE(float, orthographic_y, 0, 0)
PNANOVDB_REFLECT_VALUE(float, aspect_ratio, 0, 0)
PNANOVDB_REFLECT_END(0)
#undef PNANOVDB_REFLECT_TYPE

typedef struct pnanovdb_imgui_settings_render_t
{
    const pnanovdb_reflect_data_type_t* data_type;
    pnanovdb_bool_t is_projection_rh = true;
    pnanovdb_bool_t is_orthographic = false;
    pnanovdb_bool_t is_reverse_z = true;
    pnanovdb_bool_t is_y_up = true;
    pnanovdb_bool_t is_upside_down = false;
    pnanovdb_bool_t vsync = true;
    pnanovdb_bool_t enable_encoder = false;
    pnanovdb_bool_t encode_to_file = false;
    pnanovdb_bool_t encode_resize = false;
    pnanovdb_bool_t window_resize = false;
    pnanovdb_int32_t window_width = -1;
    pnanovdb_int32_t window_height = -1;
    char encode_filename[256u] = { 'c', 'a', 'p', 't', 'u', 'r', 'e', '_', 's', 't', 'r', 'e', 'a', 'm', '\0' };
    char server_address[256u] = { 'l', 'o', 'c', 'a', 'l', 'h', 'o', 's', 't', '\0' };
    pnanovdb_int32_t server_port = 8080;
    pnanovdb_int32_t server_create_max_attempts = 32;
    pnanovdb_bool_t sync_camera = false;
    pnanovdb_camera_state_t camera_state = {};
    pnanovdb_camera_config_t camera_config = {};
    float camera_speed_multiplier = 1.f;
    char ui_profile_name[256u] = { 'd', 'e', 'f', 'a', 'u', 'l', 't', '\0' };
    // NOTE: When adding new fields here, ensure you categorize them as persistent, config-only,
    //       or runtime-only, and update RenderSettingsConfig.h to reflect the appropriate category

} pnanovdb_imgui_settings_render_t;

#define PNANOVDB_REFLECT_TYPE pnanovdb_imgui_settings_render_t
PNANOVDB_REFLECT_BEGIN()
PNANOVDB_REFLECT_VALUE(pnanovdb_int32_t, is_projection_rh, 0, 0)
PNANOVDB_REFLECT_VALUE(pnanovdb_bool_t, is_orthographic, 0, 0)
PNANOVDB_REFLECT_VALUE(pnanovdb_bool_t, is_reverse_z, 0, 0)
PNANOVDB_REFLECT_VALUE(pnanovdb_bool_t, is_y_up, 0, 0)
PNANOVDB_REFLECT_VALUE(pnanovdb_bool_t, is_upside_down, 0, 0)
PNANOVDB_REFLECT_VALUE(pnanovdb_bool_t, vsync, 0, 0)
PNANOVDB_REFLECT_VALUE(pnanovdb_bool_t, enable_encoder, 0, 0)
PNANOVDB_REFLECT_VALUE(pnanovdb_bool_t, encode_to_file, 0, 0)
PNANOVDB_REFLECT_VALUE(pnanovdb_bool_t, encode_resize, 0, 0)
PNANOVDB_REFLECT_VALUE(pnanovdb_bool_t, window_resize, 0, 0)
PNANOVDB_REFLECT_VALUE(pnanovdb_int32_t, window_width, 0, 0)
PNANOVDB_REFLECT_VALUE(pnanovdb_int32_t, window_height, 0, 0)
PNANOVDB_REFLECT_VALUE(char, encode_filename[0], 0, 0)
PNANOVDB_REFLECT_VALUE(char, server_address[0], 0, 0)
PNANOVDB_REFLECT_VALUE(pnanovdb_int32_t, server_port, 0, 0)
PNANOVDB_REFLECT_VALUE(pnanovdb_int32_t, server_create_max_attempts, 0, 0)
PNANOVDB_REFLECT_VALUE(pnanovdb_bool_t, sync_camera, 0, 0)
PNANOVDB_REFLECT_VALUE(pnanovdb_camera_state_t, camera_state, 0, 0)
PNANOVDB_REFLECT_VALUE(pnanovdb_camera_config_t, camera_config, 0, 0)
PNANOVDB_REFLECT_VALUE(float, camera_speed_multiplier, 0, 0)
PNANOVDB_REFLECT_END(0)
#undef PNANOVDB_REFLECT_TYPE

typedef struct pnanovdb_imgui_instance_interface_t
{
    PNANOVDB_REFLECT_INTERFACE();

    pnanovdb_imgui_instance_t*(PNANOVDB_ABI* create)(void* userdata,
                                                     void* user_settings,
                                                     const pnanovdb_reflect_data_type_t* user_settings_data_type);

    void(PNANOVDB_ABI* destroy)(pnanovdb_imgui_instance_t* instance);

    void(PNANOVDB_ABI* update)(pnanovdb_imgui_instance_t* instance);

    ImGuiStyle*(PNANOVDB_ABI* get_style)(pnanovdb_imgui_instance_t* instance);

    ImGuiIO*(PNANOVDB_ABI* get_io)(pnanovdb_imgui_instance_t* instance);

    void(PNANOVDB_ABI* get_tex_data_as_rgba32)(pnanovdb_imgui_instance_t* instance,
                                               unsigned char** out_pixels,
                                               int* out_width,
                                               int* out_height);

    ImDrawData*(PNANOVDB_ABI* get_draw_data)(pnanovdb_imgui_instance_t* instance);

} pnanovdb_imgui_instance_interface_t;

#define PNANOVDB_REFLECT_TYPE pnanovdb_imgui_instance_interface_t
PNANOVDB_REFLECT_BEGIN()
PNANOVDB_REFLECT_FUNCTION_POINTER(create, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(destroy, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(update, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(get_style, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(get_io, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(get_tex_data_as_rgba32, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(get_draw_data, 0, 0)
PNANOVDB_REFLECT_END(0)
PNANOVDB_REFLECT_INTERFACE_IMPL()
#undef PNANOVDB_REFLECT_TYPE

typedef struct pnanovdb_imgui_window_interface_t
{
    PNANOVDB_REFLECT_INTERFACE();

    pnanovdb_imgui_window_t*(PNANOVDB_ABI* create)(const pnanovdb_compute_t* compute,
                                                   const pnanovdb_compute_device_t* device,
                                                   pnanovdb_int32_t width,
                                                   pnanovdb_int32_t height,
                                                   void** imgui_user_settings,
                                                   pnanovdb_bool_t enable_default_imgui,
                                                   pnanovdb_imgui_instance_interface_t** imgui_instance_interfaces,
                                                   void** imgui_instance_userdatas,
                                                   pnanovdb_uint64_t imgui_instance_instance_count,
                                                   pnanovdb_bool_t headless);

    void(PNANOVDB_ABI* destroy)(const pnanovdb_compute_t* compute,
                                pnanovdb_compute_queue_t* queue,
                                pnanovdb_imgui_window_t* window,
                                pnanovdb_imgui_settings_render_t* settings);

    pnanovdb_bool_t(PNANOVDB_ABI* update)(const pnanovdb_compute_t* compute,
                                          pnanovdb_compute_queue_t* compute_queue,
                                          pnanovdb_compute_texture_transient_t* background,
                                          pnanovdb_int32_t* out_width,
                                          pnanovdb_int32_t* out_height,
                                          void(PNANOVDB_ABI* report_resolved_port)(void* userdata, pnanovdb_int32_t port),
                                          void* report_resolved_port_userdata,
                                          pnanovdb_imgui_window_t* window,
                                          pnanovdb_imgui_settings_render_t* user_settings,
                                          pnanovdb_int32_t (*get_external_active_count)(void* external_active_count),
                                          void* external_active_count);

    void(PNANOVDB_ABI* get_camera_view_proj)(pnanovdb_imgui_window_t* window,
                                             pnanovdb_int32_t* out_width,
                                             pnanovdb_int32_t* out_height,
                                             pnanovdb_camera_mat_t* out_view,
                                             pnanovdb_camera_mat_t* out_projection);

    void(PNANOVDB_ABI* get_camera)(pnanovdb_imgui_window_t* window,
                                   pnanovdb_camera_state_t* out_camera_state,
                                   pnanovdb_camera_config_t* out_camera_config);

    void(PNANOVDB_ABI* update_camera)(pnanovdb_imgui_window_t* window, pnanovdb_imgui_settings_render_t* user_settings);

} pnanovdb_imgui_window_interface_t;

#define PNANOVDB_REFLECT_TYPE pnanovdb_imgui_window_interface_t
PNANOVDB_REFLECT_BEGIN()
PNANOVDB_REFLECT_FUNCTION_POINTER(create, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(destroy, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(update, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(get_camera_view_proj, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(get_camera, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(update_camera, 0, 0)
PNANOVDB_REFLECT_END(0)
PNANOVDB_REFLECT_INTERFACE_IMPL()
#undef PNANOVDB_REFLECT_TYPE

typedef pnanovdb_imgui_window_interface_t*(PNANOVDB_ABI* PFN_pnanovdb_imgui_get_window_interface)();

PNANOVDB_API pnanovdb_imgui_window_interface_t* pnanovdb_imgui_get_window_interface();

PNANOVDB_API void pnanovdb_imgui_set_system_clipboard(const char* text);
PNANOVDB_API const char* pnanovdb_imgui_get_system_clipboard();
