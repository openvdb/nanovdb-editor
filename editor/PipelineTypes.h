// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   editor/PipelineTypes.h

    \brief  Pipeline type enum (internal; API has only opaque pnanovdb_pipeline_type_t).
*/

#pragma once

#include "nanovdb_editor/putil/Editor.h"

enum pnanovdb_pipeline_type_enum_t
{
    pnanovdb_pipeline_type_noop = 0,
    pnanovdb_pipeline_type_nanovdb_render = 1,
    pnanovdb_pipeline_type_raster2d = 2,
    pnanovdb_pipeline_type_raster3d = 3,
    pnanovdb_pipeline_type_voxelbvh_render = 4,
    pnanovdb_pipeline_type_voxelbvh_lines_render = 5,
    pnanovdb_pipeline_type_voxelbvh_triangles_render = 6,
    pnanovdb_pipeline_type_voxelbvh_triangle_lines_render = 7,
    pnanovdb_pipeline_type_voxelbvh_debug_render = 8,
    pnanovdb_pipeline_type_voxelbvh_build = 9,
    pnanovdb_pipeline_type_count
};
