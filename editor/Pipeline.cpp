// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   editor/Pipeline.h

    \author Petra Hapalova, Andrew Reidmeyer

    \brief  This file provides pipeline interface and registration system.
*/

#include "Pipeline.h"
#include "Editor.h"
#include "EditorScene.h"
#include "EditorSceneManager.h"
#include "EditorToken.h"
#include "Renderer.h"
#include "Console.h"
#include "Profiler.h"
#include "nanovdb_editor/putil/Reflect.h"
#include "nanovdb_editor/putil/WorkerThread.hpp"
#include "raster/Raster.h"

#include <cstdlib>
#include <cstring>
#include <array>
#include <vector>
#include <algorithm>
#include <string>
#include <filesystem>
#include <mutex>
#include <memory>

struct Raster3DParams
{
    float voxels_per_unit = 128.f;
};

static void init_raster3d_params(pnanovdb_pipeline_params_t* params);
static float pnanovdb_pipeline_params_get_voxels_per_unit(pnanovdb_pipeline_params_t* params);
static void pnanovdb_pipeline_params_set_voxels_per_unit(pnanovdb_pipeline_params_t* params, float value);

// ============================================================================
// Pipeline Registry
// ============================================================================

static std::array<const pnanovdb_pipeline_descriptor_t*, pnanovdb_pipeline_type_count> s_pipeline_registry = {};
static pnanovdb_uint32_t s_pipeline_count = 0;

void pnanovdb_pipeline_register(const pnanovdb_pipeline_descriptor_t* descriptor)
{
    if (!descriptor || descriptor->type >= pnanovdb_pipeline_type_count)
    {
        return;
    }
    s_pipeline_registry[descriptor->type] = descriptor;
    s_pipeline_count = 0;
    for (auto* d : s_pipeline_registry)
    {
        if (d)
        {
            ++s_pipeline_count;
        }
    }
}

pnanovdb_uint32_t pnanovdb_pipeline_get_count(void)
{
    return s_pipeline_count;
}

// ============================================================================
// Raster3D Pipeline Conversion Worker
// ============================================================================

