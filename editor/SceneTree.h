// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/editor/SceneTree.h

    \author Petra Hapalova

    \brief  Scene tree window for displaying and managing scene hierarchy
*/

#pragma once

#define PNANOVDB_C
#include <nanovdb/PNanoVDB.h>
#undef PNANOVDB_C

#include <imgui/ImguiTLS.h>
#include <imgui.h>

namespace imgui_instance_user
{
struct Instance;
}

namespace pnanovdb_editor
{
class SceneTree
{
public:
    static SceneTree& getInstance()
    {
        static SceneTree instance;
        return instance;
    }

    void render(imgui_instance_user::Instance* ptr);

private:
    SceneTree() = default;
    ~SceneTree() = default;

    SceneTree(const SceneTree&) = delete;
    SceneTree& operator=(const SceneTree&) = delete;
    SceneTree(SceneTree&&) = delete;
    SceneTree& operator=(SceneTree&&) = delete;

    bool renderSceneItem(const char* name,
                         bool isSelected,
                         float indentSpacing = 0.0f,
                         bool useIndent = false,
                         pnanovdb_bool_t* visibilityCheckbox = nullptr);

    bool renderTreeNodeHeader(const char* label, bool* visibilityCheckbox = nullptr, bool isSelected = false);
};
}
