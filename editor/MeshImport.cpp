// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/editor/MeshImport.cpp

    \author Petra Hapalova

    \brief
*/

#include "EditorImport.h"

#include "Console.h"
#include "Pipeline.h"
#include "PipelineRuntime.h"
#include "PipelineTypes.h"

#include "nanovdb_editor/putil/Compute.h"

namespace pnanovdb_editor
{
namespace mesh_import
{
bool mesh(const pnanovdb_compute_t* compute, pnanovdb_editor_token_t* scene, const char* filepath, const Options& options)
{
    if (!scene || !filepath || !compute)
        return false;

    pnanovdb_pipeline_params_t load_params{};
    pnanovdb_pipeline_get_default_params(pnanovdb_pipeline_type_mesh_load, &load_params);
    pipeline_params_set_mesh_load_inflation_radius(&load_params, options.inflation_radius);
    pipeline_params_set_mesh_load_resolution(&load_params, options.resolution);
    pipeline_params_set_mesh_load_show_debug(&load_params, options.show_debug);

    PipelineLoadRequest request;
    request.load_pipeline = pnanovdb_pipeline_type_mesh_load;
    request.process_pipeline = pnanovdb_pipeline_type_voxelbvh_build;
    request.render_pipeline = pnanovdb_pipeline_type_voxelbvh_triangles_render;
    request.source_filepath = filepath;
    request.load_params = &load_params;

    const bool started = pipeline_load(/*scene_manager*/ nullptr, scene, request);
    pipeline_params_release(&load_params);

    if (!started)
    {
        Console::getInstance().addLog(
            Console::LogLevel::Error, "Import Mesh: failed to start async load for '%s'", filepath);
    }
    return started;
}

} // namespace mesh_import
} // namespace pnanovdb_editor
