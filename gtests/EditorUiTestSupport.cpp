// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

#include "nanovdb_editor/putil/Reflect.h"

#include <imgui.h>

PNANOVDB_API ImGuiContext* pnanovdb_editor_test_get_imgui_context()
{
    return ImGuiTLS;
}
