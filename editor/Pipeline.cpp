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
    float resolution = 511.f;
    float inflation_radius = 0.f;
};

PNANOVDB_REFLECT_STRUCT_OPAQUE_IMPL(VoxelBVHBuildParams)


// ============================================================================
// Pipeline Registry
// ============================================================================

static std::array<const pnanovdb_pipeline_descriptor_t*, pnanovdb_pipeline_type_count> s_pipeline_registry = {};
static pnanovdb_uint32_t s_pipeline_count = 0;
static std::mutex s_pipeline_registry_mutex;

void pnanovdb_pipeline_register(const pnanovdb_pipeline_descriptor_t* descriptor)
{
    if (!descriptor || descriptor->type >= pnanovdb_pipeline_type_count)
    {
        return;
    }
    std::lock_guard<std::mutex> lock(s_pipeline_registry_mutex);
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
    std::lock_guard<std::mutex> lock(s_pipeline_registry_mutex);
    return s_pipeline_count;
}

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
    auto* voxelbvh_ctx = ctx->voxelbvh_ctx;
    auto* compute = ctx->compute;
    auto* queue = ctx->queue;

    pnanovdb_compute_array_t* result = nullptr;
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

    auto* scene_manager = cast(ctx->scene_manager);
    const bool async_available = ctx->compute_queue && ctx->compute_queue != ctx->queue && scene_manager;
    auto try_start_async = [&](VoxelBVHBuildRequest&& req) -> pnanovdb_pipeline_result_t
    {
        if (!async_available)
        {
            return pnanovdb_pipeline_result_skipped;
        }
        const bool already_running = with_runtime(false,
                                                  [](PipelineRuntime& rt)
                                                  {
                                                      auto* w = rt.worker<VoxelBVHWorker>();
                                                      return w && w->is_running();
                                                  });
        if (already_running)
        {
            return pnanovdb_pipeline_result_pending;
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
        return pnanovdb_pipeline_result_skipped;
    };

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

        VoxelBVHBuildRequest req;
        req.source = VoxelBVHBuildSource::GaussianFile;
        req.resolution = resolution;
        req.filepath = path;
        const pnanovdb_pipeline_result_t async_result = try_start_async(std::move(req));
        if (async_result == pnanovdb_pipeline_result_pending)
            return pnanovdb_pipeline_result_pending;

        result = voxelbvh->nanovdb_from_gaussians_file(compute, queue, voxelbvh_ctx, path.c_str(), resolution);
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

        VoxelBVHBuildRequest req;
        req.source = VoxelBVHBuildSource::Triangles;
        req.resolution = resolution;
        req.inflation_radius = effective_inflation_radius;
        req.array_owners[0] = get_named_owner("indices");
        req.array_owners[1] = get_named_owner("positions");
        req.array_owners[2] = get_named_owner("colors");
        req.array_ptrs[0] = indices;
        req.array_ptrs[1] = positions;
        req.array_ptrs[2] = colors;
        const pnanovdb_pipeline_result_t async_result = try_start_async(std::move(req));
        if (async_result == pnanovdb_pipeline_result_pending)
            return pnanovdb_pipeline_result_pending;

        result = voxelbvh->nanovdb_from_triangles_array(
            compute, queue, voxelbvh_ctx, indices, positions, colors, effective_inflation_radius, resolution);
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

        VoxelBVHBuildRequest req;
        req.source = VoxelBVHBuildSource::Lines;
        req.resolution = resolution;
        req.inflation_radius = effective_inflation_radius;
        req.array_owners[0] = get_named_owner("indices");
        req.array_owners[1] = get_named_owner("positions");
        req.array_owners[2] = get_named_owner("colors");
        req.array_ptrs[0] = indices;
        req.array_ptrs[1] = positions;
        req.array_ptrs[2] = colors;
        const pnanovdb_pipeline_result_t async_result = try_start_async(std::move(req));
        if (async_result == pnanovdb_pipeline_result_pending)
            return pnanovdb_pipeline_result_pending;

        result = voxelbvh->nanovdb_from_lines_array(
            compute, queue, voxelbvh_ctx, indices, positions, colors, effective_inflation_radius, resolution);
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

        VoxelBVHBuildRequest req;
        req.source = VoxelBVHBuildSource::GaussianArrays;
        req.resolution = resolution;
        for (int i = 0; i < 6; ++i)
        {
            req.array_owners[i] = get_named_owner(gaussian_keys[i]);
            req.array_ptrs[i] = gaussian_arrays[i];
        }
        const pnanovdb_pipeline_result_t async_result = try_start_async(std::move(req));
        if (async_result == pnanovdb_pipeline_result_pending)
            return pnanovdb_pipeline_result_pending;

        result = voxelbvh->nanovdb_from_gaussians_array(compute, queue, voxelbvh_ctx, gaussian_arrays, 6u, resolution);
        break;
    }
    default:
        Console::getInstance().addLog(Console::LogLevel::Error, "VoxelBVH build: unknown source_type %d", source_type);
        scene_obj->process_dirty() = false;
        return pnanovdb_pipeline_result_error;
    }

    if (!result)
    {
        Console::getInstance().addLog(
            Console::LogLevel::Error, "VoxelBVH build: nanovdb_from_* returned null (source_type=%d)", source_type);
        scene_obj->process_dirty() = false;
        return pnanovdb_pipeline_result_error;
    }

    std::shared_ptr<pnanovdb_compute_array_t> owner(result,
                                                    [compute](pnanovdb_compute_array_t* arr)
                                                    {
                                                        if (arr && compute)
                                                            compute->destroy_array(arr);
                                                    });
    scene_obj->nanovdb_array() = result;
    scene_obj->resources.nanovdb_array_owner = owner;

    Console::getInstance().addLog(
        "VoxelBVH build: source_type=%d, render_pipeline=%d, resolution=%u, "
        "inflation_radius=%.5f, nanovdb (%llu elements)",
        source_type, static_cast<int>(scene_obj->render_pipeline()), resolution, effective_inflation_radius,
        (unsigned long long)result->element_count);

    scene_obj->process_dirty() = false;
    return pnanovdb_pipeline_result_success;
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

    const bool conversion_running = with_runtime(false,
                                                 [](PipelineRuntime& rt)
                                                 {
                                                     auto* w = rt.worker<GaussianVoxelizeWorker>();
                                                     return w && w->is_running();
                                                 });
    if (conversion_running)
    {
        const float running_vpu =
            with_runtime(pnanovdb_editor::k_default_voxels_per_unit,
                         [](PipelineRuntime& rt)
                         {
                             auto* w = rt.worker<GaussianVoxelizeWorker>();
                             return w ? w->get_running_voxels_per_unit() : pnanovdb_editor::k_default_voxels_per_unit;
                         });
        const float requested_vpu = pnanovdb_editor::pipeline_params_get_voxels_per_unit(&process_params);
        if (requested_vpu != running_vpu)
        {
            Console::getInstance().addLog(
                Console::LogLevel::Debug,
                "Gaussian voxelize: conversion running (vpu=%.1f), will re-convert with vpu=%.1f when done",
                running_vpu, requested_vpu);
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
// ============================================================================
// Built-in Pipeline Definitions
// ============================================================================

PNANOVDB_DEFINE_PIPELINE_SHADERS(s_nanovdb_render_shaders,
                                 PNANOVDB_PIPELINE_SHADER("editor/editor.slang", nullptr, PNANOVDB_TRUE));

PNANOVDB_DEFINE_PIPELINE_SHADERS(s_gaussian_splat_shaders,
                                 PNANOVDB_PIPELINE_SHADER("raster/gaussian_rasterize_2d.slang",
                                                          "raster/raster2d_group",
                                                          PNANOVDB_FALSE));

PNANOVDB_DEFINE_PIPELINE_SHADERS(s_gaussian_voxelize_shaders,
                                 PNANOVDB_PIPELINE_SHADER("raster/gaussian_frag_color.slang", nullptr, PNANOVDB_TRUE));

PNANOVDB_DEFINE_PIPELINE_SHADERS(s_voxelbvh_render_shaders,
                                 PNANOVDB_PIPELINE_SHADER("editor/voxelbvh.slang", nullptr, PNANOVDB_TRUE));

PNANOVDB_DEFINE_PIPELINE_SHADERS(s_voxelbvh_lines_render_shaders,
                                 PNANOVDB_PIPELINE_SHADER("editor/voxelbvh_lines.slang", nullptr, PNANOVDB_TRUE));

PNANOVDB_DEFINE_PIPELINE_SHADERS(s_voxelbvh_triangles_render_shaders,
                                 PNANOVDB_PIPELINE_SHADER("editor/voxelbvh_triangles.slang", nullptr, PNANOVDB_TRUE));

PNANOVDB_DEFINE_PIPELINE_SHADERS(s_voxelbvh_triangles_debug_render_shaders,
                                 PNANOVDB_PIPELINE_SHADER("editor/voxelbvh_triangles_debug.slang", nullptr, PNANOVDB_TRUE));

PNANOVDB_DEFINE_PIPELINE_SHADERS(s_voxelbvh_debug_render_shaders,
                                 PNANOVDB_PIPELINE_SHADER("editor/voxelbvh_debug.slang", nullptr, PNANOVDB_TRUE));

// Field descriptors for GaussianVoxelizeParams (voxels_per_unit)
static const pnanovdb_pipeline_param_field_t s_gaussian_voxelize_param_fields[] = {
    { "Voxels/Unit", "Higher = finer detail, more memory", PNANOVDB_REFLECT_TYPE_FLOAT,
      offsetof(GaussianVoxelizeParams, voxels_per_unit), pnanovdb_editor::k_default_voxels_per_unit, 1.0f, 512.0f, 1.0f,
      nullptr, 0 }
};

// Field descriptors for VoxelBVHBuildParams
static const pnanovdb_pipeline_param_field_t s_voxelbvh_build_param_fields[] = {
    { "Resolution", "Max BVH integer coordinate (1..4095). Higher = finer voxel grid.", PNANOVDB_REFLECT_TYPE_FLOAT,
      offsetof(VoxelBVHBuildParams, resolution), 511.0f, 1.0f, 4095.0f, 1.0f, nullptr, 0 },
    { "Inflation Radius", "World-space inflation applied to lines/triangles. 0 = auto for Debug/Lines renders.",
      PNANOVDB_REFLECT_TYPE_FLOAT, offsetof(VoxelBVHBuildParams, inflation_radius), 0.0f, 0.0f, 100.0f, 0.01f, nullptr,
      0 },
};

// ----------------------------------------------------------------------------
// Self-registering pipeline descriptors.
// ----------------------------------------------------------------------------
namespace
{
struct PipelineRegistrar
{
    explicit PipelineRegistrar(const pnanovdb_pipeline_descriptor_t* desc)
    {
        pnanovdb_pipeline_register(desc);
    }
};
} // namespace

#define PNANOVDB_REGISTER_PIPELINE(desc_var) static const PipelineRegistrar desc_var##_registrar(&desc_var)

#define PNANOVDB_DEFINE_NANOVDB_RENDER_PIPELINE(desc_var, type_, name_, shaders_)                                      \
    static const pnanovdb_pipeline_descriptor_t desc_var = {                                                           \
        /*type*/ (type_),                                                                                              \
        /*stage*/ pnanovdb_pipeline_stage_render,                                                                      \
        /*name*/ (name_),                                                                                              \
        /*shaders*/ (shaders_),                                                                                        \
        /*shader_count*/ 1,                                                                                            \
        /*params_size*/ sizeof(NanoVDBRenderParams),                                                                   \
        /*params_data_type*/ PNANOVDB_REFLECT_DATA_TYPE(NanoVDBRenderParams),                                          \
        /*init_params*/ init_params_t<NanoVDBRenderParams>,                                                            \
        /*execute*/ execute_nanovdb_render,                                                                            \
        /*get_render_method*/ get_render_method_nanovdb,                                                               \
        /*map_params*/ map_params<&SceneObject::render_params>,                                                        \
        /*param_fields*/ nullptr,                                                                                      \
        /*param_field_count*/ 0,                                                                                       \
    }

static const pnanovdb_pipeline_descriptor_t s_noop_descriptor = {
    /*type*/ pnanovdb_pipeline_type_noop,
    /*stage*/ pnanovdb_pipeline_stage_load,
    /*name*/ "No Operation",
    /*shaders*/ nullptr,
    /*shader_count*/ 0,
    /*params_size*/ 0,
    /*params_data_type*/ nullptr,
    /*init_params*/ nullptr,
    /*execute*/ execute_noop,
    /*get_render_method*/ get_render_method_none,
    /*map_params*/ nullptr,
    /*param_fields*/ nullptr,
    /*param_field_count*/ 0,
};
PNANOVDB_REGISTER_PIPELINE(s_noop_descriptor);

PNANOVDB_DEFINE_NANOVDB_RENDER_PIPELINE(s_nanovdb_render_descriptor,
                                        pnanovdb_pipeline_type_nanovdb_render,
                                        "NanoVDB Render",
                                        s_nanovdb_render_shaders);
PNANOVDB_REGISTER_PIPELINE(s_nanovdb_render_descriptor);

static const pnanovdb_pipeline_descriptor_t s_gaussian_splat_descriptor = {
    /*type*/ pnanovdb_pipeline_type_gaussian_splat,
    /*stage*/ pnanovdb_pipeline_stage_render,
    /*name*/ "Gaussian 2D Splatting",
    /*shaders*/ s_gaussian_splat_shaders,
    /*shader_count*/ 1,
    /*params_size*/ 0,
    /*params_data_type*/ nullptr, // params come from shader JSON
    /*init_params*/ nullptr,
    /*execute*/ execute_gaussian_splat,
    /*get_render_method*/ get_render_method_gaussian,
    /*map_params*/ nullptr,
    /*param_fields*/ nullptr,
    /*param_field_count*/ 0,
};
PNANOVDB_REGISTER_PIPELINE(s_gaussian_splat_descriptor);

// Gaussian voxelize converts Gaussians to NanoVDB and then renders as NanoVDB --
// hence get_render_method_nanovdb.
static const pnanovdb_pipeline_descriptor_t s_gaussian_voxelize_descriptor = {
    /*type*/ pnanovdb_pipeline_type_gaussian_voxelize,
    /*stage*/ pnanovdb_pipeline_stage_process,
    /*name*/ "Gaussian to NanoVDB",
    /*shaders*/ s_gaussian_voxelize_shaders,
    /*shader_count*/ 1,
    /*params_size*/ sizeof(GaussianVoxelizeParams),
    /*params_data_type*/ PNANOVDB_REFLECT_DATA_TYPE(GaussianVoxelizeParams),
    /*init_params*/ init_params_t<GaussianVoxelizeParams>,
    /*execute*/ execute_gaussian_voxelize,
    /*get_render_method*/ get_render_method_nanovdb,
    /*map_params*/ map_params<&SceneObject::process_params>,
    /*param_fields*/ s_gaussian_voxelize_param_fields,
    /*param_field_count*/ sizeof(s_gaussian_voxelize_param_fields) / sizeof(s_gaussian_voxelize_param_fields[0]),
};
PNANOVDB_REGISTER_PIPELINE(s_gaussian_voxelize_descriptor);

PNANOVDB_DEFINE_NANOVDB_RENDER_PIPELINE(s_voxelbvh_render_descriptor,
                                        pnanovdb_pipeline_type_voxelbvh_render,
                                        "Voxel BVH Render",
                                        s_voxelbvh_render_shaders);
PNANOVDB_REGISTER_PIPELINE(s_voxelbvh_render_descriptor);

PNANOVDB_DEFINE_NANOVDB_RENDER_PIPELINE(s_voxelbvh_lines_render_descriptor,
                                        pnanovdb_pipeline_type_voxelbvh_lines_render,
                                        "Voxel BVH Lines",
                                        s_voxelbvh_lines_render_shaders);
PNANOVDB_REGISTER_PIPELINE(s_voxelbvh_lines_render_descriptor);

PNANOVDB_DEFINE_NANOVDB_RENDER_PIPELINE(s_voxelbvh_triangles_render_descriptor,
                                        pnanovdb_pipeline_type_voxelbvh_triangles_render,
                                        "Voxel BVH Triangles",
                                        s_voxelbvh_triangles_render_shaders);
PNANOVDB_REGISTER_PIPELINE(s_voxelbvh_triangles_render_descriptor);

PNANOVDB_DEFINE_NANOVDB_RENDER_PIPELINE(s_voxelbvh_triangles_debug_render_descriptor,
                                        pnanovdb_pipeline_type_voxelbvh_triangles_debug_render,
                                        "Voxel BVH Triangles Debug",
                                        s_voxelbvh_triangles_debug_render_shaders);
PNANOVDB_REGISTER_PIPELINE(s_voxelbvh_triangles_debug_render_descriptor);

static const pnanovdb_pipeline_descriptor_t s_voxelbvh_build_descriptor = {
    /*type*/ pnanovdb_pipeline_type_voxelbvh_build,
    /*stage*/ pnanovdb_pipeline_stage_process,
    /*name*/ "Voxel BVH Build",
    /*shaders*/ nullptr,
    /*shader_count*/ 0,
    /*params_size*/ sizeof(VoxelBVHBuildParams),
    /*params_data_type*/ PNANOVDB_REFLECT_DATA_TYPE(VoxelBVHBuildParams),
    /*init_params*/ init_params_t<VoxelBVHBuildParams>,
    /*execute*/ execute_voxelbvh_build,
    /*get_render_method*/ get_render_method_nanovdb,
    /*map_params*/ map_params<&SceneObject::process_params>,
    /*param_fields*/ s_voxelbvh_build_param_fields,
    /*param_field_count*/ sizeof(s_voxelbvh_build_param_fields) / sizeof(s_voxelbvh_build_param_fields[0]),
};
PNANOVDB_REGISTER_PIPELINE(s_voxelbvh_build_descriptor);

PNANOVDB_DEFINE_NANOVDB_RENDER_PIPELINE(s_voxelbvh_debug_render_descriptor,
                                        pnanovdb_pipeline_type_voxelbvh_debug_render,
                                        "Voxel BVH Debug",
                                        s_voxelbvh_debug_render_shaders);
PNANOVDB_REGISTER_PIPELINE(s_voxelbvh_debug_render_descriptor);

static const pnanovdb_pipeline_descriptor_t s_mesh_load_descriptor = {
    /*type*/ pnanovdb_pipeline_type_mesh_load,
    /*stage*/ pnanovdb_pipeline_stage_load,
    /*name*/ "Mesh PLY Load",
    /*shaders*/ nullptr,
    /*shader_count*/ 0,
    /*params_size*/ sizeof(pnanovdb_editor::MeshLoadParams),
    /*params_data_type*/ PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_editor::MeshLoadParams),
    /*init_params*/ init_params_t<pnanovdb_editor::MeshLoadParams>,
    /*execute*/ nullptr,
    /*get_render_method*/ get_render_method_none,
    /*map_params*/ nullptr,
    /*param_fields*/ nullptr,
    /*param_field_count*/ 0,
};
PNANOVDB_REGISTER_PIPELINE(s_mesh_load_descriptor);

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
    std::lock_guard<std::mutex> lock(s_pipeline_registry_mutex);
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
        scene_obj->process_dirty() = true;
}