namespace
{
pnanovdb_compute_array_t** pipeline_get_shader_params_arrays();
pnanovdb_raster_shader_params_t* pipeline_get_init_raster_params();

// Worker thread for async Gaussian-to-NanoVDB conversion
static pnanovdb_util::WorkerThread s_conversion_worker;
static pnanovdb_util::WorkerThread::TaskId s_conversion_task_id = pnanovdb_util::WorkerThread::invalidTaskId();

// Pending conversion state
static pnanovdb_uint32_t s_pending_scene_token_id = 0;
static pnanovdb_uint32_t s_pending_name_token_id = 0;
static pnanovdb_editor::EditorSceneManager* s_pending_scene_manager = nullptr;
static pnanovdb_compute_array_t* s_pending_nanovdb_array = nullptr;
static const pnanovdb_compute_t* s_pending_compute = nullptr;
static std::string s_pending_conversion_filepath; // source filepath for re-conversion (kept alive for worker)
static bool s_conversion_enqueued = false; // set immediately on enqueue, cleared on completion handling
static pnanovdb_pipeline_params_t s_pending_conversion_params = {};

bool is_conversion_running()
{
    return s_conversion_enqueued || s_conversion_worker.isTaskRunning(s_conversion_task_id);
}

bool is_conversion_completed()
{
    return s_conversion_worker.isTaskCompleted(s_conversion_task_id);
}

bool start_conversion(pnanovdb_editor::SceneObject* scene_obj,
                      pnanovdb_editor::EditorSceneManager* scene_manager,
                      const pnanovdb_pipeline_context_t* ctx)
{
    if (!scene_obj || !scene_manager || !ctx || !ctx->raster || !ctx->compute)
    {
        pnanovdb_editor::Console::getInstance().addLog(pnanovdb_editor::Console::LogLevel::Error,
            "start_conversion: null pointer (obj=%p, mgr=%p, ctx=%p, raster=%p, compute=%p)",
            (void*)scene_obj, (void*)scene_manager, (void*)ctx,
            ctx ? (void*)ctx->raster : nullptr, ctx ? (void*)ctx->compute : nullptr);
        return false;
    }

    if (scene_obj->resources.source_filepath.empty())
    {
        pnanovdb_editor::Console::getInstance().addLog(pnanovdb_editor::Console::LogLevel::Error,
            "start_conversion: no source file path for re-conversion");
        return false;
    }

    if (s_conversion_worker.hasRunningTask())
    {
        pnanovdb_editor::Console::getInstance().addLog("Warning: Conversion already in progress, skipping");
        return false;
    }

    // Store token IDs for safe lookup after completion
    s_pending_scene_token_id = scene_obj->scene_token ? scene_obj->scene_token->id : 0;
    s_pending_name_token_id = scene_obj->name_token ? scene_obj->name_token->id : 0;
    s_pending_scene_manager = scene_manager;
    s_pending_compute = ctx->compute;
    s_pending_nanovdb_array = nullptr;

    // Deep-copy pipeline process params so the worker thread can read them safely
    {
        free(s_pending_conversion_params.data);
        s_pending_conversion_params = {};
        auto& src = scene_obj->pipeline.process().params;
        if (src.data && src.size > 0)
        {
            s_pending_conversion_params.data = malloc(src.size);
            memcpy(s_pending_conversion_params.data, src.data, src.size);
            s_pending_conversion_params.size = src.size;
            s_pending_conversion_params.type = src.type;
        }
    }

    s_pending_conversion_filepath = scene_obj->resources.source_filepath;
    s_conversion_enqueued = true;
    s_conversion_task_id = s_conversion_worker.enqueue(
        [&](pnanovdb_raster_t* raster, const pnanovdb_compute_t* compute, pnanovdb_compute_queue_t* queue,
            const char* filepath, pnanovdb_compute_array_t** out_nanovdb,
            pnanovdb_compute_array_t** shader_arrays, pnanovdb_raster_shader_params_t* raster_params) -> bool
        {
            float voxel_sz = 1.0f / pnanovdb_pipeline_params_get_voxels_per_unit(&s_pending_conversion_params);
            return raster->raster_file(raster, compute, queue, filepath, voxel_sz,
                                       out_nanovdb,      // NanoVDB output
                                       nullptr,          // gaussian_data (not needed)
                                       nullptr,          // raster_context (not needed)
                                       shader_arrays,    // shader_params_arrays (needed by raster_file)
                                       raster_params,    // raster_params
                                       nullptr,          // profiler
                                       (void*)&s_conversion_worker);
        },
        const_cast<pnanovdb_raster_t*>(ctx->raster), ctx->compute, ctx->compute_queue,
        s_pending_conversion_filepath.c_str(), &s_pending_nanovdb_array,
        pipeline_get_shader_params_arrays(), pipeline_get_init_raster_params());

    float vpu = pnanovdb_pipeline_params_get_voxels_per_unit(&s_pending_conversion_params);
    pnanovdb_editor::Console::getInstance().addLog(
        "Starting re-conversion from '%s' (voxels_per_unit=%.1f, voxel_size=%.6f)...",
        s_pending_conversion_filepath.c_str(), vpu, 1.0f / vpu);
    return true;
}

bool handle_conversion_completion()
{
    if (!is_conversion_completed())
        return false;

    bool success = s_conversion_worker.isTaskSuccessful(s_conversion_task_id);

    pnanovdb_editor_token_t* scene_token = pnanovdb_editor::EditorToken::getInstance().getTokenById(s_pending_scene_token_id);
    pnanovdb_editor_token_t* name_token = pnanovdb_editor::EditorToken::getInstance().getTokenById(s_pending_name_token_id);

    pnanovdb_editor::Console::getInstance().addLog(pnanovdb_editor::Console::LogLevel::Debug,
        "handle_conversion_completion: success=%d, scene_token=%p, name_token=%p ('%s'), "
        "scene_manager=%p, nanovdb_array=%p",
        (int)success, (void*)scene_token, (void*)name_token,
        (name_token && name_token->str) ? name_token->str : "<null>",
        (void*)s_pending_scene_manager, (void*)s_pending_nanovdb_array);

    if (success && scene_token && name_token && s_pending_scene_manager && s_pending_nanovdb_array)
    {
        bool object_found = false;
        s_pending_scene_manager->with_object(
            scene_token, name_token,
            [&](pnanovdb_editor::SceneObject* scene_obj)
            {
                if (!scene_obj)
                    return;

                object_found = true;

                // Store the converted NanoVDB in the scene object
                std::shared_ptr<pnanovdb_compute_array_t> owner(
                    s_pending_nanovdb_array,
                    [compute = s_pending_compute](pnanovdb_compute_array_t* arr) {
                        if (arr)
                            compute->destroy_array(arr);
                    });
                scene_obj->resources.nanovdb_array = s_pending_nanovdb_array;
                scene_obj->resources.converted_nanovdb = s_pending_nanovdb_array;
                scene_obj->resources.nanovdb_array_owner = owner;
                scene_obj->resources.converted_nanovdb_owner = owner;

                // Switch to NanoVDB rendering and initialize render params for the new pipeline
                scene_obj->pipeline.render().type = pnanovdb_pipeline_type_nanovdb_render;
                pnanovdb_pipeline_get_default_params(pnanovdb_pipeline_type_nanovdb_render,
                                                     &scene_obj->pipeline.render().params);

                // Only clear dirty if params haven't changed since conversion started
                float used_vpu = pnanovdb_pipeline_params_get_voxels_per_unit(&s_pending_conversion_params);
                float current_vpu = pnanovdb_pipeline_params_get_voxels_per_unit(
                    &scene_obj->pipeline.process().params);
                bool params_changed = (current_vpu != used_vpu);
                if (!params_changed)
                    scene_obj->pipeline.process().dirty = false;
                pnanovdb_editor::Console::getInstance().addLog(
                    pnanovdb_editor::Console::LogLevel::Debug,
                    "Conversion complete: used_vpu=%.1f, current_vpu=%.1f, dirty=%s",
                    used_vpu, current_vpu,
                    params_changed ? "true (re-convert)" : "false");

                scene_obj->params.shader_name.shader_name = nullptr;
                scene_obj->params.shader_params = nullptr;
                scene_obj->params.shader_params_data_type = nullptr;
            });

        if (object_found)
        {
            pnanovdb_editor::Console::getInstance().addLog("Gaussian->NanoVDB conversion complete");
        }
        else
        {
            // Object was removed during conversion - clean up the orphaned array
            if (s_pending_compute && s_pending_nanovdb_array)
            {
                s_pending_compute->destroy_array(s_pending_nanovdb_array);
            }
            pnanovdb_editor::Console::getInstance().addLog(
                pnanovdb_editor::Console::LogLevel::Warning,
                "Conversion completed but scene object was removed");
        }
    }
    else if (success && !s_pending_nanovdb_array)
    {
        pnanovdb_editor::Console::getInstance().addLog(
            pnanovdb_editor::Console::LogLevel::Error,
            "Conversion succeeded but produced no NanoVDB array (raster_file returned true with null output)");
    }
    else if (!success)
    {
        pnanovdb_editor::Console::getInstance().addLog(
            pnanovdb_editor::Console::LogLevel::Error, "Gaussian->NanoVDB conversion failed");

        if (scene_token && name_token && s_pending_scene_manager)
        {
            s_pending_scene_manager->with_object(
                scene_token, name_token,
                [](pnanovdb_editor::SceneObject* scene_obj)
                {
                    if (scene_obj)
                        scene_obj->pipeline.process().dirty = false;
                });
        }
    }

    // Clear state
    s_pending_scene_token_id = 0;
    s_pending_name_token_id = 0;
    s_pending_scene_manager = nullptr;
    s_pending_nanovdb_array = nullptr;
    s_pending_compute = nullptr;
    free(s_pending_conversion_params.data);
    s_pending_conversion_params = {};
    s_conversion_enqueued = false;
    s_conversion_worker.removeCompletedTask(s_conversion_task_id);

    return true;
}

bool get_conversion_progress(std::string& text, float& value)
{
    if (s_conversion_worker.isTaskRunning(s_conversion_task_id))
    {
        value = s_conversion_worker.getTaskProgress(s_conversion_task_id);
        text = s_conversion_worker.getTaskProgressText(s_conversion_task_id);
        return true;
    }

    // Task is enqueued but not yet started executing on the worker thread
    if (s_conversion_enqueued)
    {
        value = 0.0f;
        text = "Waiting for worker...";
        return true;
    }

    return false;
}

// ============================================================================
// Rasterization Worker (file import -> Gaussian/NanoVDB)
// ============================================================================

static pnanovdb_util::WorkerThread s_raster_worker;
static pnanovdb_util::WorkerThread::TaskId s_raster_task_id = pnanovdb_util::WorkerThread::invalidTaskId();

// Stored config (captured at init or start time)
static const pnanovdb_compute_t* s_raster_compute = nullptr;
static pnanovdb_raster_t* s_raster_raster = nullptr;
static pnanovdb_compute_queue_t* s_raster_device_queue = nullptr;
static pnanovdb_compute_queue_t* s_raster_compute_queue = nullptr;

// Pending rasterization state
static std::string s_pending_raster_filepath;
static float s_pending_voxel_size = 1.f / 128.f;
static pnanovdb_raster_gaussian_data_t* s_pending_gaussian_data = nullptr;
static pnanovdb_raster_context_t* s_pending_raster_ctx = nullptr;
static pnanovdb_raster_shader_params_t* s_pending_raster_params = nullptr;
static pnanovdb_compute_array_t* s_pending_raster_nanovdb_array = nullptr;
static pnanovdb_editor_token_t* s_pending_raster_scene_token = nullptr;

// Pipeline config for pending import
static pnanovdb_pipeline_type_t s_pending_process_pipeline = pnanovdb_pipeline_type_noop;
static pnanovdb_pipeline_type_t s_pending_render_pipeline = pnanovdb_pipeline_type_raster2d;
static pnanovdb_pipeline_params_t s_pending_process_params = {};
static pnanovdb_raster_shader_params_t s_init_raster_shader_params;
static const pnanovdb_reflect_data_type_t* s_raster_shader_params_data_type = nullptr;

static pnanovdb_compute_array_t* s_pending_shader_params_arrays[pnanovdb_raster::shader_param_count] = {};

pnanovdb_compute_array_t** pipeline_get_shader_params_arrays()
{
    return s_pending_shader_params_arrays;
}

pnanovdb_raster_shader_params_t* pipeline_get_init_raster_params()
{
    return &s_init_raster_shader_params;
}

bool is_rasterization_running()
{
    return s_raster_worker.isTaskRunning(s_raster_task_id);
}

bool is_rasterization_completed()
{
    return s_raster_worker.isTaskCompleted(s_raster_task_id);
}

bool get_raster_task_progress(std::string& text, float& value)
{
    if (s_raster_worker.isTaskRunning(s_raster_task_id))
    {
        text = s_raster_worker.getTaskProgressText(s_raster_task_id);
        value = s_raster_worker.getTaskProgress(s_raster_task_id);
        return true;
    }
    return false;
}

void update_raster_shader_params(pnanovdb_editor::EditorSceneManager* scene_manager)
{
    if (!s_raster_compute || !scene_manager)
    {
        return;
    }
    s_raster_compute->destroy_array(s_pending_shader_params_arrays[pnanovdb_raster::gaussian_frag_color_slang]);
    s_pending_shader_params_arrays[pnanovdb_raster::gaussian_frag_color_slang] =
        scene_manager->shader_params.get_compute_array_for_shader("raster/gaussian_frag_color.slang", s_raster_compute);
}

} // anonymous namespace

