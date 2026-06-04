// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/editor/EditorImport.h

    \author Petra Hapalova

    \brief
*/

#pragma once

#include "nanovdb_editor/putil/Editor.h"
#include "PipelineTypes.h" // pnanovdb_pipeline_type_t

struct pnanovdb_compute_t;

namespace pnanovdb_editor
{
class EditorScene;
class EditorSceneManager;

namespace mesh_import
{

struct Options
{
    bool show_debug = false;
    float inflation_radius = 0.f; //!< 0 = auto for line-based renders
    pnanovdb_uint32_t resolution = 512u; //!< 1..4096
};

bool mesh(EditorScene& editor_scene,
          const pnanovdb_compute_t* compute,
          pnanovdb_editor_token_t* scene,
          const char* filepath,
          const Options& options = {});

} // namespace mesh_import

namespace nanovdb_import
{

bool nanovdb(EditorScene& editor_scene,
             const pnanovdb_compute_t* compute,
             pnanovdb_editor_token_t* scene,
             const char* filepath,
             pnanovdb_pipeline_type_t render_pipeline = pnanovdb_pipeline_type_nanovdb_render);

} // namespace nanovdb_import

namespace gaussian_import
{
enum class Mode : int
{
    Raster2D = 0,
    Raster3D = 1,
    VoxelBVH = 2,
};

bool gaussian(EditorScene& editor_scene,
              EditorSceneManager& scene_manager,
              const pnanovdb_compute_t* compute,
              pnanovdb_editor_token_t* scene,
              const char* filepath,
              pnanovdb_pipeline_type_t process_pipeline,
              pnanovdb_pipeline_type_t render_pipeline,
              float voxels_per_unit);

} // namespace gaussian_import
} // namespace pnanovdb_editor
