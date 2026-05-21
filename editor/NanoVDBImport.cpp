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

#define PNANOVDB_C
#include <nanovdb/PNanoVDB.h>
#undef PNANOVDB_C

namespace pnanovdb_editor
{
namespace nanovdb_import
{
namespace
{

pnanovdb_uint32_t required_blind_metadata_count(pnanovdb_pipeline_type_t render_pipeline)
{
    switch (render_pipeline)
    {
    case pnanovdb_pipeline_type_voxelbvh_render:
        return 8u; // gaussian: range, prim_id, mean, opacity, quat, scale, sh0, shn
    case pnanovdb_pipeline_type_voxelbvh_lines_render:
    case pnanovdb_pipeline_type_voxelbvh_triangles_render:
    case pnanovdb_pipeline_type_voxelbvh_triangles_debug_render:
        return 5u; // mesh: range, prim_id, indices, positions, colors
    default:
        return 0u; // nanovdb_render, voxelbvh_debug_render, etc.
    }
}

pnanovdb_uint32_t get_blind_metadata_count(const pnanovdb_compute_array_t* array)
{
    if (!array || !array->data || array->element_count == 0u || array->element_size == 0u)
    {
        return 0u;
    }
    const pnanovdb_uint64_t uint32_count = array->element_size * array->element_count / 4u;
    if (uint32_count == 0u)
    {
        return 0u;
    }
    pnanovdb_buf_t buf = pnanovdb_make_buf(static_cast<pnanovdb_uint32_t*>(array->data), uint32_count);
    pnanovdb_grid_handle_t grid = { pnanovdb_address_null() };
    return pnanovdb_grid_get_blind_metadata_count(buf, grid);
}

} // namespace

bool nanovdb(EditorScene& editor_scene,
             const pnanovdb_compute_t* compute,
             pnanovdb_editor_token_t* scene,
             const char* filepath,
             pnanovdb_pipeline_type_t render_pipeline)
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

    // fall back to standard NanoVDB rendering when the requested VoxelBVH variant
    // cannot be used (e.g. the file does not carry the required blind metadata).
    const pnanovdb_uint32_t required_metadata = required_blind_metadata_count(render_pipeline);
    if (required_metadata > 0u)
    {
        const pnanovdb_uint32_t metadata_count = get_blind_metadata_count(array);
        if (metadata_count < required_metadata)
        {
            Console::getInstance().addLog("'%s' has no VoxelBVH blind metadata (have %u, need %u); "
                                          "using standard NanoVDB render",
                                          filepath, metadata_count, required_metadata);
            render_pipeline = pnanovdb_pipeline_type_nanovdb_render;
        }
    }

    editor_scene.handle_nanovdb_data_load(scene, array, filepath, render_pipeline);
    Console::getInstance().addLog(
        "Loaded NanoVDB from '%s' (render_pipeline=%d)", filepath, static_cast<int>(render_pipeline));
    return true;
}

} // namespace nanovdb_import
} // namespace pnanovdb_editor
