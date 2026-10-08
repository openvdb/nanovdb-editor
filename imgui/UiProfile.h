// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   UiProfile.h

    \author Petra Hapalova

    \brief
*/

#pragma once

#include <cstring>

namespace pnanovdb_imgui
{
enum class UiLayout
{
    Editor,
    Viewer,
};

struct UiProfile
{
    const char* name;
    const char* title;
    UiLayout layout;
};

inline constexpr UiProfile ui_profiles[] = {
    { "default", "NanoVDB Editor", UiLayout::Editor },
    { "viewer", "NanoVDB Editor - fVDB", UiLayout::Viewer },
    { "nvflow", "NanoVDB Editor - NvFlow", UiLayout::Viewer },
};

inline const UiProfile& ui_profile(const char* name)
{
    for (const auto& profile : ui_profiles)
    {
        if (name && std::strcmp(name, profile.name) == 0)
        {
            return profile;
        }
    }
    return ui_profiles[0];
}
}
