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
    pnanovdb_pipeline_type_count
};
