// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   editor/Pipeline.cpp

    \author Petra Hapalova, Andrew Reidmeyer

    \brief  This file provides pipeline interface and registration system.
*/

#include "Pipeline.h"
#include "Editor.h"
#include "EditorImport.h"
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
    pnanovdb_uint32_t source_type = 0u; // pnanovdb_pipeline_voxelbvh_source_t
    pnanovdb_uint32_t resolution = pnanovdb_editor::k_default_bvh_resolution;
    float inflation_radius = 0.f;
};

#define PNANOVDB_REFLECT_TYPE VoxelBVHBuildParams
PNANOVDB_REFLECT_BEGIN()
PNANOVDB_REFLECT_VALUE(pnanovdb_uint32_t, source_type, 0, 0)
PNANOVDB_REFLECT_VALUE(pnanovdb_uint32_t, resolution, 0, 0)
PNANOVDB_REFLECT_VALUE(float, inflation_radius, 0, 0)
PNANOVDB_REFLECT_END(0)
#undef PNANOVDB_REFLECT_TYPE

struct VoxelBVHRgba8Params
{
    pnanovdb_uint32_t resolution = pnanovdb_editor::k_default_bvh_resolution;
    pnanovdb_bool_t upsample = PNANOVDB_TRUE; // duplicate topology at 2x resolution before filling
};

#define PNANOVDB_REFLECT_TYPE VoxelBVHRgba8Params
PNANOVDB_REFLECT_BEGIN()
PNANOVDB_REFLECT_VALUE(pnanovdb_uint32_t, resolution, 0, 0)
PNANOVDB_REFLECT_VALUE(pnanovdb_bool_t, upsample, 0, 0)
PNANOVDB_REFLECT_END(0)
#undef PNANOVDB_REFLECT_TYPE


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

namespace
{

bool has_named_array(const SceneObject* obj, const char* name)
{
    if (!obj)
        return false;
    const auto it = obj->resources.named_arrays.find(name);
    return it != obj->resources.named_arrays.end() && it->second != nullptr;
}

bool source_resources_available(const SceneObject* obj, SceneObjectSourceKind source)
{
    if (!obj)
        return false;

    switch (source)
    {
    case SceneObjectSourceKind::NanoVDB:
        return obj->resources.nanovdb_array != nullptr || obj->resources.converted_nanovdb != nullptr;
    case SceneObjectSourceKind::GaussianData:
        return obj->resources.gaussian_data != nullptr;
    case SceneObjectSourceKind::GaussianFile:
        return obj->pipeline.load().type == pnanovdb_pipeline_type_gaussian_load &&
               !obj->resources.source_filepath.empty();
    case SceneObjectSourceKind::GaussianArrays:
    {
        static constexpr const char* required[] = { "means", "opacities", "quaternions", "scales", "sh_0", "sh_n" };
        return std::all_of(
            std::begin(required), std::end(required), [obj](const char* name) { return has_named_array(obj, name); });
    }
    case SceneObjectSourceKind::MeshTriangles:
    case SceneObjectSourceKind::MeshLines:
    {
        if (!has_named_array(obj, "indices") || !has_named_array(obj, "positions"))
            return false;
        const auto* indices = obj->resources.named_arrays.at("indices");
        const bool line_indices = indices->element_size == 2u * sizeof(uint32_t);
        return source == SceneObjectSourceKind::MeshLines ? line_indices : !line_indices;
    }
    case SceneObjectSourceKind::None:
        return false;
    }
    return false;
}

SceneObjectSourceKind voxelbvh_source_kind(pnanovdb_pipeline_voxelbvh_source_t source)
{
    switch (source)
    {
    case pnanovdb_pipeline_voxelbvh_source_gaussian_file:
        return SceneObjectSourceKind::GaussianFile;
    case pnanovdb_pipeline_voxelbvh_source_triangles:
        return SceneObjectSourceKind::MeshTriangles;
    case pnanovdb_pipeline_voxelbvh_source_lines:
        return SceneObjectSourceKind::MeshLines;
    case pnanovdb_pipeline_voxelbvh_source_gaussian_arrays:
        return SceneObjectSourceKind::GaussianArrays;
    }
    return SceneObjectSourceKind::None;
}

} // namespace

bool pnanovdb_editor::process_pipeline_supports_object(const SceneObject* obj, pnanovdb_pipeline_type_t type)
{
    if (!obj)
        return false;

    const SceneObjectSourceKind source = obj->source_kind();
    switch (type)
    {
    case pnanovdb_pipeline_type_voxelbvh_rgba8:
        return source == SceneObjectSourceKind::MeshTriangles && source_resources_available(obj, source);
    case pnanovdb_pipeline_type_voxelbvh_build:
        return (source == SceneObjectSourceKind::GaussianFile || source == SceneObjectSourceKind::GaussianArrays ||
                source == SceneObjectSourceKind::MeshTriangles || source == SceneObjectSourceKind::MeshLines) &&
               source_resources_available(obj, source);
    case pnanovdb_pipeline_type_gaussian_voxelize:
        return source == SceneObjectSourceKind::GaussianFile && source_resources_available(obj, source);
    default:
        return true;
    }
}

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

    auto& process_params = scene_obj->pipeline.process_step((size_t)scene_obj->pipeline.active_process_step).params;
    if (!process_params.data || process_params.size < sizeof(VoxelBVHBuildParams))
    {
        free(process_params.data);
        process_params.data = nullptr;
        process_params.size = 0;
        init_params_t<VoxelBVHBuildParams>(&process_params);
    }
    const auto* build_params = static_cast<const VoxelBVHBuildParams*>(process_params.data);

    if (build_params->source_type > pnanovdb_pipeline_voxelbvh_source_gaussian_arrays)
    {
        Console::getInstance().addLog(
            Console::LogLevel::Error, "VoxelBVH build: invalid source_type %u", build_params->source_type);
        scene_obj->process_dirty() = false;
        return pnanovdb_pipeline_result_error;
    }
    const int source_type = (int)build_params->source_type;
    const auto source = static_cast<pnanovdb_pipeline_voxelbvh_source_t>(source_type);
    if (!source_resources_available(scene_obj, voxelbvh_source_kind(source)))
    {
        Console::getInstance().addLog(
            Console::LogLevel::Error, "VoxelBVH build: source resources are incomplete (source_type=%d)", source_type);
        scene_obj->process_dirty() = false;
        return pnanovdb_pipeline_result_no_data;
    }
    const pnanovdb_uint32_t resolution = pnanovdb_pipeline_voxelbvh_sanitize_resolution(build_params->resolution);
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

