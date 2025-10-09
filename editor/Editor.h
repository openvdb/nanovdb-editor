// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/editor/Editor.h

    \author Petra Hapalova

    \brief
*/

#include "nanovdb_editor/putil/Reflect.h"
#include "nanovdb_editor/putil/Editor.h"

struct pnanovdb_editor_impl_t
{
    const pnanovdb_compiler_t* compiler;
    const pnanovdb_compute_t* compute;
    void* editor_worker;
    pnanovdb_compute_array_t* nanovdb_array;
    pnanovdb_compute_array_t* data_array;
    pnanovdb_raster_gaussian_data_t* gaussian_data;
    pnanovdb_camera_t* camera;
    pnanovdb_raster_context_t* raster_ctx;
    void* shader_params;
    const pnanovdb_reflect_data_type_t* shader_params_data_type;
    void* views;
};

namespace pnanovdb_editor
{
PNANOVDB_API pnanovdb_editor_t* pnanovdb_get_editor();
}
