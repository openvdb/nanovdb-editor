// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   editor/Pipeline.cpp

    \author Petra Hapalova, Andrew Reidmeyer

    \brief  This file provides pipeline interface and registration system.
*/

#include "Pipeline.h"
#include "Editor.h"
#include "EditorScene.h"
#include "EditorSceneManager.h"
#include "EditorToken.h"
#include "PipelineRuntime.h"
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

using GaussianVoxelizeParams = pnanovdb_editor::GaussianVoxelizeParams;

struct VoxelBVHBuildParams
{
    float source_type = 0.f; // pnanovdb_pipeline_voxelbvh_source_t
    float resolution = static_cast<float>(pnanovdb_editor::k_default_bvh_resolution);
    float inflation_radius = 0.f;
};

PNANOVDB_REFLECT_STRUCT_OPAQUE_IMPL(VoxelBVHBuildParams)


// ============================================================================
// Async Worker Runtime Access
// ============================================================================

namespace
{

using pnanovdb_editor::current_runtime;
using pnanovdb_editor::PipelineRuntime;
using pnanovdb_editor::VoxelBVHBuildRequest;
using pnanovdb_editor::VoxelBVHBuildSource;
using pnanovdb_editor::with_runtime;

template <typename F>
bool with_runtime_or_warn(const char* fn_name, F&& fn)
{
    if (PipelineRuntime* rt = current_runtime())
    {
        return fn(*rt);
    }
    pnanovdb_editor::Console::getInstance().addLog(pnanovdb_editor::Console::LogLevel::Warning,
                                                   "%s: no PipelineRuntime registered for current thread; "
                                                   "entry point requires an editor::show() RuntimeScope to be active",
                                                   fn_name);
    return false;
}

} // anonymous namespace

// ============================================================================
// Pipeline Execute Functions (one per pipeline type)
// ============================================================================

using namespace pnanovdb_editor;

// ----------------------------------------------------------------------------
// Generic param helpers
// ----------------------------------------------------------------------------
template <typename T>
static void init_params_t(pnanovdb_pipeline_params_t* params)
{
    params->size = sizeof(T);
    params->type = nullptr;
    params->data = malloc(sizeof(T));
    if (params->data)
    {
        *static_cast<T*>(params->data) = T{};
    }
}

template <auto Getter>
static void* map_params(pnanovdb_scene_object_t* obj)
{
    auto* scene_obj = cast(obj);
    return scene_obj ? (scene_obj->*Getter)().data : nullptr;
}

static pnanovdb_pipeline_result_t execute_noop(pnanovdb_scene_object_t* obj, pnanovdb_pipeline_context_t*)
{
    auto* scene_obj = cast(obj);
    if (scene_obj)
        scene_obj->process_dirty() = false;
    return pnanovdb_pipeline_result_skipped;
}

static pnanovdb_pipeline_result_t execute_nanovdb_render(pnanovdb_scene_object_t* obj, pnanovdb_pipeline_context_t*)
{
    auto* scene_obj = cast(obj);
    if (!scene_obj || !scene_obj->nanovdb_array())
        return pnanovdb_pipeline_result_no_data;
    scene_obj->process_dirty() = false;
    return pnanovdb_pipeline_result_success;
}

static pnanovdb_pipeline_result_t execute_gaussian_splat(pnanovdb_scene_object_t* obj, pnanovdb_pipeline_context_t*)
{
    auto* scene_obj = cast(obj);
    if (!scene_obj || !scene_obj->gaussian_data())
        return pnanovdb_pipeline_result_no_data;
    scene_obj->process_dirty() = false;
    return pnanovdb_pipeline_result_success;
}

static pnanovdb_compute_array_t* get_or_synthesize_colors(pnanovdb_editor::SceneObject* scene_obj,
                                                          const pnanovdb_compute_t* compute,
                                                          pnanovdb_compute_array_t* positions)
{
    auto& arrays = scene_obj->named_arrays();
    auto it = arrays.find("colors");
    if (it != arrays.end() && it->second)
        return it->second;
    if (!positions || !compute || !compute->create_array || !compute->destroy_array)
        return nullptr;

    std::vector<float> white(positions->element_count, 1.0f);
    pnanovdb_compute_array_t* fallback = compute->create_array(sizeof(float), positions->element_count, white.data());
    if (fallback)
    {
        arrays["colors"] = fallback;
        scene_obj->resources.named_array_owners["colors"] =
            std::shared_ptr<pnanovdb_compute_array_t>(fallback,
                                                      [compute](pnanovdb_compute_array_t* arr)
                                                      {
                                                          if (arr && compute && compute->destroy_array)
                                                              compute->destroy_array(arr);
                                                      });
        Console::getInstance().addLog(Console::LogLevel::Debug,
                                      "VoxelBVH build: synthesized default white colors (%llu floats)",
                                      (unsigned long long)positions->element_count);
    }
    return fallback;
}