static pnanovdb_pipeline_result_t execute_voxelbvh_rgba8(pnanovdb_scene_object_t* obj, pnanovdb_pipeline_context_t* ctx)
{
    auto* scene_obj = cast(obj);
    if (!scene_obj)
    {
        return pnanovdb_pipeline_result_no_data;
    }
    if (!process_pipeline_supports_object(scene_obj, pnanovdb_pipeline_type_voxelbvh_rgba8))
    {
        Console::getInstance().addLog(
            Console::LogLevel::Error, "VoxelBVH->RGBA8: triangle source resources are incomplete");
        return pnanovdb_pipeline_result_no_data;
    }
    if (!ctx || !ctx->voxelbvh || !ctx->voxelbvh_ctx || !ctx->compute || !ctx->queue)
    {
        Console::getInstance().addLog(
            Console::LogLevel::Error, "VoxelBVH->RGBA8: missing voxelbvh interface or compute context");
        return pnanovdb_pipeline_result_error;
    }
    auto* scene_manager = cast(ctx->scene_manager);
    if (!scene_manager)
    {
        return pnanovdb_pipeline_result_error;
    }

    const bool already_busy = with_runtime(false, [](PipelineRuntime& rt) { return rt.any_worker_busy(); });
    if (already_busy)
    {
        return pnanovdb_pipeline_result_pending;
    }

    const int step = scene_obj->pipeline.active_process_step;
    pnanovdb_compute_array_t* src = nullptr;
    std::shared_ptr<pnanovdb_compute_array_t> src_owner;
    bool has_voxelbvh_producer = false;
    if (step > 0)
    {
        const auto& below = scene_obj->pipeline.process_step((size_t)(step - 1)).output;
        src = below.get_array(pnanovdb_editor::k_stage_output_nanovdb);
        src_owner = below.get_array_owner(pnanovdb_editor::k_stage_output_nanovdb);
        for (size_t i = (size_t)step; i-- > 0;)
        {
            const pnanovdb_pipeline_type_t type = scene_obj->pipeline.process_step(i).type;
            if (type != pnanovdb_pipeline_type_noop)
            {
                has_voxelbvh_producer = type == pnanovdb_pipeline_type_voxelbvh_build;
                break;
            }
        }
    }
    if (!src)
    {
        src = scene_obj->nanovdb_array();
        src_owner = scene_obj->resources.nanovdb_array_owner;
    }
    if (!src)
    {
        Console::getInstance().addLog(
            Console::LogLevel::Error, "VoxelBVH->RGBA8: no input NanoVDB grid available for conversion");
        return pnanovdb_pipeline_result_no_data;
    }
    if (!has_voxelbvh_producer || scene_obj->source_kind() != SceneObjectSourceKind::MeshTriangles ||
        !nanovdb_import::has_voxelbvh_mesh_metadata(src))
    {
        Console::getInstance().addLog(
            Console::LogLevel::Error, "VoxelBVH->RGBA8: input is not a supported triangle VoxelBVH grid");
        return pnanovdb_pipeline_result_no_data;
    }

    auto& params = scene_obj->pipeline.process_step((size_t)step).params;
    if (!params.data || params.size < sizeof(VoxelBVHRgba8Params))
    {
        free(params.data);
        params.data = nullptr;
        params.size = 0;
        init_params_t<VoxelBVHRgba8Params>(&params);
    }
    const auto* p = static_cast<const VoxelBVHRgba8Params*>(params.data);
    pnanovdb_uint32_t resolution = p->resolution;
    if (resolution < 1u)
        resolution = 1u;
    if (resolution > pnanovdb_editor::k_max_bvh_resolution)
        resolution = pnanovdb_editor::k_max_bvh_resolution;
    const pnanovdb_bool_t upsample = p->upsample ? PNANOVDB_TRUE : PNANOVDB_FALSE;

    const bool started = with_runtime_or_warn(
        "execute_voxelbvh_rgba8",
        [&](PipelineRuntime& rt)
        {
            auto* w = rt.worker<VoxelBVHRgba8Worker>();
            return w && w->start(scene_obj, scene_manager, ctx, src, src_owner, resolution, upsample);
        });
    if (!started)
    {
        Console::getInstance().addLog(Console::LogLevel::Error, "VoxelBVH->RGBA8: worker start failed; will retry");
    }
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
        if (scene_obj)
            scene_obj->process_dirty() = false;
        return pnanovdb_pipeline_result_no_data;
    }

    if (!process_pipeline_supports_object(scene_obj, pnanovdb_pipeline_type_gaussian_voxelize))
    {
        Console::getInstance().addLog(
            Console::LogLevel::Error, "Gaussian->NanoVDB skipped: source is not a complete Gaussian file object");
        scene_obj->process_dirty() = false;
        return pnanovdb_pipeline_result_no_data;
    }

    auto* scene_manager = cast(ctx->scene_manager);

    if (!scene_manager)
    {
        Console::getInstance().addLog(
            Console::LogLevel::Error, "Gaussian voxelize processing failed: missing scene_manager");
        // Terminal condition: clear dirty so we don't retry every frame.
        scene_obj->process_dirty() = false;
        return pnanovdb_pipeline_result_error;
    }

    if (scene_obj->load_pipeline() == pnanovdb_pipeline_type_mesh_load)
    {
        Console::getInstance().addLog(Console::LogLevel::Error,
                                      "Gaussian->NanoVDB skipped: '%s' was loaded as a mesh, not Gaussian data. "
                                      "Use 'VoxelBVH build' to process a mesh.",
                                      scene_obj->resources.source_filepath.c_str());
        scene_obj->process_dirty() = false;
        return pnanovdb_pipeline_result_error;
    }

    // Ensure process params are allocated (may be missing if pipeline type was changed after creation).
    // Use the active process step's params (this pipeline may be placed at any step in a chain), not
    // unconditionally step 0 (process_params()).
    auto& process_params = scene_obj->pipeline.process_step((size_t)scene_obj->pipeline.active_process_step).params;
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
    if (source < pnanovdb_pipeline_voxelbvh_source_gaussian_file ||
        source > pnanovdb_pipeline_voxelbvh_source_gaussian_arrays)
        return false;
    if (!ensure_voxelbvh_build_params(params))
        return false;
    static_cast<VoxelBVHBuildParams*>(params->data)->source_type = static_cast<pnanovdb_uint32_t>(source);
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
    static_cast<VoxelBVHBuildParams*>(params->data)->resolution =
        pnanovdb_pipeline_voxelbvh_sanitize_resolution(resolution);
    return true;
}

