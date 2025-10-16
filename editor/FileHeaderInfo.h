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
    static FileHeaderInfo& getInstance()
    {
        static FileHeaderInfo instance;
        return instance;
    }

    bool render(pnanovdb_compute_array_t* array);

private:
    FileHeaderInfo() = default;
    ~FileHeaderInfo() = default;

    FileHeaderInfo(const FileHeaderInfo&) = delete;
    FileHeaderInfo& operator=(const FileHeaderInfo&) = delete;
    FileHeaderInfo(FileHeaderInfo&&) = delete;
    FileHeaderInfo& operator=(FileHeaderInfo&&) = delete;
};
}
