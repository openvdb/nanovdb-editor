// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   editor/PipelineRegistry.cpp

    \author Petra Hapalova

    \brief
*/

#include "PipelineRegistry.h"

#include <array>
#include <cstdlib>
#include <cstring>
#include <mutex>

// ============================================================================
// Pipeline Registry storage
// ============================================================================

namespace
{
using PipelineRegistryStorage = std::array<const pnanovdb_pipeline_descriptor_t*, pnanovdb_pipeline_type_count>;

PipelineRegistryStorage& registry()
{
    static PipelineRegistryStorage s_registry = {};
    return s_registry;
}

std::mutex& registry_mutex()
{
    static std::mutex s_mutex;
    return s_mutex;
}

pnanovdb_uint32_t& pipeline_count()
{
    static pnanovdb_uint32_t s_count = 0;
    return s_count;
}
} // namespace

void pnanovdb_pipeline_register(const pnanovdb_pipeline_descriptor_t* descriptor)
{
    if (!descriptor || descriptor->type >= pnanovdb_pipeline_type_count)
    {
        return;
    }
    std::lock_guard<std::mutex> lock(registry_mutex());
    registry()[descriptor->type] = descriptor;
    pnanovdb_uint32_t count = 0;
    for (auto* d : registry())
    {
        if (d)
        {
            ++count;
        }
    }
    pipeline_count() = count;
}

pnanovdb_uint32_t pnanovdb_pipeline_get_count(void)
{
    std::lock_guard<std::mutex> lock(registry_mutex());
    return pipeline_count();
}

// ============================================================================
// Pipeline Registry Functions
// ============================================================================

const char* pnanovdb_pipeline_get_shader_name(pnanovdb_pipeline_type_t type)
{
    const auto* desc = pnanovdb_pipeline_get_descriptor(type);
    return (desc && desc->shader_count > 0) ? desc->shaders[0].shader_name : nullptr;
}

const char* pnanovdb_pipeline_get_shader_group(pnanovdb_pipeline_type_t type)
{
    const auto* desc = pnanovdb_pipeline_get_descriptor(type);
    return (desc && desc->shader_count > 0) ? desc->shaders[0].shader_group : nullptr;
}

const pnanovdb_pipeline_descriptor_t* pnanovdb_pipeline_get_descriptor(pnanovdb_pipeline_type_t type)
{
    if (type >= pnanovdb_pipeline_type_count)
        return nullptr;
    std::lock_guard<std::mutex> lock(registry_mutex());
    return registry()[type];
}

void pnanovdb_pipeline_get_default_params(pnanovdb_pipeline_type_t type, pnanovdb_pipeline_params_t* params)
{
    if (!params)
        return;
    free(params->data);
    memset(params, 0, sizeof(*params));

    const auto* desc = pnanovdb_pipeline_get_descriptor(type);
    if (desc && desc->init_params)
        desc->init_params(params);
}

pnanovdb_pipeline_result_t pnanovdb_pipeline_execute(pnanovdb_pipeline_type_t type,
                                                     pnanovdb_scene_object_t* obj,
                                                     pnanovdb_pipeline_context_t* ctx)
{
    const auto* desc = pnanovdb_pipeline_get_descriptor(type);
    if (!desc)
        return pnanovdb_pipeline_result_error;
    if (!desc->execute)
        return pnanovdb_pipeline_result_skipped;
    return desc->execute(obj, ctx);
}

pnanovdb_pipeline_render_method_t pnanovdb_pipeline_get_render_method(pnanovdb_pipeline_type_t type)
{
    const auto* desc = pnanovdb_pipeline_get_descriptor(type);
    if (!desc || !desc->get_render_method)
        return pnanovdb_pipeline_render_method_none;
    return desc->get_render_method();
}