pnanovdb_uint32_t pnanovdb_pipeline_voxelbvh_sanitize_resolution(pnanovdb_uint32_t resolution)
{
    return std::clamp(resolution, 1u, (pnanovdb_uint32_t)PNANOVDB_VOXELBVH_MAX_RESOLUTION);
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

    const int step = obj->next_dirty_process_step();
    if (step < 0)
        return pnanovdb_pipeline_result_skipped;
    obj->pipeline.active_process_step = step;

    pnanovdb_pipeline_context_t pipeline_ctx = {
        ctx.compute,    ctx.device,   ctx.queue,        ctx.compute_queue,  ctx.raster,
        ctx.raster_ctx, ctx.voxelbvh, ctx.voxelbvh_ctx, cast(ctx.renderer), cast(ctx.scene_manager)
    };
    return pnanovdb_pipeline_execute(obj->pipeline.process_step((size_t)step).type, cast(obj), &pipeline_ctx);
}

bool pipeline_needs_process(SceneObject* obj)
{
    return obj && obj->next_dirty_process_step() >= 0;
}

namespace
{
bool is_async_process_pipeline(pnanovdb_pipeline_type_t type)
{
    return type == pnanovdb_pipeline_type_gaussian_voxelize || type == pnanovdb_pipeline_type_voxelbvh_build ||
           type == pnanovdb_pipeline_type_voxelbvh_rgba8;
}

struct PipelineObjectIdentity
{
    pnanovdb_editor_token_t* scene = nullptr;
    pnanovdb_editor_token_t* name = nullptr;
    uint64_t lifetime_id = 0;
};

bool worker_targets_object(AsyncWorker* worker, const PipelineObjectIdentity& target)
{
    if (!worker || !target.scene || !target.name || target.lifetime_id == 0)
    {
        return false;
    }
    return worker->pending_target_matches(target.scene->id, target.name->id, target.lifetime_id);
}

bool async_process_worker_in_flight(const AsyncWorker* worker)
{
    return worker && worker->is_busy() && !worker->pending_completion();
}

AsyncWorker* active_async_process_worker_for_object(PipelineRuntime& rt, const PipelineObjectIdentity& target)
{
    if (!target.scene || !target.name || target.lifetime_id == 0)
    {
        return nullptr;
    }
    for (const auto& w : rt.workers())
    {
        if (!w || !is_async_process_pipeline(w->pipeline_type()))
        {
            continue;
        }
        if (!worker_targets_object(w.get(), target))
        {
            continue;
        }
        if (async_process_worker_in_flight(w.get()))
        {
            return w.get();
        }
    }
    return nullptr;
}

AsyncWorker* resolve_async_process_worker(PipelineRuntime& rt, const PipelineObjectIdentity& target)
{
    if (!target.scene || !target.name || target.lifetime_id == 0)
    {
        return nullptr;
    }
    for (const auto& w : rt.workers())
    {
        if (w && is_async_process_pipeline(w->pipeline_type()) && w->is_busy() && worker_targets_object(w.get(), target))
        {
            return w.get();
        }
    }
    return nullptr;
}

bool pipeline_object_cancel_in_flight(const SceneObject* obj)
{
    if (!obj || !obj->scene_token || !obj->name_token)
    {
        return false;
    }
    return with_runtime(
        false,
        [&](PipelineRuntime& rt) -> bool
        {
            for (const auto& w : rt.workers())
            {
                if (!w || !w->user_cancel_requested() || !async_process_worker_in_flight(w.get()))
                {
                    continue;
                }
                if (w->pending_target_matches(obj->scene_token->id, obj->name_token->id, obj->lifetime_id))
                {
                    return true;
                }
            }
            return false;
        });
}

PipelineObjectIdentity canonical_object_identity(EditorSceneManager* scene_manager,
                                                 pnanovdb_editor_token_t* scene,
                                                 pnanovdb_editor_token_t* name)
{
    PipelineObjectIdentity result{ scene, name, 0 };
    if (!scene_manager || !scene || !name)
    {
        return result;
    }
    scene_manager->with_object(scene, name,
                               [&](SceneObject* obj)
                               {
                                   if (obj && obj->scene_token && obj->name_token)
                                   {
                                       result.scene = obj->scene_token;
                                       result.name = obj->name_token;
                                       result.lifetime_id = obj->lifetime_id;
                                   }
                               });
    return result;
}
} // namespace

