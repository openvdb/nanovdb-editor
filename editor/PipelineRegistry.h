// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   editor/PipelineRegistry.h

    \author Petra Hapalova

    \brief
*/

#pragma once

#include "PipelineTypes.h"

// ============================================================================
// Pipeline Registry
// ============================================================================

void pnanovdb_pipeline_register(const pnanovdb_pipeline_descriptor_t* descriptor);

// Exported (default visibility) so tests can enumerate the registered pipelines
// and their shaders without linking the whole editor.
PNANOVDB_API pnanovdb_uint32_t pnanovdb_pipeline_get_count(void);

PNANOVDB_API const char* pnanovdb_pipeline_get_shader_name(pnanovdb_pipeline_type_t type);
PNANOVDB_API const char* pnanovdb_pipeline_get_shader_group(pnanovdb_pipeline_type_t type);
PNANOVDB_API const pnanovdb_pipeline_descriptor_t* pnanovdb_pipeline_get_descriptor(pnanovdb_pipeline_type_t type);
void pnanovdb_pipeline_get_default_params(pnanovdb_pipeline_type_t type, pnanovdb_pipeline_params_t* params);

pnanovdb_pipeline_result_t pnanovdb_pipeline_execute(pnanovdb_pipeline_type_t type,
                                                     pnanovdb_scene_object_t* obj,
                                                     pnanovdb_pipeline_context_t* ctx);
pnanovdb_pipeline_render_method_t pnanovdb_pipeline_get_render_method(pnanovdb_pipeline_type_t type);

// ============================================================================
// Self-registration helper
// ============================================================================

struct PipelineRegistrar
{
    explicit PipelineRegistrar(const pnanovdb_pipeline_descriptor_t* desc)
    {
        pnanovdb_pipeline_register(desc);
    }
};

#define PNANOVDB_REGISTER_PIPELINE(desc_var) static const PipelineRegistrar desc_var##_registrar(&desc_var)
