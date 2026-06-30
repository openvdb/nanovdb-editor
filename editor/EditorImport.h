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
    pnanovdb_uint32_t resolution = k_default_bvh_resolution; //!< 1..k_max_bvh_resolution
    pnanovdb_pipeline_type_t process_pipeline = pnanovdb_pipeline_type_voxelbvh_build;
    bool replace_existing = false;
};

bool mesh(const pnanovdb_compute_t* compute,
          pnanovdb_editor_token_t* scene,
          const char* filepath,
          const Options& options = {},
          pnanovdb_editor_token_t* name = nullptr);

} // namespace mesh_import

namespace nanovdb_import
{

PNANOVDB_API bool has_voxelbvh_mesh_metadata(const pnanovdb_compute_array_t* array);
PNANOVDB_API bool has_voxelbvh_render_metadata(const pnanovdb_compute_array_t* array,
                                               pnanovdb_pipeline_type_t render_pipeline);

bool nanovdb(EditorScene& editor_scene,
             const pnanovdb_compute_t* compute,
             pnanovdb_editor_token_t* scene,
             const char* filepath,
             pnanovdb_pipeline_type_t render_pipeline = pnanovdb_pipeline_type_nanovdb_render,
             pnanovdb_editor_token_t* name = nullptr);

} // namespace nanovdb_import

namespace gaussian_import
{
enum class Mode : int
{
    Splat = 0,
    Voxelize = 1,
    VoxelBVH = 2,
};

bool gaussian(EditorScene& editor_scene,
              EditorSceneManager& scene_manager,
              const pnanovdb_compute_t* compute,
              pnanovdb_editor_token_t* scene,
              const char* filepath,
              pnanovdb_pipeline_type_t process_pipeline,
              pnanovdb_pipeline_type_t render_pipeline,
              float voxels_per_unit,
              pnanovdb_editor_token_t* name = nullptr,
              bool replace_existing = false);

} // namespace gaussian_import
} // namespace pnanovdb_editor