void pipeline_execute_pending(EditorSceneManager* manager, const PipelineContext& ctx)
{
    if (!manager)
    {
        return;
    }

    std::vector<uint64_t> terminal_replacement_failures;
    manager->for_each_object(
        [&ctx, &terminal_replacement_failures](SceneObject* obj)
        {
            if (pipeline_needs_process(obj) && !pipeline_object_cancel_in_flight(obj))
            {
                auto result = pipeline_execute_process(obj, ctx);
                if (result == pnanovdb_pipeline_result_success || result == pnanovdb_pipeline_result_skipped)
                {
                    obj->advance_process_chain(true);
                }
                else if (result == pnanovdb_pipeline_result_error || result == pnanovdb_pipeline_result_no_data)
                {
                    obj->advance_process_chain(false);
                    terminal_replacement_failures.push_back(obj->lifetime_id);
                }
            }
            return true;
        });

    for (uint64_t lifetime_id : terminal_replacement_failures)
    {
        manager->finish_file_object_replacement(lifetime_id, false);
    }
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
                                   rt.set_editor_scene(editor_scene);
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

            EditorScene* editor_scene = rt.editor_scene();
            pnanovdb_editor_token_t* target_name = request.name_token;
            if (!target_name && request.source_filepath)
            {
                const std::string stem = std::filesystem::path(request.source_filepath).stem().string();
                target_name = EditorToken::getInstance().getToken(stem.c_str());
            }
            if (!editor_scene || !scene_token || !target_name)
            {
                Console::getInstance().addLog(
                    Console::LogLevel::Error, "pipeline_load: cannot resolve async load target");
                return false;
            }

            PipelineLoadRequest reserved_request = request;
            reserved_request.name_token = target_name;
            reserved_request.reservation_id =
                editor_scene->reserve_async_load_target(scene_token, target_name, request.replace_existing);
            if (!reserved_request.reservation_id)
            {
                Console::getInstance().addLog(
                    Console::LogLevel::Error, "pipeline_load: object name '%s' is already in use in scene '%s'",
                    target_name->str ? target_name->str : "?", scene_token->str ? scene_token->str : "?");
                return false;
            }
            for (const auto& worker : rt.workers())
            {
                if (worker && worker->start_from_request(reserved_request, scene_manager, scene_token))
                {
                    return true;
                }
            }
            editor_scene->finish_async_load_target(reserved_request.reservation_id, false);
            Console::getInstance().addLog(Console::LogLevel::Warning,
                                          "pipeline_load: no load worker registered for pipeline type %u",
                                          (unsigned)request.load_pipeline);
            return false;
        });
}

bool pipeline_load_available()
{
    return with_runtime(false, [](PipelineRuntime& rt) { return !rt.any_worker_busy(); });
}

bool pipeline_update(std::string& progress_text, float& progress_value)
{
    PipelineRuntime* rt = current_runtime();
    if (!rt)
    {
        return false;
    }

    rt->handle_completions();
    AsyncWorker* active = rt->running_worker();
    if (active)
    {
        if (is_async_process_pipeline(active->pipeline_type()) && active->user_cancel_requested())
        {
            progress_text.clear();
            progress_value = 0.0f;
            return false;
        }
        active->get_progress(progress_text, progress_value);
        return true;
    }

    progress_text.clear();
    progress_value = 0.0f;
    return false;
}

bool pipeline_async_process_running_for(EditorSceneManager* scene_manager,
                                        pnanovdb_editor_token_t* scene,
                                        pnanovdb_editor_token_t* name)
{
    const PipelineObjectIdentity target = canonical_object_identity(scene_manager, scene, name);
    return with_runtime(
        false, [&](PipelineRuntime& rt) -> bool { return resolve_async_process_worker(rt, target) != nullptr; });
}

bool pipeline_async_process_cancelling_for(EditorSceneManager* scene_manager,
                                           pnanovdb_editor_token_t* scene,
                                           pnanovdb_editor_token_t* name)
{
    const PipelineObjectIdentity target = canonical_object_identity(scene_manager, scene, name);
    return with_runtime(false,
                        [&](PipelineRuntime& rt) -> bool
                        {
                            if (!target.scene || !target.name || target.lifetime_id == 0)
                            {
                                return false;
                            }
                            for (const auto& w : rt.workers())
                            {
                                if (!w || !is_async_process_pipeline(w->pipeline_type()))
                                {
                                    continue;
                                }
                                if (!worker_targets_object(w.get(), target))
                                {
                                    continue;
                                }
                                if (w->user_cancel_requested() && async_process_worker_in_flight(w.get()))
                                {
                                    return true;
                                }
                            }
                            return false;
                        });
}

