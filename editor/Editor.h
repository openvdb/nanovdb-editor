// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/editor/Editor.h

    \author Petra Hapalova

    \brief
*/

#include "nanovdb_editor/putil/Reflect.h"
#include "nanovdb_editor/putil/Editor.h"
#include "nanovdb_editor/putil/Pipeline.h"

#include <atomic>
#include <string>
#include <cstdio>
#include <mutex>
#include <memory>
#include <thread>

struct pnanovdb_editor_start_params_t
{
    pnanovdb_compute_device_t* device;
    pnanovdb_editor_config_t config = {};
    std::string config_ip_address;
    std::string config_ui_profile_name;
};

struct pnanovdb_editor_impl_t
{
    const pnanovdb_compiler_t* compiler;
    const pnanovdb_compute_t* compute;

    pnanovdb_pipeline_t pipeline = {};
    pnanovdb_pipeline_registry_t* pipeline_registry = nullptr;
    pnanovdb_pipeline_config_t* pipeline_config = nullptr;

    std::unique_ptr<pnanovdb_editor_start_params_t> start_params;
    std::thread worker_thread;

    std::atomic<pnanovdb_int32_t> resolved_port;

    std::atomic<bool> show_active{ false };
    std::atomic<bool> should_stop{ false };

    std::mutex map_params_mutex;
    const pnanovdb_reflect_data_type_t* map_params_type = nullptr;
    pnanovdb_bool_t map_params_status = PNANOVDB_FALSE;

    pnanovdb_camera_t tmp_camera = {};
    pnanovdb_pipeline_params_t tmp_pipeline_params = {};
};

namespace pnanovdb_editor
{
PNANOVDB_API pnanovdb_editor_t* pnanovdb_get_editor();

} // namespace pnanovdb_editor

#include "imgui/ImguiWindow.h"

pnanovdb_imgui_instance_interface_t* get_user_imgui_instance_interface();

namespace imgui_instance_user
{

struct InstanceUserdata
{
    const pnanovdb_compute_t* compute = nullptr;
    pnanovdb_pipeline_t* pipeline = nullptr;
    pnanovdb_pipeline_registry_t* pipeline_registry = nullptr;
    pnanovdb_pipeline_config_t* pipeline_config = nullptr;
};

}