// ============================================================================
// Pipeline Execute Functions (one per pipeline type)
// ============================================================================

using namespace pnanovdb_editor;

static pnanovdb_pipeline_result_t execute_noop(pnanovdb_scene_object_t* obj, pnanovdb_pipeline_context_t*)
{
    auto* scene_obj = cast(obj);
    if (scene_obj)
        scene_obj->pipeline.process().dirty = false;
    return pnanovdb_pipeline_result_skipped;
}

static pnanovdb_pipeline_result_t execute_nanovdb_render(pnanovdb_scene_object_t* obj, pnanovdb_pipeline_context_t*)
{
    auto* scene_obj = cast(obj);
    if (!scene_obj || !scene_obj->resources.nanovdb_array)
        return pnanovdb_pipeline_result_no_data;
    scene_obj->pipeline.process().dirty = false;
    return pnanovdb_pipeline_result_success;
}

static pnanovdb_pipeline_result_t execute_raster2d(pnanovdb_scene_object_t* obj, pnanovdb_pipeline_context_t*)
{
    auto* scene_obj = cast(obj);
    if (!scene_obj || !scene_obj->resources.gaussian_data)
        return pnanovdb_pipeline_result_no_data;
    scene_obj->pipeline.process().dirty = false;
    return pnanovdb_pipeline_result_success;
}

static pnanovdb_pipeline_result_t execute_raster3d(pnanovdb_scene_object_t* obj, pnanovdb_pipeline_context_t* ctx)
{
    auto* scene_obj = cast(obj);
    if (!scene_obj || scene_obj->resources.source_filepath.empty())
    {
        Console::getInstance().addLog(Console::LogLevel::Debug,
            "execute_raster3d: early exit (scene_obj=%p, source_filepath='%s')",
            (void*)scene_obj,
            scene_obj ? scene_obj->resources.source_filepath.c_str() : "<null>");
        return pnanovdb_pipeline_result_no_data;
    }

    auto* scene_manager = cast(ctx->scene_manager);

    if (!scene_manager)
    {
        Console::getInstance().addLog(
            Console::LogLevel::Error, "Raster3D processing failed: missing scene_manager");
        return pnanovdb_pipeline_result_error;
    }

    // Ensure process params are allocated (may be missing if pipeline type was changed after creation)
    auto& process_params = scene_obj->pipeline.process().params;
    if (!process_params.data || process_params.size < sizeof(Raster3DParams))
    {
        Console::getInstance().addLog(Console::LogLevel::Debug,
            "execute_raster3d: initializing missing Raster3DParams");
        free(process_params.data);
        process_params.data = nullptr;
        process_params.size = 0;
        init_raster3d_params(&process_params);
    }

    // Check if conversion is already running
    if (is_conversion_running())
    {
        float running_vpu = pnanovdb_pipeline_params_get_voxels_per_unit(&s_pending_conversion_params);
        float requested_vpu = pnanovdb_pipeline_params_get_voxels_per_unit(&process_params);
        if (requested_vpu != running_vpu)
        {
            Console::getInstance().addLog(Console::LogLevel::Debug,
                "Raster3D: conversion running (vpu=%.1f), will re-convert with vpu=%.1f when done",
                running_vpu, requested_vpu);
        }
        return pnanovdb_pipeline_result_pending;
    }

    float vpu = pnanovdb_pipeline_params_get_voxels_per_unit(&process_params);
    Console::getInstance().addLog("Starting Gaussian->NanoVDB conversion (voxels_per_unit=%.1f)...", vpu);

    // Start the conversion
    if (start_conversion(scene_obj, scene_manager, ctx))
        return pnanovdb_pipeline_result_pending;

    // start_conversion failed - do NOT clear dirty so it can retry next frame
    Console::getInstance().addLog(Console::LogLevel::Error,
        "execute_raster3d: start_conversion failed (keeping dirty for retry)");
    return pnanovdb_pipeline_result_pending;
}

