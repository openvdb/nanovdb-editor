// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   editor/PipelineTypes.h

    \brief
*/

#pragma once

#include "nanovdb_editor/putil/Editor.h"

enum pnanovdb_pipeline_type_enum_t
{
    pnanovdb_pipeline_type_noop = 0, // no-op load stage (data added directly, no import)
    pnanovdb_pipeline_type_nanovdb_render = 1, // render: ray-march a NanoVDB grid
    pnanovdb_pipeline_type_gaussian_splat = 2, // render: 2D Gaussian splatting
    pnanovdb_pipeline_type_gaussian_voxelize = 3, // process: Gaussians -> NanoVDB
    pnanovdb_pipeline_type_voxelbvh_gaussians_render = 4, // render: VoxelBVH built from gaussians
    pnanovdb_pipeline_type_voxelbvh_lines_render = 5, // render: VoxelBVH as lines
    pnanovdb_pipeline_type_voxelbvh_triangles_render = 6, // render: VoxelBVH as triangles
    pnanovdb_pipeline_type_voxelbvh_triangles_debug_render = 7, // render: triangles, debug shading
    pnanovdb_pipeline_type_voxelbvh_debug_render = 8, // render: VoxelBVH, debug shading
    pnanovdb_pipeline_type_voxelbvh_build = 9, // process: build a VoxelBVH (from mesh/gaussians)
    pnanovdb_pipeline_type_mesh_load = 10, // load: read a PLY into compute arrays
    pnanovdb_pipeline_type_gaussian_load = 11, // load: import a Gaussian file -> data rendered by gaussian_splat
    pnanovdb_pipeline_type_nanovdb_surface = 12, // render: SDF/level-set isosurface via HDDA zero-crossing
    pnanovdb_pipeline_type_count
};

// ------------------------------------------------ Pipeline descriptors

typedef struct pnanovdb_pipeline_shader_entry_t
{
    const char* shader_name;
    const char* shader_group;
    pnanovdb_bool_t overridable;
} pnanovdb_pipeline_shader_entry_t;

typedef enum pnanovdb_pipeline_result_t
{
    pnanovdb_pipeline_result_success = 0,
    pnanovdb_pipeline_result_skipped = 1,
    pnanovdb_pipeline_result_no_data = 2,
    pnanovdb_pipeline_result_error = 3,
    pnanovdb_pipeline_result_pending = 4
} pnanovdb_pipeline_result_t;

typedef enum pnanovdb_pipeline_render_method_t
{
    pnanovdb_pipeline_render_method_none = 0,
    pnanovdb_pipeline_render_method_nanovdb = 1,
    pnanovdb_pipeline_render_method_gaussian = 2
} pnanovdb_pipeline_render_method_t;

typedef struct pnanovdb_pipeline_context_t pnanovdb_pipeline_context_t;

typedef void (*pnanovdb_pipeline_init_params_fn)(pnanovdb_pipeline_params_t* params);
typedef pnanovdb_pipeline_result_t (*pnanovdb_pipeline_execute_fn)(pnanovdb_scene_object_t* obj,
                                                                   pnanovdb_pipeline_context_t* ctx);
typedef pnanovdb_pipeline_render_method_t (*pnanovdb_pipeline_get_render_method_fn)(void);
typedef void* (*pnanovdb_pipeline_map_params_fn)(pnanovdb_scene_object_t* obj);

typedef struct pnanovdb_pipeline_param_field_t pnanovdb_pipeline_param_field_t;

typedef struct pnanovdb_pipeline_descriptor_t
{
    pnanovdb_pipeline_type_t type;
    pnanovdb_pipeline_stage_t stage;
    const char* name;

    const pnanovdb_pipeline_shader_entry_t* shaders;
    pnanovdb_uint32_t shader_count;

    pnanovdb_uint64_t params_size;
    const pnanovdb_reflect_data_type_t* params_data_type;

    pnanovdb_pipeline_init_params_fn init_params;
    pnanovdb_pipeline_execute_fn execute;
    pnanovdb_pipeline_get_render_method_fn get_render_method;
    pnanovdb_pipeline_map_params_fn map_params;

    const pnanovdb_pipeline_param_field_t* param_fields;
    pnanovdb_uint32_t param_field_count;
} pnanovdb_pipeline_descriptor_t;

namespace pnanovdb_editor
{
inline constexpr float k_default_voxels_per_unit = 128.f;
inline constexpr float k_default_voxel_size = 1.f / k_default_voxels_per_unit;
} // namespace pnanovdb_editor