bool pipeline_async_process_cancel_available_for(EditorSceneManager* scene_manager,
                                                 pnanovdb_editor_token_t* scene,
                                                 pnanovdb_editor_token_t* name)
{
    const PipelineObjectIdentity target = canonical_object_identity(scene_manager, scene, name);
    return with_runtime(false,
                        [&](PipelineRuntime& rt) -> bool
                        {
                            AsyncWorker* worker = resolve_async_process_worker(rt, target);
                            return worker && (worker->pending_completion() || worker->supports_user_cancel());
                        });
}

bool pipeline_cancel_async_process(EditorSceneManager* scene_manager,
                                   pnanovdb_editor_token_t* scene,
                                   pnanovdb_editor_token_t* name)
{
    if (!scene_manager || !scene || !name)
    {
        return false;
    }

    const PipelineObjectIdentity target = canonical_object_identity(scene_manager, scene, name);

    return with_runtime(
        false,
        [&](PipelineRuntime& rt) -> bool
        {
            AsyncWorker* worker = resolve_async_process_worker(rt, target);
            if (!worker)
            {
                return false;
            }
            if (!worker->pending_completion() && !worker->supports_user_cancel())
            {
                return false;
            }
            worker->request_user_cancel();
            scene_manager->with_object_lifetime(target.scene, target.name, target.lifetime_id,
                                                [](SceneObject* obj)
                                                {
                                                    if (obj)
                                                        obj->process_user_cancel();
                                                });
            const pnanovdb_pipeline_descriptor_t* desc = pnanovdb_pipeline_get_descriptor(worker->pipeline_type());
            Console::getInstance().addLog("Cancelling %s...", (desc && desc->name) ? desc->name : "process task");
            if (worker->pending_completion())
            {
                worker->handle_completion();
            }
            return true;
        });
}

bool pipeline_retarget_async_process_target(pnanovdb_editor_token_t* old_scene,
                                            pnanovdb_editor_token_t* old_name,
                                            pnanovdb_editor_token_t* new_scene,
                                            pnanovdb_editor_token_t* new_name,
                                            uint64_t lifetime_id)
{
    if (!old_scene || !old_name || !new_scene || !new_name || lifetime_id == 0)
        return false;
    return with_runtime(false,
                        [&](PipelineRuntime& rt)
                        {
                            bool retargeted = false;
                            for (const auto& worker : rt.workers())
                            {
                                if (worker && is_async_process_pipeline(worker->pipeline_type()))
                                {
                                    retargeted |= worker->retarget_pending_target(
                                        old_scene->id, old_name->id, new_scene->id, new_name->id, lifetime_id);
                                }
                            }
                            return retargeted;
                        });
}

void pipeline_process_pending_user_cancels(EditorSceneManager* scene_manager)
{
    if (!scene_manager)
    {
        return;
    }

    struct PendingCancel
    {
        pnanovdb_editor_token_t* scene;
        pnanovdb_editor_token_t* name;
    };
    std::vector<PendingCancel> pending;
    scene_manager->for_each_object(
        [&pending](SceneObject* obj) -> bool
        {
            if (obj && obj->pipeline.process_user_cancel_requested && obj->scene_token && obj->name_token)
            {
                pending.push_back({ obj->scene_token, obj->name_token });
            }
            return true;
        });

    for (const PendingCancel& pc : pending)
    {
        const PipelineObjectIdentity target = canonical_object_identity(scene_manager, pc.scene, pc.name);

        const bool accepted =
            with_runtime(false,
                         [&](PipelineRuntime& rt)
                         {
                             AsyncWorker* worker = resolve_async_process_worker(rt, target);
                             if (!worker || (!worker->pending_completion() && !worker->supports_user_cancel()))
                             {
                                 return false;
                             }
                             worker->request_user_cancel();
                             if (worker->pending_completion())
                             {
                                 worker->handle_completion();
                             }
                             return true;
                         });

        if (!accepted)
        {
            scene_manager->with_object_lifetime(target.scene, target.name, target.lifetime_id,
                                                [](SceneObject* obj)
                                                {
                                                    if (obj)
                                                        obj->clear_process_cancel_state();
                                                });
            continue;
        }

        if (pipeline_async_process_cancelling_for(scene_manager, pc.scene, pc.name))
        {
            continue;
        }
        if (pipeline_async_process_running_for(scene_manager, pc.scene, pc.name))
        {
            (void)pipeline_cancel_async_process(scene_manager, pc.scene, pc.name);
            continue;
        }
        scene_manager->with_object(pc.scene, pc.name,
                                   [](SceneObject* o)
                                   {
                                       if (o)
                                       {
                                           o->clear_process_cancel_state();
                                       }
                                   });
    }
}