// ============================================================================
// Built-in Pipeline Parameter Types (internal - used with generic param system)
// ============================================================================

// NanoVDB render pipeline params
struct NanoVDBRenderParams
{
    const char* shader_name_override = nullptr;
};

// Raster3DParams defined at top of file (forward declared for execute_raster3d)

// ============================================================================
// Pipeline Init Params Functions
// ============================================================================

static void init_nanovdb_render_params(pnanovdb_pipeline_params_t* params)
{
    params->size = sizeof(NanoVDBRenderParams);
    params->type = nullptr;
    params->data = malloc(sizeof(NanoVDBRenderParams));
    if (params->data)
        *static_cast<NanoVDBRenderParams*>(params->data) = NanoVDBRenderParams{};
}

static void init_raster3d_params(pnanovdb_pipeline_params_t* params)
{
    params->size = sizeof(Raster3DParams);
    params->type = nullptr;
    params->data = malloc(sizeof(Raster3DParams));
    if (params->data)
        *static_cast<Raster3DParams*>(params->data) = Raster3DParams{};
}

// ============================================================================
// Pipeline Render Method Functions
// ============================================================================

static pnanovdb_pipeline_render_method_t get_render_method_none(void)
{
    return pnanovdb_pipeline_render_method_none;
}
static pnanovdb_pipeline_render_method_t get_render_method_nanovdb(void)
{
    return pnanovdb_pipeline_render_method_nanovdb;
}
static pnanovdb_pipeline_render_method_t get_render_method_raster2d(void)
{
    return pnanovdb_pipeline_render_method_raster2d;
}

// ============================================================================
// Pipeline Map Params Functions
// ============================================================================

static void* map_nanovdb_render_params(pnanovdb_scene_object_t* obj)
{
    auto* scene_obj = cast(obj);
    return scene_obj ? scene_obj->pipeline.render().params.data : nullptr;
}

static void* map_raster3d_params(pnanovdb_scene_object_t* obj)
{
    auto* scene_obj = cast(obj);
    return scene_obj ? scene_obj->pipeline.process().params.data : nullptr;
}

// ============================================================================
// Built-in Pipeline Definitions
// ============================================================================

PNANOVDB_DEFINE_PIPELINE_SHADERS(s_nanovdb_render_shaders,
                                 PNANOVDB_PIPELINE_SHADER("editor/editor.slang", nullptr, PNANOVDB_TRUE));

PNANOVDB_DEFINE_PIPELINE_SHADERS(s_raster2d_shaders,
                                 PNANOVDB_PIPELINE_SHADER("raster/gaussian_rasterize_2d.slang",
                                                          "raster/raster2d_group",
                                                          PNANOVDB_FALSE));

PNANOVDB_DEFINE_PIPELINE_SHADERS(s_raster3d_shaders,
                                 PNANOVDB_PIPELINE_SHADER("raster/gaussian_frag_color.slang", nullptr, PNANOVDB_TRUE));

// Field descriptors for Raster3DParams (voxels_per_unit)
static const pnanovdb_pipeline_param_field_t s_raster3d_param_fields[] = {
    { "Voxels/Unit",
      "Higher = finer detail, more memory",
      PNANOVDB_REFLECT_TYPE_FLOAT,
      offsetof(Raster3DParams, voxels_per_unit),
      128.0f, 1.0f, 512.0f, 1.0f }
};

// Complete pipeline descriptors with all function pointers
static const pnanovdb_pipeline_descriptor_t s_noop_descriptor = {
    pnanovdb_pipeline_type_noop,
    pnanovdb_pipeline_stage_load,
    "No Operation",
    nullptr,
    0, // shaders
    0,
    nullptr, // params_size, params_type_name
    nullptr, // init_params
    execute_noop, // execute
    get_render_method_none, // get_render_method
    nullptr, // map_params
    nullptr, 0 // param_fields
};

static const pnanovdb_pipeline_descriptor_t s_nanovdb_render_descriptor = {
    pnanovdb_pipeline_type_nanovdb_render,
    pnanovdb_pipeline_stage_render,
    "NanoVDB Render",
    s_nanovdb_render_shaders,
    1,
    sizeof(NanoVDBRenderParams),
    "NanoVDBRenderParams",
    init_nanovdb_render_params,
    execute_nanovdb_render,
    get_render_method_nanovdb,
    map_nanovdb_render_params,
    nullptr, 0 // param_fields
};

static const pnanovdb_pipeline_descriptor_t s_raster2d_descriptor = {
    pnanovdb_pipeline_type_raster2d,
    pnanovdb_pipeline_stage_render,
    "Gaussian 2D Splatting",
    s_raster2d_shaders,
    1,
    0,
    nullptr, // params come from shader JSON
    nullptr,
    execute_raster2d,
    get_render_method_raster2d,
    nullptr,
    nullptr, 0 // param_fields
};

static const pnanovdb_pipeline_descriptor_t s_raster3d_descriptor = {
    pnanovdb_pipeline_type_raster3d,
    pnanovdb_pipeline_stage_process,
    "Gaussian to NanoVDB",
    s_raster3d_shaders,
    1,
    sizeof(Raster3DParams),
    "Raster3DParams",
    init_raster3d_params,
    execute_raster3d,
    get_render_method_nanovdb, // raster3d converts to nanovdb, then renders as nanovdb
    map_raster3d_params,
    s_raster3d_param_fields,
    sizeof(s_raster3d_param_fields) / sizeof(s_raster3d_param_fields[0])
};

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
    return s_pipeline_registry[type];
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

// ============================================================================
// C API - Scene Object Pipeline Operations
// ============================================================================

void pnanovdb_scene_object_set_pipeline(pnanovdb_scene_object_t* obj,
                                        pnanovdb_pipeline_stage_t stage,
                                        pnanovdb_pipeline_type_t type)
{
    auto* scene_obj = cast(obj);
    if (!scene_obj || stage >= pnanovdb_pipeline_stage_count)
        return;

    scene_obj->pipeline.stages[stage].type = type;
}

pnanovdb_pipeline_type_t pnanovdb_scene_object_get_pipeline(pnanovdb_scene_object_t* obj, pnanovdb_pipeline_stage_t stage)
{
    auto* scene_obj = cast(obj);
    if (!scene_obj || stage >= pnanovdb_pipeline_stage_count)
        return pnanovdb_pipeline_type_noop;

    return scene_obj->pipeline.stages[stage].type;
}

