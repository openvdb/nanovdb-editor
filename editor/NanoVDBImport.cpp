// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/editor/NanoVDBImport.cpp

    \author Petra Hapalova

    \brief
*/

#include "EditorImport.h"

#include "EditorScene.h"
#include "Console.h"

#include "nanovdb_editor/putil/Compute.h"

namespace pnanovdb_editor
{
namespace nanovdb_import
{

bool nanovdb(EditorScene& editor_scene,
             const pnanovdb_compute_t* compute,
             pnanovdb_editor_token_t* scene,
             const char* filepath)
{
    if (!scene || !filepath || !compute)
    {
        return false;
    }

    pnanovdb_compute_array_t* array = compute->load_nanovdb(filepath);
    if (!array)
    {
        Console::getInstance().addLog(Console::LogLevel::Error, "Failed to load '%s'", filepath);
        return false;
    }

    editor_scene.handle_nanovdb_data_load(scene, array, filepath);
    Console::getInstance().addLog("Loaded NanoVDB from '%s'", filepath);
    return true;
}

} // namespace nanovdb_import
} // namespace pnanovdb_editor