namespace
{
constexpr float k_mesh_debug_voxel_multiplier = 0.1f;
constexpr float k_mesh_lines_voxel_multiplier = 2.0f;

float bbox_max_extent(const pnanovdb_compute_array_t* positions)
{
    if (!positions || !positions->data || positions->element_count == 0u)
        return 0.f;
    const float* p = static_cast<const float*>(positions->data);
    const uint64_t vert_count = positions->element_count / 3u;
    if (vert_count == 0u)
        return 0.f;
    float mn[3] = { p[0], p[1], p[2] };
    float mx[3] = { p[0], p[1], p[2] };
    for (uint64_t v = 1u; v < vert_count; v++)
    {
        for (int a = 0; a < 3; a++)
        {
            const float val = p[3u * v + a];
            if (val < mn[a])
                mn[a] = val;
            if (val > mx[a])
                mx[a] = val;
        }
    }
    return std::max({ mx[0] - mn[0], mx[1] - mn[1], mx[2] - mn[2] });
}
} // namespace

static pnanovdb_pipeline_result_t execute_voxelbvh_build(pnanovdb_scene_object_t* obj, pnanovdb_pipeline_context_t* ctx)
{
    auto* scene_obj = cast(obj);
    if (!scene_obj)
    {
        return pnanovdb_pipeline_result_no_data;
    }

    if (!ctx || !ctx->voxelbvh || !ctx->voxelbvh_ctx || !ctx->compute || !ctx->queue)
    {
        Console::getInstance().addLog(
            Console::LogLevel::Error, "VoxelBVH build: missing voxelbvh interface or compute context");
        scene_obj->process_dirty() = false;
        return pnanovdb_pipeline_result_error;
    }

    auto& process_params = scene_obj->process_params();
    if (!process_params.data || process_params.size < sizeof(VoxelBVHBuildParams))
    {
        free(process_params.data);
        process_params.data = nullptr;
        process_params.size = 0;
        init_params_t<VoxelBVHBuildParams>(&process_params);
    }
    const auto* build_params = static_cast<const VoxelBVHBuildParams*>(process_params.data);

    const int source_type_raw = (int)(build_params->source_type + 0.5f);
    const int source_type = source_type_raw < 0 ? 0 : (source_type_raw > 3 ? 3 : source_type_raw);
    const pnanovdb_uint32_t resolution = (pnanovdb_uint32_t)(build_params->resolution + 0.5f);
    const float user_inflation_radius = build_params->inflation_radius;

    auto* voxelbvh = ctx->voxelbvh;
    auto* compute = ctx->compute;

    auto* scene_manager = cast(ctx->scene_manager);
    if (!scene_manager)
    {
        Console::getInstance().addLog(Console::LogLevel::Error, "VoxelBVH build: missing scene_manager");
        scene_obj->process_dirty() = false;
        return pnanovdb_pipeline_result_error;
    }

    float effective_inflation_radius = user_inflation_radius;

    auto get_named_array = [&](const char* key) -> pnanovdb_compute_array_t*
    {
        auto& arrays = scene_obj->named_arrays();
        auto it = arrays.find(key);
        return it == arrays.end() ? nullptr : it->second;
    };
    auto get_named_owner = [&](const char* key) -> std::shared_ptr<pnanovdb_compute_array_t>
    {
        auto& owners = scene_obj->resources.named_array_owners;
        auto it = owners.find(key);
        return it == owners.end() ? std::shared_ptr<pnanovdb_compute_array_t>{} : it->second;
    };

    const bool already_busy = with_runtime(false, [](PipelineRuntime& rt) { return rt.any_worker_busy(); });
    if (already_busy)
    {
        return pnanovdb_pipeline_result_pending;
    }

    VoxelBVHBuildRequest req;
    switch (source_type)
    {
    case pnanovdb_pipeline_voxelbvh_source_gaussian_file:
    {
        const std::string& path = scene_obj->resources.source_filepath;
        if (path.empty())
        {
            Console::getInstance().addLog(
                Console::LogLevel::Error, "VoxelBVH build (GaussianFile): scene object has no source_filepath");
            scene_obj->process_dirty() = false;
            return pnanovdb_pipeline_result_no_data;
        }
        if (!voxelbvh->nanovdb_from_gaussians_file)
        {
            Console::getInstance().addLog(
                Console::LogLevel::Error, "VoxelBVH build: voxelbvh interface missing nanovdb_from_gaussians_file");
            scene_obj->process_dirty() = false;
            return pnanovdb_pipeline_result_error;
        }

        req.source = VoxelBVHBuildSource::GaussianFile;
        req.resolution = resolution;
        req.filepath = path;
        break;
    }
    case pnanovdb_pipeline_voxelbvh_source_triangles:
    {
        auto* indices = get_named_array("indices");
        auto* positions = get_named_array("positions");
        if (!indices || !positions)
        {
            Console::getInstance().addLog(
                Console::LogLevel::Error,
                "VoxelBVH build (Triangles): missing named arrays 'indices' and/or 'positions'");
            scene_obj->process_dirty() = false;
            return pnanovdb_pipeline_result_no_data;
        }
        auto* colors = get_or_synthesize_colors(scene_obj, compute, positions);
        if (!colors)
        {
            Console::getInstance().addLog(
                Console::LogLevel::Error, "VoxelBVH build (Triangles): failed to obtain or synthesize 'colors'");
            scene_obj->process_dirty() = false;
            return pnanovdb_pipeline_result_error;
        }
        if (!voxelbvh->nanovdb_from_triangles_array)
        {
            Console::getInstance().addLog(
                Console::LogLevel::Error, "VoxelBVH build: voxelbvh interface missing nanovdb_from_triangles_array");
            scene_obj->process_dirty() = false;
            return pnanovdb_pipeline_result_error;
        }

        const bool auto_inflation = (user_inflation_radius == 0.f);
        if (auto_inflation && scene_obj->render_pipeline() == pnanovdb_pipeline_type_voxelbvh_triangles_debug_render &&
            resolution > 0u)
        {
            const float voxel_size = bbox_max_extent(positions) / static_cast<float>(resolution);
            if (voxel_size > 0.f)
                effective_inflation_radius = k_mesh_debug_voxel_multiplier * voxel_size;
        }

        req.source = VoxelBVHBuildSource::Triangles;
        req.resolution = resolution;
        req.inflation_radius = effective_inflation_radius;
        req.array_owners[0] = get_named_owner("indices");
        req.array_owners[1] = get_named_owner("positions");
        req.array_owners[2] = get_named_owner("colors");
        req.array_ptrs[0] = indices;
        req.array_ptrs[1] = positions;
        req.array_ptrs[2] = colors;
        break;
    }
    case pnanovdb_pipeline_voxelbvh_source_lines:
    {
        auto* indices = get_named_array("indices");
        auto* positions = get_named_array("positions");
        if (!indices || !positions)
        {
            Console::getInstance().addLog(
                Console::LogLevel::Error, "VoxelBVH build (Lines): missing named arrays 'indices' and/or 'positions'");
            scene_obj->process_dirty() = false;
            return pnanovdb_pipeline_result_no_data;
        }
        auto* colors = get_or_synthesize_colors(scene_obj, compute, positions);
        if (!colors)
        {
            Console::getInstance().addLog(
                Console::LogLevel::Error, "VoxelBVH build (Lines): failed to obtain or synthesize 'colors'");
            scene_obj->process_dirty() = false;
            return pnanovdb_pipeline_result_error;
        }
        if (!voxelbvh->nanovdb_from_lines_array)
        {
            Console::getInstance().addLog(
                Console::LogLevel::Error, "VoxelBVH build: voxelbvh interface missing nanovdb_from_lines_array");
            scene_obj->process_dirty() = false;
            return pnanovdb_pipeline_result_error;
        }
        const bool auto_inflation = (user_inflation_radius == 0.f);
        if (auto_inflation && resolution > 0u)
        {
            const float voxel_size = bbox_max_extent(positions) / static_cast<float>(resolution);
            if (voxel_size > 0.f)
                effective_inflation_radius = k_mesh_lines_voxel_multiplier * voxel_size;
        }

        req.source = VoxelBVHBuildSource::Lines;
        req.resolution = resolution;
        req.inflation_radius = effective_inflation_radius;
        req.array_owners[0] = get_named_owner("indices");
        req.array_owners[1] = get_named_owner("positions");
        req.array_owners[2] = get_named_owner("colors");
        req.array_ptrs[0] = indices;
        req.array_ptrs[1] = positions;
        req.array_ptrs[2] = colors;
        break;
    }
    case pnanovdb_pipeline_voxelbvh_source_gaussian_arrays:
    {
        static const char* gaussian_keys[6] = { "means", "opacities", "quaternions", "scales", "sh_0", "sh_n" };
        pnanovdb_compute_array_t* gaussian_arrays[6] = {};
        for (int i = 0; i < 6; ++i)
        {
            gaussian_arrays[i] = get_named_array(gaussian_keys[i]);
            if (!gaussian_arrays[i])
            {
                Console::getInstance().addLog(Console::LogLevel::Error,
                                              "VoxelBVH build (GaussianArrays): missing named array '%s'",
                                              gaussian_keys[i]);
                scene_obj->process_dirty() = false;
                return pnanovdb_pipeline_result_no_data;
            }
        }
        if (!voxelbvh->nanovdb_from_gaussians_array)
        {
            Console::getInstance().addLog(
                Console::LogLevel::Error, "VoxelBVH build: voxelbvh interface missing nanovdb_from_gaussians_array");
            scene_obj->process_dirty() = false;
            return pnanovdb_pipeline_result_error;
        }

        req.source = VoxelBVHBuildSource::GaussianArrays;
        req.resolution = resolution;
        for (int i = 0; i < 6; ++i)
        {
            req.array_owners[i] = get_named_owner(gaussian_keys[i]);
            req.array_ptrs[i] = gaussian_arrays[i];
        }
        break;
    }
    default:
        Console::getInstance().addLog(Console::LogLevel::Error, "VoxelBVH build: unknown source_type %d", source_type);
        scene_obj->process_dirty() = false;
        return pnanovdb_pipeline_result_error;
    }

    const bool started = with_runtime_or_warn("execute_voxelbvh_build",
                                              [&](PipelineRuntime& rt)
                                              {
                                                  auto* w = rt.worker<VoxelBVHWorker>();
                                                  return w && w->start(scene_obj, scene_manager, ctx, std::move(req));
                                              });
    if (started)
    {
        return pnanovdb_pipeline_result_pending;
    }

    // Worker start failed; keep process_dirty set so the next frame retries.
    Console::getInstance().addLog(
        Console::LogLevel::Error, "VoxelBVH build: worker start failed (source_type=%d); will retry", source_type);
    return pnanovdb_pipeline_result_pending;
}