void pnanovdb_scene_object_mark_dirty(pnanovdb_scene_object_t* obj)
{
    auto* scene_obj = cast(obj);
    if (scene_obj)
        scene_obj->pipeline.process().dirty = true;
}

void* pnanovdb_scene_object_map_params(pnanovdb_scene_object_t* obj, const char* param_type_name)
{
    if (!obj || !param_type_name)
        return nullptr;

    // Look up pipeline by params type name and use its map_params function
    for (size_t i = 0; i < pnanovdb_pipeline_type_count; ++i)
    {
        const auto* desc = s_pipeline_registry[i];
        if (desc && desc->params_type_name && desc->map_params)
        {
            if (strcmp(desc->params_type_name, param_type_name) == 0)
                return desc->map_params(obj);
        }
    }
    return nullptr;
}

void pnanovdb_scene_object_unmap_params(pnanovdb_scene_object_t* obj, const char* param_type_name)
{
    // Currently no-op - params stay mapped
    (void)obj;
    (void)param_type_name;
}

pnanovdb_uint32_t pnanovdb_scene_object_get_pipeline_stage_shader_count(pnanovdb_scene_object_t* obj,
                                                                        pnanovdb_pipeline_stage_t stage)
{
    auto* scene_obj = cast(obj);
    if (!scene_obj || stage >= pnanovdb_pipeline_stage_count)
        return 0;

    pnanovdb_pipeline_type_t type = scene_obj->pipeline.stages[stage].type;
    const auto* desc = pnanovdb_pipeline_get_descriptor(type);
    return desc ? desc->shader_count : 0;
}

void pnanovdb_scene_object_set_pipeline_stage_shader(pnanovdb_scene_object_t* obj,
                                                     pnanovdb_pipeline_stage_t stage,
                                                     pnanovdb_uint32_t shader_idx,
                                                     const char* override_shader)
{
    auto* scene_obj = cast(obj);
    if (!scene_obj || !override_shader || stage >= pnanovdb_pipeline_stage_count)
        return;

    auto& stage_ref = scene_obj->pipeline.stages[stage];

    // Ensure shader_overrides vector is large enough
    if (shader_idx >= stage_ref.shader_overrides.size())
        stage_ref.shader_overrides.resize(shader_idx + 1);

    // Store override in the per-stage shader overrides
    stage_ref.shader_overrides[shader_idx].shader_name = override_shader;
}

const char* pnanovdb_scene_object_get_pipeline_stage_shader(pnanovdb_scene_object_t* obj,
                                                            pnanovdb_pipeline_stage_t stage,
                                                            pnanovdb_uint32_t shader_idx)
{
    auto* scene_obj = cast(obj);
    if (!scene_obj || stage >= pnanovdb_pipeline_stage_count)
        return nullptr;

    auto& stage_ref = scene_obj->pipeline.stages[stage];

    // Check for override first
    if (shader_idx < stage_ref.shader_overrides.size() && stage_ref.shader_overrides[shader_idx].has_shader_override())
    {
        return stage_ref.shader_overrides[shader_idx].shader_name.c_str();
    }

    // Fall back to pipeline descriptor default
    const auto* desc = pnanovdb_pipeline_get_descriptor(stage_ref.type);
    if (desc && shader_idx < desc->shader_count && desc->shaders)
        return desc->shaders[shader_idx].shader_name;

    return nullptr;
}

pnanovdb_bool_t pnanovdb_scene_object_map_shader_params(pnanovdb_scene_object_t* obj,
                                                        pnanovdb_uint32_t shader_idx,
                                                        pnanovdb_shader_params_desc_t* out_desc)
{
    auto* scene_obj = cast(obj);
    if (!scene_obj || !out_desc)
        return PNANOVDB_FALSE;

    const auto* dt = scene_obj->params.shader_params_data_type;
    if (!dt || !scene_obj->params.shader_params)
        return PNANOVDB_FALSE;

    out_desc->data = scene_obj->params.shader_params;
    out_desc->data_size = dt->element_size;
    out_desc->shader_name = dt->struct_typename;
    out_desc->element_names = nullptr; // TODO: populate from reflection
    out_desc->element_typenames = nullptr; // TODO: populate from reflection
    out_desc->element_offsets = nullptr; // TODO: populate from reflection
    out_desc->element_count = dt->child_reflect_data_count;

    return PNANOVDB_TRUE;
}

void pnanovdb_scene_object_unmap_shader_params(pnanovdb_scene_object_t* obj, pnanovdb_uint32_t shader_idx)
{
    // No-op
    (void)obj;
    (void)shader_idx;
}

const pnanovdb_reflect_data_type_t* pnanovdb_scene_object_get_shader_params_type(pnanovdb_scene_object_t* obj,
                                                                                 pnanovdb_uint32_t shader_idx)
{
    auto* scene_obj = cast(obj);
    if (!scene_obj)
        return nullptr;
    return scene_obj->params.shader_params_data_type;
}

// ============================================================================
// Pipeline Parameter Accessors (internal - used by start_conversion / handle_conversion_completion)
// ============================================================================

static float pnanovdb_pipeline_params_get_voxels_per_unit(pnanovdb_pipeline_params_t* params)
{
    if (!params || !params->data || params->size < sizeof(Raster3DParams))
    {
        return 128.0f; // default
    }
    return static_cast<Raster3DParams*>(params->data)->voxels_per_unit;
}

static void pnanovdb_pipeline_params_set_voxels_per_unit(pnanovdb_pipeline_params_t* params, float value)
{
    if (!params)
    {
        return;
    }
    if (!params->data || params->size < sizeof(Raster3DParams))
    {
        free(params->data);
        params->data = nullptr;
        params->size = 0;
        init_raster3d_params(params);
        if (!params->data)
        {
            return;
        }
    }
    static_cast<Raster3DParams*>(params->data)->voxels_per_unit = value;
}

// ============================================================================
// Shader Parameter Provider System Implementation
// ============================================================================

namespace
{

struct RegisteredProvider
{
    pnanovdb_shader_param_provider_t provider;
    uint32_t priority;
    uint32_t handle;
    bool active;
};

constexpr size_t MAX_PROVIDERS = 16;
static std::array<RegisteredProvider, MAX_PROVIDERS> s_providers = {};
static uint32_t s_next_provider_handle = 1;
static std::mutex s_provider_mutex;

} // namespace