void* pnanovdb_scene_object_map_params(pnanovdb_scene_object_t* obj, const pnanovdb_reflect_data_type_t* param_data_type)
{
    if (!obj || !param_data_type)
        return nullptr;

    std::lock_guard<std::mutex> lock(s_pipeline_registry_mutex);
    for (size_t i = 0; i < pnanovdb_pipeline_type_count; ++i)
    {
        const auto* desc = s_pipeline_registry[i];
        if (desc && desc->params_data_type && desc->map_params &&
            pnanovdb_reflect_layout_compare(desc->params_data_type, param_data_type))
        {
            return desc->map_params(obj);
        }
    }
    return nullptr;
}

void pnanovdb_scene_object_unmap_params(pnanovdb_scene_object_t* obj, const pnanovdb_reflect_data_type_t* param_data_type)
{
    // Currently no-op - params stay mapped
    (void)obj;
    (void)param_data_type;
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

    (void)shader_idx;

    const char* shader_name = nullptr;
    if (scene_obj->shader_name())
    {
        shader_name = scene_obj->shader_name()->str;
    }

    const auto* dt = scene_obj->shader_params_data_type();
    if (!dt || !scene_obj->shader_params())
        return PNANOVDB_FALSE;

    ShaderParamsDescCache& cache = scene_obj->params.shader_params_desc_cache;
    if (cache.source_data_type != dt)
    {
        cache.source_data_type = dt;
        cache.element_names.clear();
        cache.element_type_names.clear();
        cache.element_offsets.clear();
        cache.element_names.reserve(dt->child_reflect_data_count);
        cache.element_type_names.reserve(dt->child_reflect_data_count);
        cache.element_offsets.reserve(dt->child_reflect_data_count);
        for (pnanovdb_uint64_t i = 0; i < dt->child_reflect_data_count; ++i)
        {
            const pnanovdb_reflect_data_t& child = dt->child_reflect_datas[i];
            cache.element_names.push_back(child.name);
            const char* type_name = "unknown";
            if (child.data_type)
            {
                type_name = child.data_type->struct_typename ?
                                child.data_type->struct_typename :
                                pnanovdb_reflect_type_to_string(child.data_type->data_type);
            }
            cache.element_type_names.push_back(type_name);
            cache.element_offsets.push_back(child.data_offset);
        }
    }

    out_desc->data = scene_obj->shader_params();
    out_desc->data_size = dt->element_size;
    out_desc->shader_name = shader_name ? shader_name : dt->struct_typename;
    out_desc->element_names = cache.element_names.empty() ? nullptr : cache.element_names.data();
    out_desc->element_typenames = cache.element_type_names.empty() ? nullptr : cache.element_type_names.data();
    out_desc->element_offsets = cache.element_offsets.empty() ? nullptr : cache.element_offsets.data();
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
    return scene_obj->shader_params_data_type();
}

// ----------------------------------------------------------------------------
// Voxel BVH build params setters (public; declared in Pipeline.h)
//
// VoxelBVHBuildParams is intentionally kept private to this TU. Callers
// modify the params blob via these helpers so a future struct change (extra
// field, reordering) doesn't silently break editor code.
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

void pipeline_mark_dirty(SceneObject* obj)
{
    if (obj)
    {
        obj->process_dirty() = true;
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
    return with_runtime_or_warn("pipeline_load",
                                [&](PipelineRuntime& rt)
                                {
                                    for (const auto& worker : rt.workers())
                                    {
                                        if (worker && worker->start_from_request(request, scene_manager, scene_token))
                                        {
                                            return true;
                                        }
                                    }
                                    Console::getInstance().addLog(
                                        Console::LogLevel::Warning,
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

    for (const auto& worker : rt->workers())
    {
        if (!worker)
        {
            continue;
        }
        if (worker->is_running())
        {
            worker->get_progress(progress_text, progress_value);
            return true;
        }
        if (worker->handle_completion())
        {
            progress_text.clear();
            progress_value = 0.0f;
            return false;
        }
    }

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