bool pipeline_create_variant(EditorSceneManager* scene_manager,
                             pnanovdb_editor_token_t* scene_token,
                             pnanovdb_editor_token_t* source_name,
                             const char* new_name)
{
    if (!scene_manager || !scene_token || !source_name || !new_name)
    {
        Console::getInstance().addLog(Console::LogLevel::Error, "pipeline_create_variant: null argument");
        return false;
    }

    std::string source_filepath;
    PipelineStage source_load;
    std::vector<PipelineStage> source_process_steps;
    std::map<std::string, pnanovdb_compute_array_t*> source_named_arrays;
    std::map<std::string, pnanovdb_compute_array_t*> source_file_backed_named_arrays;
    std::map<std::string, std::shared_ptr<pnanovdb_compute_array_t>> source_named_array_owners;
    SceneObjectType source_object_type = SceneObjectType::Uninitialized;
    pnanovdb_pipeline_type_t source_render_type = pnanovdb_pipeline_type_nanovdb_render;
    bool source_drop_intermediate = false;
    bool source_found = false;

    scene_manager->with_object(scene_token, source_name,
                               [&](SceneObject* src)
                               {
                                   if (!src)
                                       return;
                                   source_found = true;
                                   source_filepath = src->resources.source_filepath;
                                   source_load = src->pipeline.load();
                                   source_named_arrays = src->resources.named_arrays;
                                   source_file_backed_named_arrays = src->resources.file_backed_named_arrays;
                                   source_named_array_owners = src->resources.named_array_owners;
                                   source_object_type = src->type;
                                   source_render_type = src->render_pipeline();
                                   source_drop_intermediate = src->pipeline.drop_intermediate;
                                   source_process_steps.reserve(src->pipeline.process_count());
                                   for (size_t i = 0; i < src->pipeline.process_count(); ++i)
                                       source_process_steps.push_back(src->pipeline.process_step(i));
                               });

    if (!source_found)
    {
        Console::getInstance().addLog(Console::LogLevel::Error, "Cannot create variant: source object '%s' not found",
                                      source_name->str ? source_name->str : "?");
        return false;
    }

    if (source_filepath.empty() && source_load.output.empty() && source_named_arrays.empty())
    {
        Console::getInstance().addLog(Console::LogLevel::Error,
                                      "Cannot create variant: source object '%s' has no reusable input data",
                                      source_name->str ? source_name->str : "?");
        return false;
    }

    pnanovdb_editor_token_t* new_name_token = pnanovdb_editor::EditorToken::getInstance().getToken(new_name);
    uint64_t reserved_lifetime_id = 0;
    if (!scene_manager->reserve_load_target(scene_token, new_name_token, &reserved_lifetime_id))
    {
        Console::getInstance().addLog(
            Console::LogLevel::Error, "Cannot create variant '%s': that object name is already in use", new_name);
        return false;
    }

    bool configured = false;
    scene_manager->with_object_lifetime(
        scene_token, new_name_token, reserved_lifetime_id,
        [&](SceneObject* obj)
        {
            if (!obj || obj->type != SceneObjectType::Uninitialized)
                return;
            obj->type = source_object_type;
            obj->resources.source_filepath = source_filepath;
            obj->pipeline.load() = source_load;
            obj->render_pipeline() = source_render_type;
            pnanovdb_pipeline_get_default_params(source_render_type, &obj->render_params());
            obj->set_named_array_bindings(source_named_arrays, source_named_array_owners);
            obj->resources.file_backed_named_arrays = source_file_backed_named_arrays;
            obj->pipeline.process() = source_process_steps.front();
            obj->pipeline.extra_process.assign(source_process_steps.begin() + 1, source_process_steps.end());
            obj->pipeline.active_process_step = 0;
            obj->pipeline.drop_intermediate = source_drop_intermediate;
            obj->pipeline.process_run_snapshot.reset();
            obj->pipeline.process_user_cancel_requested = false;
            for (size_t i = 0; i < obj->pipeline.process_count(); ++i)
            {
                PipelineStage& step = obj->pipeline.process_step(i);
                step.output.clear();
                step.bump_revision();
                step.dirty = step.type != pnanovdb_pipeline_type_noop;
            }
            obj->resolve_resources();
            configured = true;
        });

    if (!configured)
    {
        scene_manager->cancel_load_target(scene_token, new_name_token, reserved_lifetime_id);
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
#define PNANOVDB_PIPELINE_CHAIN(arr) (arr), (sizeof(arr) / sizeof((arr)[0]))
#define PNANOVDB_PIPELINE_NO_CHAIN nullptr, 0

// load stage: no shaders, no render method, params are never mapped to the object.
#define PNANOVDB_REGISTER_LOAD_PIPELINE(var, type_, name_, params_, execute_, outputs_)                                \
    static const pnanovdb_pipeline_descriptor_t var = {                                                                \
        (type_),                                                                                                       \
        pnanovdb_pipeline_stage_load,                                                                                  \
        (name_),                                                                                                       \
        nullptr,                                                                                                       \
        0,                                                                                                             \
        params_,                                                                                                       \
        (execute_),                                                                                                    \
        get_render_method_none,                                                                                        \
        nullptr,                                                                                                       \
        nullptr,                                                                                                       \
        PNANOVDB_PIPELINE_NO_CHAIN,                                                                                    \
        (outputs_),                                                                                                    \
        0u,                                                                                                            \
    };                                                                                                                 \
    PNANOVDB_REGISTER_PIPELINE(var)

// process stage: params are always mapped to SceneObject::process_params.
#define PNANOVDB_REGISTER_PROCESS_PIPELINE(                                                                            \
    var, type_, name_, shaders_, shader_count_, params_, execute_, render_method_, params_hints_, outputs_, inputs_)   \
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
        (params_hints_),                                                                                               \
        PNANOVDB_PIPELINE_NO_CHAIN,                                                                                    \
        (outputs_),                                                                                                    \
        (inputs_),                                                                                                     \
    };                                                                                                                 \
    PNANOVDB_REGISTER_PIPELINE(var)