pnanovdb_uint32_t pnanovdb_shader_param_provider_register(const pnanovdb_shader_param_provider_t* provider,
                                                          pnanovdb_uint32_t priority)
{
    if (!provider || !provider->get_value)
        return 0;

    std::lock_guard<std::mutex> lock(s_provider_mutex);

    // Find empty slot
    for (size_t i = 0; i < MAX_PROVIDERS; ++i)
    {
        if (!s_providers[i].active)
        {
            s_providers[i].provider = *provider;
            s_providers[i].priority = priority;
            s_providers[i].handle = s_next_provider_handle++;
            s_providers[i].active = true;
            return s_providers[i].handle;
        }
    }

    Console::getInstance().addLog(
        Console::LogLevel::Warning, "Max shader parameter providers reached (%zu)", MAX_PROVIDERS);
    return 0;
}

void pnanovdb_shader_param_provider_unregister(pnanovdb_uint32_t handle)
{
    if (handle == 0)
        return;

    std::lock_guard<std::mutex> lock(s_provider_mutex);

    for (size_t i = 0; i < MAX_PROVIDERS; ++i)
    {
        if (s_providers[i].active && s_providers[i].handle == handle)
        {
            s_providers[i].active = false;
            s_providers[i].handle = 0;
            return;
        }
    }
}

pnanovdb_bool_t pnanovdb_shader_param_get(const char* shader_name,
                                          const char* param_name,
                                          pnanovdb_shader_param_value_t* out_value)
{
    if (!shader_name || !param_name || !out_value)
        return PNANOVDB_FALSE;

    // Initialize output
    out_value->data = nullptr;
    out_value->size = 0;
    out_value->element_count = 0;
    out_value->type = 0;

    std::lock_guard<std::mutex> lock(s_provider_mutex);

    // Build list of active providers sorted by priority (descending)
    std::vector<RegisteredProvider*> active;
    for (size_t i = 0; i < MAX_PROVIDERS; ++i)
    {
        if (s_providers[i].active)
            active.push_back(&s_providers[i]);
    }

    std::sort(active.begin(), active.end(), [](const RegisteredProvider* a, const RegisteredProvider* b)
              { return a->priority > b->priority; });

    // Query providers in priority order
    for (auto* p : active)
    {
        if (p->provider.get_value(p->provider.ctx, shader_name, param_name, out_value))
            return PNANOVDB_TRUE;
    }

    return PNANOVDB_FALSE;
}


// ============================================================================
// C++ Implementation
// ============================================================================

namespace pnanovdb_editor
{

// ============================================================================
// Pipeline Registration
// ============================================================================

void pipeline_register_builtins()
{
    pnanovdb_pipeline_register(&s_noop_descriptor);
    pnanovdb_pipeline_register(&s_nanovdb_render_descriptor);
    pnanovdb_pipeline_register(&s_raster2d_descriptor);
    pnanovdb_pipeline_register(&s_raster3d_descriptor);
}

// ============================================================================
// C++ Pipeline Functions (using registry)
// ============================================================================

pnanovdb_pipeline_render_method_t pipeline_get_render_method(pnanovdb_pipeline_type_t render_pipeline)
{
    return pnanovdb_pipeline_get_render_method(render_pipeline);
}

pnanovdb_pipeline_result_t pipeline_execute_process(SceneObject* obj, const PipelineContext& ctx)
{
    if (!obj)
        return pnanovdb_pipeline_result_error;
    if (!obj->pipeline.process().dirty)
        return pnanovdb_pipeline_result_skipped;

    pnanovdb_pipeline_context_t pipeline_ctx = { ctx.compute,       ctx.device,  ctx.queue,
                                                 ctx.compute_queue, ctx.raster,  ctx.raster_ctx,
                                                 cast(ctx.renderer), cast(ctx.scene_manager) };
    return pnanovdb_pipeline_execute(obj->pipeline.process().type, cast(obj), &pipeline_ctx);
}

bool pipeline_needs_process(SceneObject* obj)
{
    return obj && obj->pipeline.process().dirty && obj->pipeline.process().type != pnanovdb_pipeline_type_noop;
}

void pipeline_mark_dirty(SceneObject* obj)
{
    if (obj)
    {
        obj->pipeline.process().dirty = true;
    }
}

void pipeline_execute_pending(EditorSceneManager* manager, const PipelineContext& ctx)
{
    if (!manager)
    {
        return;
    }

    manager->for_each_object(
        [&ctx](SceneObject* obj)
        {
            if (pipeline_needs_process(obj))
            {
                auto result = pipeline_execute_process(obj, ctx);
                if (result == pnanovdb_pipeline_result_success || result == pnanovdb_pipeline_result_skipped)
                    obj->pipeline.process().dirty = false;
            }
            return true;
        });
}

const char* pipeline_get_shader(const SceneObject* obj)
{
    if (!obj)
    {
        return nullptr;
    }
    
    // Priority 1: Check obj->params.shader_name (user-set via Properties panel)
    if (obj->params.shader_name.shader_name && obj->params.shader_name.shader_name->str &&
        obj->params.shader_name.shader_name->str[0] != '\0')
    {
        return obj->params.shader_name.shader_name->str;
    }
    
    // Priority 2: Check render stage shader overrides
    const auto& render_stage = obj->pipeline.render();
    if (!render_stage.shader_overrides.empty() && render_stage.shader_overrides[0].has_shader_override())
    {
        return render_stage.shader_overrides[0].shader_name.c_str();
    }
    
    // Priority 3: Fall back to pipeline's default shader
    return pnanovdb_pipeline_get_shader_name(render_stage.type);
}

// ============================================================================
// Rasterization Functions (moved from Renderer)
// ============================================================================

void pipeline_init_rasterizer(const PipelineContext& ctx)
{
    s_raster_compute = ctx.compute;
    s_raster_raster = ctx.raster;
    s_raster_device_queue = ctx.queue;
    s_raster_compute_queue = ctx.compute_queue;

    // Initialize shader params arrays
    for (pnanovdb_uint32_t idx = 0u; idx < pnanovdb_raster::shader_param_count; idx++)
    {
        s_pending_shader_params_arrays[idx] = nullptr;
    }

    // Initialize raster shader params data type
    s_raster_shader_params_data_type = PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_raster_shader_params_t);
    const pnanovdb_raster_shader_params_t* default_raster_shader_params =
        (const pnanovdb_raster_shader_params_t*)s_raster_shader_params_data_type->default_value;
    s_init_raster_shader_params = *default_raster_shader_params;
}