static pnanovdb_pipeline_result_t execute_gaussian_voxelize(pnanovdb_scene_object_t* obj, pnanovdb_pipeline_context_t* ctx)
{
    auto* scene_obj = cast(obj);
    if (!scene_obj || scene_obj->resources.source_filepath.empty())
    {
        Console::getInstance().addLog(
            Console::LogLevel::Debug, "execute_gaussian_voxelize: early exit (scene_obj=%p, source_filepath='%s')",
            (void*)scene_obj, scene_obj ? scene_obj->resources.source_filepath.c_str() : "<null>");
        return pnanovdb_pipeline_result_no_data;
    }

    auto* scene_manager = cast(ctx->scene_manager);

    if (!scene_manager)
    {
        Console::getInstance().addLog(
            Console::LogLevel::Error, "Gaussian voxelize processing failed: missing scene_manager");
        return pnanovdb_pipeline_result_error;
    }

    // Ensure process params are allocated (may be missing if pipeline type was changed after creation)
    auto& process_params = scene_obj->process_params();
    if (!process_params.data || process_params.size < sizeof(GaussianVoxelizeParams))
    {
        Console::getInstance().addLog(
            Console::LogLevel::Debug, "execute_gaussian_voxelize: initializing missing GaussianVoxelizeParams");
        free(process_params.data);
        process_params.data = nullptr;
        process_params.size = 0;
        init_params_t<GaussianVoxelizeParams>(&process_params);
    }

    // Only one worker runs at a time
    bool busy = false;
    bool self_running = false;
    float running_vpu = pnanovdb_editor::k_default_voxels_per_unit;
    (void)with_runtime(false,
                       [&](PipelineRuntime& rt)
                       {
                           if (AsyncWorker* running = rt.busy_worker())
                           {
                               busy = true;
                               if (running->pipeline_type() == GaussianVoxelizeWorker::kPipelineType)
                               {
                                   self_running = true;
                                   if (auto* w = rt.worker<GaussianVoxelizeWorker>())
                                   {
                                       running_vpu = w->get_running_voxels_per_unit();
                                   }
                               }
                           }
                           return true;
                       });
    if (busy)
    {
        if (self_running)
        {
            const float requested_vpu = pnanovdb_editor::pipeline_params_get_voxels_per_unit(&process_params);
            if (requested_vpu != running_vpu)
            {
                Console::getInstance().addLog(
                    Console::LogLevel::Debug,
                    "Gaussian voxelize: conversion running (vpu=%.1f), will re-convert with vpu=%.1f when done",
                    running_vpu, requested_vpu);
            }
        }
        return pnanovdb_pipeline_result_pending;
    }

    const float vpu = pnanovdb_editor::pipeline_params_get_voxels_per_unit(&process_params);
    Console::getInstance().addLog("Starting Gaussian->NanoVDB conversion (voxels_per_unit=%.1f)...", vpu);

    const bool started = with_runtime_or_warn("execute_gaussian_voxelize",
                                              [&](PipelineRuntime& rt)
                                              {
                                                  auto* w = rt.worker<GaussianVoxelizeWorker>();
                                                  return w && w->start(scene_obj, scene_manager, ctx);
                                              });
    if (started)
        return pnanovdb_pipeline_result_pending;

    // start failed -- do NOT clear dirty so the next frame can retry.
    Console::getInstance().addLog(
        Console::LogLevel::Error, "execute_gaussian_voxelize: worker start failed (keeping dirty for retry)");
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

PNANOVDB_REFLECT_STRUCT_OPAQUE_IMPL(NanoVDBRenderParams)


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
static pnanovdb_pipeline_render_method_t get_render_method_gaussian(void)
{
    return pnanovdb_pipeline_render_method_gaussian;
}

// Field descriptors for GaussianVoxelizeParams (voxels_per_unit)
static const pnanovdb_pipeline_param_field_t s_gaussian_voxelize_param_fields[] = {
    { "Voxels/Unit", "Higher = finer detail, more memory", PNANOVDB_REFLECT_TYPE_FLOAT,
      offsetof(GaussianVoxelizeParams, voxels_per_unit), pnanovdb_editor::k_default_voxels_per_unit, 1.0f, 512.0f, 1.0f,
      nullptr, 0 }
};

// Field descriptors for VoxelBVHBuildParams
static const pnanovdb_pipeline_param_field_t s_voxelbvh_build_param_fields[] = {
    { "Resolution", "Max BVH integer coordinate (1..4096). Higher = finer voxel grid.", PNANOVDB_REFLECT_TYPE_FLOAT,
      offsetof(VoxelBVHBuildParams, resolution), static_cast<float>(pnanovdb_editor::k_default_bvh_resolution), 1.0f,
      static_cast<float>(pnanovdb_editor::k_max_bvh_resolution), 1.0f, nullptr, 0 },
    { "Inflation Radius", "World-space inflation applied to lines/triangles. 0 = auto for Debug/Lines renders.",
      PNANOVDB_REFLECT_TYPE_FLOAT, offsetof(VoxelBVHBuildParams, inflation_radius), 0.0f, 0.0f, 100.0f, 0.01f, nullptr,
      0 },
};

// ----------------------------------------------------------------------------
// Voxel BVH build params setters (public; declared in Pipeline.h)
// ----------------------------------------------------------------------------
static bool ensure_voxelbvh_build_params(pnanovdb_pipeline_params_t* params)
{
    if (!params)
        return false;
    if (!params->data || params->size < sizeof(VoxelBVHBuildParams))
    {
        free(params->data);
        params->data = nullptr;
        params->size = 0;
        init_params_t<VoxelBVHBuildParams>(params);
    }
    return params->data != nullptr;
}

bool pnanovdb_pipeline_voxelbvh_build_params_set_source_type(pnanovdb_pipeline_params_t* params,
                                                             pnanovdb_pipeline_voxelbvh_source_t source)
{
    if (!ensure_voxelbvh_build_params(params))
        return false;
    static_cast<VoxelBVHBuildParams*>(params->data)->source_type = static_cast<float>(source);
    return true;
}

bool pnanovdb_pipeline_voxelbvh_build_params_set_inflation_radius(pnanovdb_pipeline_params_t* params, float radius)
{
    if (!ensure_voxelbvh_build_params(params))
        return false;
    static_cast<VoxelBVHBuildParams*>(params->data)->inflation_radius = radius;
    return true;
}

bool pnanovdb_pipeline_voxelbvh_build_params_set_resolution(pnanovdb_pipeline_params_t* params,
                                                            pnanovdb_uint32_t resolution)
{
    if (!ensure_voxelbvh_build_params(params))
        return false;
    static_cast<VoxelBVHBuildParams*>(params->data)->resolution = static_cast<float>(resolution);
    return true;
}

// ============================================================================
// C++ Implementation
// ============================================================================

namespace pnanovdb_editor
{
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
    if (!obj->process_dirty())
        return pnanovdb_pipeline_result_skipped;

    pnanovdb_pipeline_context_t pipeline_ctx = {
        ctx.compute,    ctx.device,   ctx.queue,        ctx.compute_queue,  ctx.raster,
        ctx.raster_ctx, ctx.voxelbvh, ctx.voxelbvh_ctx, cast(ctx.renderer), cast(ctx.scene_manager)
    };
    return pnanovdb_pipeline_execute(obj->process_pipeline(), cast(obj), &pipeline_ctx);
}

bool pipeline_needs_process(SceneObject* obj)
{
    return obj && obj->process_dirty() && obj->process_pipeline() != pnanovdb_pipeline_type_noop;
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
                    obj->process_dirty() = false;
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
    if (obj->shader_name() && obj->shader_name()->str && obj->shader_name()->str[0] != '\0')
    {
        return obj->shader_name()->str;
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
// Public async-pipeline interface
// ============================================================================

void pipeline_init(const PipelineContext& ctx, EditorScene* editor_scene)
{
    (void)with_runtime_or_warn("pipeline_init",
                               [&](PipelineRuntime& rt)
                               {
                                   for (const auto& worker : rt.workers())
                                   {
                                       if (worker)
                                       {
                                           worker->init(ctx, editor_scene);
                                       }
                                   }
                                   return true;
                               });
}

bool pipeline_load(EditorSceneManager* scene_manager,
                   pnanovdb_editor_token_t* scene_token,
                   const PipelineLoadRequest& request)
{
    return with_runtime_or_warn(
        "pipeline_load",
        [&](PipelineRuntime& rt)
        {
            rt.handle_completions();

            if (AsyncWorker* busy = rt.busy_worker())
            {
                Console::getInstance().addLog(Console::LogLevel::Warning,
                                              "pipeline_load: an async task (pipeline type %u) is already in flight; "
                                              "ignoring load request for pipeline type %u",
                                              (unsigned)busy->pipeline_type(), (unsigned)request.load_pipeline);
                return false;
            }
            for (const auto& worker : rt.workers())
            {
                if (worker && worker->start_from_request(request, scene_manager, scene_token))
                {
                    return true;
                }
            }
            Console::getInstance().addLog(Console::LogLevel::Warning,
                                          "pipeline_load: no load worker registered for pipeline type %u",
                                          (unsigned)request.load_pipeline);
            return false;
        });
}

bool pipeline_update(std::string& progress_text, float& progress_value)
{
    PipelineRuntime* rt = current_runtime();
    if (!rt)
    {
        return false;
    }

    rt->handle_completions();
    if (AsyncWorker* running = rt->running_worker())
    {
        running->get_progress(progress_text, progress_value);
        return true;
    }

    progress_text.clear();
    progress_value = 0.0f;
    return false;
}

bool pipeline_create_variant(EditorSceneManager* scene_manager,
                             pnanovdb_editor_token_t* scene_token,
                             pnanovdb_editor_token_t* source_name,
                             const char* new_name,
                             const pnanovdb_compute_t* compute)
{
    if (!scene_manager || !scene_token || !source_name || !new_name)
    {
        Console::getInstance().addLog(Console::LogLevel::Error, "pipeline_create_variant: null argument");
        return false;
    }

    std::string source_filepath;
    pnanovdb_pipeline_type_t source_process_type = pnanovdb_pipeline_type_noop;
    void* params_copy = nullptr;
    size_t params_copy_size = 0;
    const pnanovdb_reflect_data_type_t* params_copy_type = nullptr;
    bool source_found = false;

    scene_manager->with_object(scene_token, source_name,
                               [&](SceneObject* src)
                               {
                                   if (!src)
                                       return;
                                   source_found = true;
                                   source_filepath = src->resources.source_filepath;
                                   source_process_type = src->process_pipeline();
                                   auto& sp = src->process_params();
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
        Console::getInstance().addLog(Console::LogLevel::Error, "Cannot create variant: source object '%s' not found",
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

    pnanovdb_editor_token_t* new_name_token = pnanovdb_editor::EditorToken::getInstance().getToken(new_name);

    scene_manager->add_nanovdb(scene_token, new_name_token, nullptr, nullptr, compute, nullptr, source_process_type,
                               pnanovdb_pipeline_type_nanovdb_render);

    bool configured = false;
    scene_manager->with_object(scene_token, new_name_token,
                               [&](SceneObject* obj)
                               {
                                   if (!obj)
                                       return;
                                   obj->resources.source_filepath = source_filepath;
                                   free(obj->process_params().data);
                                   obj->process_params().data = params_copy;
                                   obj->process_params().size = params_copy_size;
                                   obj->process_params().type = params_copy_type;
                                   params_copy = nullptr; // ownership transferred
                                   obj->process_dirty() = true;
                                   configured = true;
                               });

    free(params_copy); // cleanup if ownership was not transferred

    if (!configured)
    {
        Console::getInstance().addLog(Console::LogLevel::Error, "Failed to configure variant '%s'", new_name);
        return false;
    }

    return true;
}

} // namespace pnanovdb_editor

// ============================================================================
// Pipeline descriptor + registration macros
// ============================================================================

#define PNANOVDB_PIPELINE_PARAMS(T) sizeof(T), PNANOVDB_REFLECT_DATA_TYPE(T), init_params_t<T>
#define PNANOVDB_PIPELINE_NO_PARAMS 0, nullptr, nullptr
#define PNANOVDB_PIPELINE_FIELDS(arr) (arr), (sizeof(arr) / sizeof((arr)[0]))
#define PNANOVDB_PIPELINE_NO_FIELDS nullptr, 0

// load stage: no shaders, no render method, params are never mapped to the object.
#define PNANOVDB_REGISTER_LOAD_PIPELINE(var, type_, name_, params_, execute_)                                          \
    static const pnanovdb_pipeline_descriptor_t var = {                                                                \
        (type_), pnanovdb_pipeline_stage_load, (name_), nullptr, 0, params_, (execute_), get_render_method_none,       \
        nullptr, PNANOVDB_PIPELINE_NO_FIELDS,                                                                          \
    };                                                                                                                 \
    PNANOVDB_REGISTER_PIPELINE(var)

// process stage: params are always mapped to SceneObject::process_params.
#define PNANOVDB_REGISTER_PROCESS_PIPELINE(                                                                            \
    var, type_, name_, shaders_, shader_count_, params_, execute_, render_method_, fields_)                            \
    static const pnanovdb_pipeline_descriptor_t var = {                                                                \
        (type_),                                                                                                       \
        pnanovdb_pipeline_stage_process,                                                                               \
        (name_),                                                                                                       \
        (shaders_),                                                                                                    \
        (shader_count_),                                                                                               \
        params_,                                                                                                       \
        (execute_),                                                                                                    \
        (render_method_),                                                                                              \
        map_params<&SceneObject::process_params>,                                                                      \
        fields_,                                                                                                       \
    };                                                                                                                 \
    PNANOVDB_REGISTER_PIPELINE(var)

// render stage: caller picks the render method and how params map to the object.
#define PNANOVDB_REGISTER_RENDER_PIPELINE(                                                                             \
    var, type_, name_, shaders_, shader_count_, params_, execute_, render_method_, map_)                               \
    static const pnanovdb_pipeline_descriptor_t var = {                                                                \
        (type_),         pnanovdb_pipeline_stage_render,                                                               \
        (name_),         (shaders_),                                                                                   \
        (shader_count_), params_,                                                                                      \
        (execute_),      (render_method_),                                                                             \
        (map_),          PNANOVDB_PIPELINE_NO_FIELDS,                                                                  \
    };                                                                                                                 \
    PNANOVDB_REGISTER_PIPELINE(var)

// Common render case: draw an existing NanoVDB grid with NanoVDBRenderParams.
#define PNANOVDB_REGISTER_NANOVDB_RENDER_PIPELINE(var, type_, name_, shaders_)                                         \
    PNANOVDB_REGISTER_RENDER_PIPELINE(var, (type_), (name_), (shaders_), 1,                                            \
                                      PNANOVDB_PIPELINE_PARAMS(NanoVDBRenderParams), execute_nanovdb_render,           \
                                      get_render_method_nanovdb, map_params<&SceneObject::render_params>)

// ============================================================================
// Pipeline shader definitions
// ============================================================================

PNANOVDB_DEFINE_PIPELINE_SHADERS(s_nanovdb_render_shaders,
                                 PNANOVDB_PIPELINE_SHADER("editor/editor.slang", nullptr, PNANOVDB_TRUE));

PNANOVDB_DEFINE_PIPELINE_SHADERS(s_nanovdb_surface_shaders,
                                 PNANOVDB_PIPELINE_SHADER("editor/editor_surface.slang", nullptr, PNANOVDB_TRUE));

PNANOVDB_DEFINE_PIPELINE_SHADERS(s_gaussian_splat_shaders,
                                 PNANOVDB_PIPELINE_SHADER("raster/gaussian_rasterize_2d.slang",
                                                          "raster/raster2d_group",
                                                          PNANOVDB_FALSE));

PNANOVDB_DEFINE_PIPELINE_SHADERS(s_gaussian_voxelize_shaders,
                                 PNANOVDB_PIPELINE_SHADER("raster/gaussian_frag_color.slang", nullptr, PNANOVDB_TRUE));

PNANOVDB_DEFINE_PIPELINE_SHADERS(s_voxelbvh_gaussians_shaders,
                                 PNANOVDB_PIPELINE_SHADER("editor/voxelbvh_gaussians.slang", nullptr, PNANOVDB_TRUE));

PNANOVDB_DEFINE_PIPELINE_SHADERS(s_voxelbvh_lines_render_shaders,
                                 PNANOVDB_PIPELINE_SHADER("editor/voxelbvh_lines.slang", nullptr, PNANOVDB_TRUE));

PNANOVDB_DEFINE_PIPELINE_SHADERS(s_voxelbvh_triangles_render_shaders,
                                 PNANOVDB_PIPELINE_SHADER("editor/voxelbvh_triangles.slang", nullptr, PNANOVDB_TRUE));

PNANOVDB_DEFINE_PIPELINE_SHADERS(s_voxelbvh_triangles_debug_render_shaders,
                                 PNANOVDB_PIPELINE_SHADER("editor/voxelbvh_triangles_debug.slang", nullptr, PNANOVDB_TRUE));

PNANOVDB_DEFINE_PIPELINE_SHADERS(s_voxelbvh_debug_render_shaders,
                                 PNANOVDB_PIPELINE_SHADER("editor/voxelbvh_debug.slang", nullptr, PNANOVDB_TRUE));

// ============================================================================
// Self-registering pipeline descriptors
// ============================================================================

PNANOVDB_REGISTER_LOAD_PIPELINE(
    s_noop_descriptor, pnanovdb_pipeline_type_noop, "No Operation", PNANOVDB_PIPELINE_NO_PARAMS, execute_noop);

PNANOVDB_REGISTER_NANOVDB_RENDER_PIPELINE(s_nanovdb_render_descriptor,
                                          pnanovdb_pipeline_type_nanovdb_render,
                                          "NanoVDB Render",
                                          s_nanovdb_render_shaders);

// SDF/level-set isosurface rendered via HDDA zero-crossing search.
PNANOVDB_REGISTER_NANOVDB_RENDER_PIPELINE(s_nanovdb_surface_descriptor,
                                          pnanovdb_pipeline_type_nanovdb_surface,
                                          "NanoVDB Surface (SDF)",
                                          s_nanovdb_surface_shaders);

PNANOVDB_REGISTER_NANOVDB_RENDER_PIPELINE(s_voxelbvh_gaussians_render_descriptor,
                                          pnanovdb_pipeline_type_voxelbvh_gaussians_render,
                                          "Voxel BVH Gaussians",
                                          s_voxelbvh_gaussians_shaders);

PNANOVDB_REGISTER_NANOVDB_RENDER_PIPELINE(s_voxelbvh_lines_render_descriptor,
                                          pnanovdb_pipeline_type_voxelbvh_lines_render,
                                          "Voxel BVH Lines",
                                          s_voxelbvh_lines_render_shaders);

PNANOVDB_REGISTER_NANOVDB_RENDER_PIPELINE(s_voxelbvh_triangles_render_descriptor,
                                          pnanovdb_pipeline_type_voxelbvh_triangles_render,
                                          "Voxel BVH Triangles",
                                          s_voxelbvh_triangles_render_shaders);

PNANOVDB_REGISTER_NANOVDB_RENDER_PIPELINE(s_voxelbvh_triangles_debug_render_descriptor,
                                          pnanovdb_pipeline_type_voxelbvh_triangles_debug_render,
                                          "Voxel BVH Triangles Debug",
                                          s_voxelbvh_triangles_debug_render_shaders);

PNANOVDB_REGISTER_NANOVDB_RENDER_PIPELINE(s_voxelbvh_debug_render_descriptor,
                                          pnanovdb_pipeline_type_voxelbvh_debug_render,
                                          "Voxel BVH Debug",
                                          s_voxelbvh_debug_render_shaders);

// Gaussian splat draws the loaded Gaussians directly; params come from shader JSON.
PNANOVDB_REGISTER_RENDER_PIPELINE(s_gaussian_splat_descriptor,
                                  pnanovdb_pipeline_type_gaussian_splat,
                                  "Gaussian 2D Splatting",
                                  s_gaussian_splat_shaders,
                                  1,
                                  PNANOVDB_PIPELINE_NO_PARAMS,
                                  execute_gaussian_splat,
                                  get_render_method_gaussian,
                                  nullptr);

// Gaussian voxelize converts Gaussians to NanoVDB and then renders as NanoVDB.
PNANOVDB_REGISTER_PROCESS_PIPELINE(s_gaussian_voxelize_descriptor,
                                   pnanovdb_pipeline_type_gaussian_voxelize,
                                   "Gaussian to NanoVDB",
                                   s_gaussian_voxelize_shaders,
                                   1,
                                   PNANOVDB_PIPELINE_PARAMS(GaussianVoxelizeParams),
                                   execute_gaussian_voxelize,
                                   get_render_method_nanovdb,
                                   PNANOVDB_PIPELINE_FIELDS(s_gaussian_voxelize_param_fields));

PNANOVDB_REGISTER_PROCESS_PIPELINE(s_voxelbvh_build_descriptor,
                                   pnanovdb_pipeline_type_voxelbvh_build,
                                   "Voxel BVH Build",
                                   nullptr,
                                   0,
                                   PNANOVDB_PIPELINE_PARAMS(VoxelBVHBuildParams),
                                   execute_voxelbvh_build,
                                   get_render_method_nanovdb,
                                   PNANOVDB_PIPELINE_FIELDS(s_voxelbvh_build_param_fields));


PNANOVDB_REGISTER_LOAD_PIPELINE(s_mesh_load_descriptor,
                                pnanovdb_pipeline_type_mesh_load,
                                "Mesh PLY Load",
                                PNANOVDB_PIPELINE_PARAMS(pnanovdb_editor::MeshLoadParams),
                                nullptr);

PNANOVDB_REGISTER_LOAD_PIPELINE(s_gaussian_load_descriptor,
                                pnanovdb_pipeline_type_gaussian_load,
                                "Gaussian File Load",
                                PNANOVDB_PIPELINE_NO_PARAMS,
                                nullptr);