// process chain: a template process pipeline that set_pipeline expands into the listed
// process sub-steps.
#define PNANOVDB_REGISTER_PROCESS_CHAIN_PIPELINE(var, type_, name_, chain_, outputs_, inputs_)                         \
    static const pnanovdb_pipeline_descriptor_t var = {                                                                \
        (type_),   pnanovdb_pipeline_stage_process, (name_), nullptr, 0,      PNANOVDB_PIPELINE_NO_PARAMS,             \
        nullptr,   get_render_method_nanovdb,       nullptr, nullptr, chain_, (outputs_),                              \
        (inputs_),                                                                                                     \
    };                                                                                                                 \
    PNANOVDB_REGISTER_PIPELINE(var)

// render stage: caller picks the render method and how params map to the object.
#define PNANOVDB_REGISTER_RENDER_PIPELINE(                                                                             \
    var, type_, name_, shaders_, shader_count_, params_, execute_, render_method_, map_, inputs_)                      \
    static const pnanovdb_pipeline_descriptor_t var = {                                                                \
        (type_),                                                                                                       \
        pnanovdb_pipeline_stage_render,                                                                                \
        (name_),                                                                                                       \
        (shaders_),                                                                                                    \
        (shader_count_),                                                                                               \
        params_,                                                                                                       \
        (execute_),                                                                                                    \
        (render_method_),                                                                                              \
        (map_),                                                                                                        \
        nullptr,                                                                                                       \
        PNANOVDB_PIPELINE_NO_CHAIN,                                                                                    \
        0u,                                                                                                            \
        (inputs_),                                                                                                     \
    };                                                                                                                 \
    PNANOVDB_REGISTER_PIPELINE(var)

// Common render case: draw an existing NanoVDB grid with NanoVDBRenderParams.
#define PNANOVDB_REGISTER_NANOVDB_RENDER_PIPELINE(var, type_, name_, shaders_, inputs_)                                \
    PNANOVDB_REGISTER_RENDER_PIPELINE(var, (type_), (name_), (shaders_), 1,                                            \
                                      PNANOVDB_PIPELINE_PARAMS(NanoVDBRenderParams), execute_nanovdb_render,           \
                                      get_render_method_nanovdb, map_params<&SceneObject::render_params>, (inputs_))

// ============================================================================
// Pipeline shader definitions
// ============================================================================

PNANOVDB_DEFINE_PIPELINE_SHADERS(s_nanovdb_render_shaders,
                                 PNANOVDB_PIPELINE_SHADER("editor/editor.slang", nullptr, PNANOVDB_TRUE));

PNANOVDB_DEFINE_PIPELINE_SHADERS(s_nanovdb_surface_shaders,
                                 PNANOVDB_PIPELINE_SHADER("editor/editor_surface.slang", nullptr, PNANOVDB_TRUE));

PNANOVDB_DEFINE_PIPELINE_SHADERS(s_image2d_render_shaders,
                                 PNANOVDB_PIPELINE_SHADER("editor/image2d.slang", nullptr, PNANOVDB_TRUE));

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

PNANOVDB_REGISTER_LOAD_PIPELINE(s_noop_descriptor,
                                pnanovdb_pipeline_type_noop,
                                "No Operation",
                                PNANOVDB_PIPELINE_NO_PARAMS,
                                execute_noop,
                                pnanovdb_pipeline_data_kind_none);

PNANOVDB_REGISTER_NANOVDB_RENDER_PIPELINE(s_nanovdb_render_descriptor,
                                          pnanovdb_pipeline_type_nanovdb_render,
                                          "NanoVDB Render",
                                          s_nanovdb_render_shaders,
                                          pnanovdb_pipeline_data_kind_nanovdb | pnanovdb_pipeline_data_kind_nanovdb_rgba8);

// SDF/level-set isosurface rendered via HDDA zero-crossing search.
PNANOVDB_REGISTER_NANOVDB_RENDER_PIPELINE(s_nanovdb_surface_descriptor,
                                          pnanovdb_pipeline_type_nanovdb_surface,
                                          "NanoVDB Surface (SDF)",
                                          s_nanovdb_surface_shaders,
                                          pnanovdb_pipeline_data_kind_nanovdb);

// Blits a NanoVDB image grid (RGBA stored as blind metadata) to a 2D texture.
PNANOVDB_REGISTER_NANOVDB_RENDER_PIPELINE(s_image2d_render_descriptor,
                                          pnanovdb_pipeline_type_image2d_render,
                                          "Image 2D",
                                          s_image2d_render_shaders,
                                          pnanovdb_pipeline_data_kind_nanovdb | pnanovdb_pipeline_data_kind_nanovdb_rgba8);

PNANOVDB_REGISTER_NANOVDB_RENDER_PIPELINE(s_voxelbvh_gaussians_render_descriptor,
                                          pnanovdb_pipeline_type_voxelbvh_gaussians_render,
                                          "Voxel BVH Gaussians",
                                          s_voxelbvh_gaussians_shaders,
                                          pnanovdb_pipeline_data_kind_voxelbvh);

PNANOVDB_REGISTER_NANOVDB_RENDER_PIPELINE(s_voxelbvh_lines_render_descriptor,
                                          pnanovdb_pipeline_type_voxelbvh_lines_render,
                                          "Voxel BVH Lines",
                                          s_voxelbvh_lines_render_shaders,
                                          pnanovdb_pipeline_data_kind_voxelbvh);