bool pipeline_start_rasterization(const char* raster_filepath,
                                  float voxels_per_unit,
                                  bool rasterize_to_nanovdb,
                                  pnanovdb_pipeline_type_t process_pipeline,
                                  pnanovdb_pipeline_type_t render_pipeline,
                                  EditorScene* editor_scene,
                                  EditorSceneManager* scene_manager,
                                  pnanovdb_editor_token_t* scene_token,
                                  const PipelineContext& ctx)
{
    if (!s_raster_raster || !raster_filepath)
    {
        return false;
    }

    if (s_raster_worker.hasRunningTask())
    {
        Console::getInstance().addLog("Error: Rasterization already in progress");
        return false;
    }

    pipeline_set_pending_pipelines(process_pipeline, render_pipeline, voxels_per_unit);

    s_pending_raster_filepath = raster_filepath;
    s_pending_raster_scene_token = scene_token;
    s_pending_voxel_size = 1.f / voxels_per_unit;

    update_raster_shader_params(scene_manager);

    s_pending_raster_params = &s_init_raster_shader_params;
    s_pending_raster_params->name = nullptr;
    s_pending_raster_params->data_type = s_raster_shader_params_data_type;

    bool needs_compute_queue = rasterize_to_nanovdb ||
                               (s_pending_process_pipeline == pnanovdb_pipeline_type_raster3d);
    pnanovdb_compute_queue_t* worker_queue = needs_compute_queue ? s_raster_compute_queue : s_raster_device_queue;

    s_raster_task_id = s_raster_worker.enqueue(
        [&](pnanovdb_raster_t* raster, const pnanovdb_compute_t* compute, pnanovdb_compute_queue_t* queue,
            const char* filepath, float voxel_size, pnanovdb_compute_array_t** nanovdb_array,
            pnanovdb_raster_gaussian_data_t** gaussian_data, pnanovdb_raster_context_t** raster_context,
            pnanovdb_compute_array_t** shader_params_arrays, pnanovdb_raster_shader_params_t* raster_params,
            pnanovdb_profiler_report_t profiler) -> bool
        {
            return raster->raster_file(raster, compute, queue, filepath, voxel_size, nanovdb_array, gaussian_data,
                                       raster_context, shader_params_arrays, raster_params, profiler,
                                       (void*)(&s_raster_worker));
        },
        s_raster_raster, s_raster_compute, worker_queue, s_pending_raster_filepath.c_str(), s_pending_voxel_size,
        rasterize_to_nanovdb ? &s_pending_raster_nanovdb_array : nullptr,
        rasterize_to_nanovdb ? nullptr : &s_pending_gaussian_data,
        rasterize_to_nanovdb ? nullptr : &s_pending_raster_ctx,
        s_pending_shader_params_arrays, s_pending_raster_params, pnanovdb_editor::Profiler::report_callback);

    Console::getInstance().addLog("Running rasterization: '%s'...", s_pending_raster_filepath.c_str());
    return true;
}

void pipeline_set_pending_pipelines(pnanovdb_pipeline_type_t process, pnanovdb_pipeline_type_t render, float voxels_per_unit)
{
    s_pending_process_pipeline = process;
    s_pending_render_pipeline = render;

    pnanovdb_pipeline_get_default_params(process, &s_pending_process_params);
    if (process == pnanovdb_pipeline_type_raster3d)
    {
        pnanovdb_pipeline_params_set_voxels_per_unit(&s_pending_process_params, voxels_per_unit);
    }
}

pnanovdb_pipeline_type_t pipeline_get_pending_process_pipeline()
{
    return s_pending_process_pipeline;
}

pnanovdb_pipeline_type_t pipeline_get_pending_render_pipeline()
{
    return s_pending_render_pipeline;
}

float pipeline_get_pending_process_voxels_per_unit()
{
    return pnanovdb_pipeline_params_get_voxels_per_unit(&s_pending_process_params);
}

bool pipeline_is_rasterizing()
{
    return is_rasterization_running();
}

bool pipeline_get_rasterization_progress(std::string& progress_text, float& progress_value)
{
    return get_raster_task_progress(progress_text, progress_value);
}

bool pipeline_handle_rasterization_completion(EditorScene* editor_scene,
                                              std::shared_ptr<pnanovdb_raster_gaussian_data_t>& old_gaussian_data_ptr)
{
    if (!is_rasterization_completed())
    {
        return false;
    }

    if (s_raster_worker.isTaskSuccessful(s_raster_task_id))
    {
        pnanovdb_editor_token_t* scene_token = s_pending_raster_scene_token;

        if (s_pending_raster_nanovdb_array)
        {
            editor_scene->handle_nanovdb_data_load(scene_token, s_pending_raster_nanovdb_array,
                                                   s_pending_raster_filepath.c_str());

            if (s_pending_process_pipeline == pnanovdb_pipeline_type_raster3d)
            {
                std::filesystem::path fsPath(s_pending_raster_filepath);
                std::string view_name = fsPath.stem().string();
                pnanovdb_editor_token_t* name_token =
                    pnanovdb_editor::EditorToken::getInstance().getToken(view_name.c_str());

                auto* scene_manager = editor_scene->get_scene_manager();
                if (scene_manager)
                {
                    scene_manager->with_object(
                        scene_token, name_token,
                        [&](pnanovdb_editor::SceneObject* obj)
                        {
                            if (!obj)
                                return;

                            obj->resources.source_filepath = s_pending_raster_filepath;
                            obj->pipeline.process().type = s_pending_process_pipeline;
                            obj->pipeline.process().dirty = false; // Already converted
                            pnanovdb_pipeline_get_default_params(s_pending_process_pipeline,
                                                                 &obj->pipeline.process().params);
                            if (s_pending_process_params.data && s_pending_process_params.size > 0)
                            {
                                void* params_copy = malloc(s_pending_process_params.size);
                                if (params_copy)
                                {
                                    memcpy(params_copy, s_pending_process_params.data, s_pending_process_params.size);
                                    free(obj->pipeline.process().params.data);
                                    obj->pipeline.process().params.data = params_copy;
                                    obj->pipeline.process().params.size = s_pending_process_params.size;
                                    obj->pipeline.process().params.type = s_pending_process_params.type;
                                }
                            }

                            float pending_vpu =
                                pnanovdb_pipeline_params_get_voxels_per_unit(&s_pending_process_params);
                            Console::getInstance().addLog(Console::LogLevel::Debug,
                                "Rasterization: set raster3d pipeline on '%s' (vpu=%.1f, filepath='%s')",
                                view_name.c_str(), pending_vpu,
                                s_pending_raster_filepath.c_str());
                        });
                }
            }
        }
        else if (s_pending_gaussian_data)
        {
            editor_scene->handle_gaussian_data_load(scene_token, s_pending_gaussian_data, s_pending_raster_params,
                                                    s_pending_raster_filepath.c_str(), old_gaussian_data_ptr,
                                                    s_pending_process_pipeline, s_pending_render_pipeline,
                                                    s_pending_process_params.data ? &s_pending_process_params : nullptr);
        }
        Console::getInstance().addLog("Rasterization of '%s' was successful", s_pending_raster_filepath.c_str());

        // Re-sync to pick up the newly selected view for immediate rendering
        editor_scene->sync_selected_view_with_current();
    }
    else
    {
        Console::getInstance().addLog("Rasterization of '%s' failed", s_pending_raster_filepath.c_str());
    }

    // Clean up temporary worker thread raster context (if created during rasterization)
    if (s_pending_raster_ctx)
    {
        s_raster_raster->destroy_context(s_raster_compute, s_raster_device_queue, s_pending_raster_ctx);
        s_pending_raster_ctx = nullptr;
    }

    s_pending_raster_filepath = "";
    s_pending_raster_scene_token = nullptr;
    free(s_pending_process_params.data);
    s_pending_process_params = {};
    s_raster_worker.removeCompletedTask(s_raster_task_id);

    return true;
}

