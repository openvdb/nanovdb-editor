// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/editor/Properties.h

    \author Petra Hapalova

    \brief  Properties window for displaying and managing scene item properties
*/

#pragma once

#include <imgui.h>

namespace imgui_instance_user
{
struct Instance;
}

namespace pnanovdb_editor
{
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
};
}
