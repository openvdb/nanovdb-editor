// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/editor/Properties.h

    \author Petra Hapalova

    \brief  Properties window for displaying and managing scene item properties
*/

#pragma once

#include <imgui.h>

#include <cstdint>
#include <unordered_map>
#include <vector>

struct pnanovdb_editor_token_t;

namespace imgui_instance_user
{
struct Instance;
}

namespace pnanovdb_editor
{
class EditorScene;
class EditorSceneManager;

struct PropertiesPanelState
{
    int stage = 1; // pnanovdb_pipeline_stage_process
    int step = 0; // process step index (0 == primary process stage)
    uint64_t params_scene = 0;
    uint64_t params_object = 0;
    int params_step = -1;
    int params_type = -1;
    bool params_editing = false;
    std::vector<unsigned char> params_snapshot;
};

class Properties
{
public:
    static Properties& getInstance()
    {
        static Properties instance;
        return instance;
    }

    void render(imgui_instance_user::Instance* ptr);

private:
    Properties() = default;
    ~Properties() = default;

    Properties(const Properties&) = delete;
    Properties& operator=(const Properties&) = delete;
    Properties(Properties&&) = delete;
    Properties& operator=(Properties&&) = delete;

    void showCameraViews(imgui_instance_user::Instance* ptr);
    void showPipelinePanel(imgui_instance_user::Instance* ptr,
                           EditorSceneManager* scene_manager,
                           EditorScene* editor_scene,
                           pnanovdb_editor_token_t* scene_token,
                           pnanovdb_editor_token_t* name_token,
                           const char* suffix,
                           bool allow_shader_edit);

    std::unordered_map<uint64_t, PropertiesPanelState> m_panel_state;
};
}
