// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <imgui.h>

namespace pnanovdb_editor_test
{

inline void create_imgui_context()
{
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(640, 480);
    io.DeltaTime = 1.f / 60.f;
    unsigned char* pixels;
    int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
}

} // namespace pnanovdb_editor_test