bool pipeline_update_async_progress(EditorScene* editor_scene,
                                    std::string& progress_text,
                                    float& progress_value,
                                    std::shared_ptr<pnanovdb_raster_gaussian_data_t>& old_gaussian_data_ptr)
{
    if (editor_scene)
    {
        if (pipeline_is_rasterizing())
        {
            pipeline_get_rasterization_progress(progress_text, progress_value);
            return true;
        }
        if (pipeline_handle_rasterization_completion(editor_scene, old_gaussian_data_ptr))
        {
            progress_text.clear();
            progress_value = 0.0f;
            return false;
        }
    }

    // Pipeline conversion (Gaussian->NanoVDB)
    if (is_conversion_completed())
    {
        pnanovdb_editor_token_t* conv_scene_token =
            pnanovdb_editor::EditorToken::getInstance().getTokenById(s_pending_scene_token_id);
        pnanovdb_editor_token_t* conv_name_token =
            pnanovdb_editor::EditorToken::getInstance().getTokenById(s_pending_name_token_id);

        handle_conversion_completion();

        if (editor_scene && conv_scene_token && conv_name_token)
        {
            editor_scene->update_scene_tree_after_conversion(conv_scene_token, conv_name_token);
        }

        return false;
    }
    if (is_conversion_running())
    {
        get_conversion_progress(progress_text, progress_value);
        return true;
    }

    return false;
}

bool pipeline_is_async_running()
{
    return is_conversion_running();
}

bool pipeline_create_variant(EditorSceneManager* scene_manager,
                             pnanovdb_editor_token_t* scene_token,
                             pnanovdb_editor_token_t* source_name,
                             const char* new_name)
{
    if (!scene_manager || !scene_token || !source_name || !new_name)
    {
        Console::getInstance().addLog(Console::LogLevel::Error,
            "pipeline_create_variant: null argument");
        return false;
    }

    std::string source_filepath;
    pnanovdb_pipeline_type_t source_process_type = pnanovdb_pipeline_type_noop;
    void* params_copy = nullptr;
    size_t params_copy_size = 0;
    const pnanovdb_reflect_data_type_t* params_copy_type = nullptr;
    bool source_found = false;

    scene_manager->with_object(
        scene_token, source_name,
        [&](SceneObject* src)
        {
            if (!src)
                return;
            source_found = true;
            source_filepath = src->resources.source_filepath;
            source_process_type = src->pipeline.process().type;
            auto& sp = src->pipeline.process().params;
            if (sp.data && sp.size > 0)
            {
                params_copy = malloc(sp.size);
                memcpy(params_copy, sp.data, sp.size);
                params_copy_size = sp.size;
                params_copy_type = sp.type;
            }
        });

    if (!source_found)
    {
        Console::getInstance().addLog(Console::LogLevel::Error,
            "Cannot create variant: source object '%s' not found",
            source_name->str ? source_name->str : "?");
        free(params_copy);
        return false;
    }

    if (source_filepath.empty())
    {
        Console::getInstance().addLog(Console::LogLevel::Error,
            "Cannot create variant: source object '%s' has no source file path",
            source_name->str ? source_name->str : "?");
        free(params_copy);
        return false;
    }

    pnanovdb_editor_token_t* new_name_token =
        pnanovdb_editor::EditorToken::getInstance().getToken(new_name);

    scene_manager->add_nanovdb(scene_token, new_name_token, nullptr, nullptr,
                               s_raster_compute, nullptr,
                               source_process_type,
                               pnanovdb_pipeline_type_nanovdb_render);

    bool configured = false;
    scene_manager->with_object(
        scene_token, new_name_token,
        [&](SceneObject* obj)
        {
            if (!obj)
                return;
            obj->resources.source_filepath = source_filepath;
            free(obj->pipeline.process().params.data);
            obj->pipeline.process().params.data = params_copy;
            obj->pipeline.process().params.size = params_copy_size;
            obj->pipeline.process().params.type = params_copy_type;
            params_copy = nullptr; // ownership transferred
            obj->pipeline.process().dirty = true;
            configured = true;
        });

    free(params_copy); // cleanup if ownership was not transferred

    if (!configured)
    {
        Console::getInstance().addLog(Console::LogLevel::Error,
            "Failed to configure variant '%s'", new_name);
        return false;
    }

    return true;
}

// ============================================================================
// ShaderParamProviderScope Implementation
// ============================================================================

ShaderParamProviderScope::ShaderParamProviderScope(uint32_t priority,
                                                   pnanovdb_shader_param_provider_ctx_t* ctx,
                                                   pnanovdb_shader_param_provider_fn get_fn)
{
    pnanovdb_shader_param_provider_t provider = {};
    provider.ctx = ctx;
    provider.get_value = get_fn;

    m_handle = pnanovdb_shader_param_provider_register(&provider, priority);
}

ShaderParamProviderScope::~ShaderParamProviderScope()
{
    if (m_handle != 0)
    {
        pnanovdb_shader_param_provider_unregister(m_handle);
    }
}

ShaderParamProviderScope::ShaderParamProviderScope(ShaderParamProviderScope&& other) noexcept : m_handle(other.m_handle)
{
    other.m_handle = 0;
}

ShaderParamProviderScope& ShaderParamProviderScope::operator=(ShaderParamProviderScope&& other) noexcept
{
    if (this != &other)
    {
        if (m_handle != 0)
        {
            pnanovdb_shader_param_provider_unregister(m_handle);
        }
        m_handle = other.m_handle;
        other.m_handle = 0;
    }
    return *this;
}

} // namespace pnanovdb_editor
