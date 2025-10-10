// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/editor/FileHeaderInfo.h

    \author Petra Hapalova

    \brief  NanoVDB grid/file header information window
*/

#pragma once

#include <imgui.h>

#include "ImguiInstance.h"

namespace pnanovdb_editor
{
class FileHeaderInfo
{
public:
    static FileHeaderInfo& getInstance();

    bool render(pnanovdb_compute_array_t* array);
};
}