PNANOVDB_REGISTER_NANOVDB_RENDER_PIPELINE(s_voxelbvh_triangles_render_descriptor,
                                          pnanovdb_pipeline_type_voxelbvh_triangles_render,
                                          "Voxel BVH Triangles",
                                          s_voxelbvh_triangles_render_shaders,
                                          pnanovdb_pipeline_data_kind_voxelbvh);

PNANOVDB_REGISTER_NANOVDB_RENDER_PIPELINE(s_voxelbvh_triangles_debug_render_descriptor,
                                          pnanovdb_pipeline_type_voxelbvh_triangles_debug_render,
                                          "Voxel BVH Triangles Debug",
                                          s_voxelbvh_triangles_debug_render_shaders,
                                          pnanovdb_pipeline_data_kind_voxelbvh);

PNANOVDB_REGISTER_NANOVDB_RENDER_PIPELINE(s_voxelbvh_debug_render_descriptor,
                                          pnanovdb_pipeline_type_voxelbvh_debug_render,
                                          "Voxel BVH Debug",
                                          s_voxelbvh_debug_render_shaders,
                                          pnanovdb_pipeline_data_kind_voxelbvh);

// Gaussian splat draws the loaded Gaussians directly; params come from shader JSON.
PNANOVDB_REGISTER_RENDER_PIPELINE(s_gaussian_splat_descriptor,
                                  pnanovdb_pipeline_type_gaussian_splat,
                                  "Gaussian 2D Splatting",
                                  s_gaussian_splat_shaders,
                                  1,
                                  PNANOVDB_PIPELINE_NO_PARAMS,
                                  execute_gaussian_splat,
                                  get_render_method_gaussian,
                                  nullptr,
                                  pnanovdb_pipeline_data_kind_gaussian);

// Gaussian voxelize converts Gaussians to NanoVDB and then renders as NanoVDB.
PNANOVDB_REGISTER_PROCESS_PIPELINE(s_gaussian_voxelize_descriptor,
                                   pnanovdb_pipeline_type_gaussian_voxelize,
                                   "Gaussian to NanoVDB",
                                   s_gaussian_voxelize_shaders,
                                   1,
                                   PNANOVDB_PIPELINE_PARAMS(GaussianVoxelizeParams),
                                   execute_gaussian_voxelize,
                                   get_render_method_nanovdb,
                                   "editor/gaussian_voxelize.slang",
                                   pnanovdb_pipeline_data_kind_nanovdb,
                                   pnanovdb_pipeline_data_kind_gaussian);

PNANOVDB_REGISTER_PROCESS_PIPELINE(s_voxelbvh_build_descriptor,
                                   pnanovdb_pipeline_type_voxelbvh_build,
                                   "VoxelBVH Build",
                                   nullptr,
                                   0,
                                   PNANOVDB_PIPELINE_PARAMS(VoxelBVHBuildParams),
                                   execute_voxelbvh_build,
                                   get_render_method_nanovdb,
                                   "editor/voxelbvh_build.slang",
                                   pnanovdb_pipeline_data_kind_voxelbvh,
                                   pnanovdb_pipeline_data_kind_mesh | pnanovdb_pipeline_data_kind_gaussian);

// VoxelBVH -> RGBA8 converts a VoxelBVH NanoVDB grid into an RGBA8 NanoVDB image grid,
// then renders it as NanoVDB.
PNANOVDB_REGISTER_PROCESS_PIPELINE(s_voxelbvh_rgba8_descriptor,
                                   pnanovdb_pipeline_type_voxelbvh_rgba8,
                                   "VoxelBVH to RGBA8",
                                   nullptr,
                                   0,
                                   PNANOVDB_PIPELINE_PARAMS(VoxelBVHRgba8Params),
                                   execute_voxelbvh_rgba8,
                                   get_render_method_nanovdb,
                                   "editor/voxelbvh_rgba8.slang",
                                   pnanovdb_pipeline_data_kind_nanovdb_rgba8,
                                   pnanovdb_pipeline_data_kind_voxelbvh);

static const pnanovdb_pipeline_type_t s_voxelbvh_rgba8_chain_steps[] = {
    pnanovdb_pipeline_type_voxelbvh_build,
    pnanovdb_pipeline_type_voxelbvh_rgba8,
};
PNANOVDB_REGISTER_PROCESS_CHAIN_PIPELINE(s_voxelbvh_rgba8_chain_descriptor,
                                         pnanovdb_pipeline_type_voxelbvh_rgba8_chain,
                                         "VoxelBVH + RGBA8",
                                         PNANOVDB_PIPELINE_CHAIN(s_voxelbvh_rgba8_chain_steps),
                                         pnanovdb_pipeline_data_kind_nanovdb_rgba8,
                                         pnanovdb_pipeline_data_kind_mesh);


PNANOVDB_REGISTER_LOAD_PIPELINE(s_mesh_load_descriptor,
                                pnanovdb_pipeline_type_mesh_load,
                                "Mesh PLY Load",
                                PNANOVDB_PIPELINE_PARAMS(pnanovdb_editor::MeshLoadParams),
                                nullptr,
                                pnanovdb_pipeline_data_kind_mesh);

PNANOVDB_REGISTER_LOAD_PIPELINE(s_gaussian_load_descriptor,
                                pnanovdb_pipeline_type_gaussian_load,
                                "Gaussian File Load",
                                PNANOVDB_PIPELINE_NO_PARAMS,
                                nullptr,
                                pnanovdb_pipeline_data_kind_gaussian);
