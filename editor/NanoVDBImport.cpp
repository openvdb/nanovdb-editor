// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/editor/NanoVDBImport.cpp

    \author Petra Hapalova

    \brief
*/

#include "EditorImport.h"

#include "EditorScene.h"
#include "EditorToken.h"
#include "Console.h"

#include "nanovdb_editor/putil/Compute.h"

#define PNANOVDB_C
#include <nanovdb/PNanoVDB.h>
#undef PNANOVDB_C

#include <cmath>
#include <cstring>
#include <cstdint>
#include <filesystem>

namespace pnanovdb_editor
{
namespace nanovdb_import
{
namespace
{

pnanovdb_uint32_t required_blind_metadata_count(pnanovdb_pipeline_type_t render_pipeline)
{
    switch (render_pipeline)
    {
    case pnanovdb_pipeline_type_voxelbvh_gaussians_render:
        return 8u; // gaussian: range, prim_id, mean, opacity, quat, scale, sh0, shn
    case pnanovdb_pipeline_type_voxelbvh_lines_render:
    case pnanovdb_pipeline_type_voxelbvh_triangles_render:
    case pnanovdb_pipeline_type_voxelbvh_triangles_debug_render:
        return 5u; // mesh: range, prim_id, indices, positions, colors
    default:
        return 0u; // nanovdb_render, voxelbvh_debug_render, etc.
    }
}

bool is_voxelbvh_metadata(const pnanovdb_compute_array_t* array, pnanovdb_uint32_t required_count)
{
    if (!array || !array->data || array->element_count == 0u || array->element_size == 0u)
    {
        return false;
    }
    const pnanovdb_uint64_t grid_bytes = array->element_size * array->element_count;
    const pnanovdb_uint64_t uint32_count = grid_bytes / 4u;
    if (uint32_count == 0u)
    {
        return false;
    }

    pnanovdb_buf_t buf = pnanovdb_make_buf(static_cast<pnanovdb_uint32_t*>(array->data), uint32_count);
    pnanovdb_grid_handle_t grid = { pnanovdb_address_null() };

    if (pnanovdb_grid_get_blind_metadata_count(buf, grid) < required_count)
    {
        return false;
    }
    const pnanovdb_int64_t meta_off_signed = pnanovdb_grid_get_blind_metadata_offset(buf, grid);
    if (meta_off_signed <= 0)
    {
        return false;
    }
    const pnanovdb_uint64_t reported_grid_size = pnanovdb_grid_get_grid_size(buf, grid);
    const pnanovdb_uint64_t grid_size =
        (reported_grid_size > 0u && reported_grid_size <= grid_bytes) ? reported_grid_size : grid_bytes;
    const pnanovdb_uint64_t meta_off = static_cast<pnanovdb_uint64_t>(meta_off_signed);
    if (meta_off > grid_size ||
        static_cast<pnanovdb_uint64_t>(PNANOVDB_GRIDBLINDMETADATA_SIZE) * required_count > grid_size - meta_off)
    {
        return false;
    }

    for (pnanovdb_uint32_t i = 0u; i < required_count; ++i)
    {
        pnanovdb_gridblindmetadata_handle_t meta = pnanovdb_grid_get_gridblindmetadata(buf, grid, i);

        const pnanovdb_uint32_t value_size = pnanovdb_gridblindmetadata_get_value_size(buf, meta);
        const pnanovdb_uint64_t value_count = pnanovdb_gridblindmetadata_get_value_count(buf, meta);
        const pnanovdb_uint32_t data_class = pnanovdb_gridblindmetadata_get_data_class(buf, meta);
        const pnanovdb_int64_t data_off_signed = pnanovdb_gridblindmetadata_get_data_offset(buf, meta);

        if (data_class == 3u || value_count == 0u || data_off_signed <= 0)
        {
            return false;
        }
        const pnanovdb_uint32_t expected_size = (i == 0u) ? 8u : 4u;
        if (value_size != expected_size)
        {
            return false;
        }

        const pnanovdb_uint64_t entry_base =
            meta_off + static_cast<pnanovdb_uint64_t>(PNANOVDB_GRIDBLINDMETADATA_SIZE) * i;
        const pnanovdb_uint64_t data_off = static_cast<pnanovdb_uint64_t>(data_off_signed);
        if (entry_base > grid_size || data_off > grid_size - entry_base)
        {
            return false;
        }
        const pnanovdb_uint64_t payload_bytes = static_cast<pnanovdb_uint64_t>(value_size) * value_count;
        if (payload_bytes > grid_size - (entry_base + data_off))
        {
            return false;
        }
    }

    return true;
}

} // namespace

bool has_voxelbvh_mesh_metadata(const pnanovdb_compute_array_t* array)
{
    return is_voxelbvh_metadata(array, 5u);
}

bool has_voxelbvh_render_metadata(const pnanovdb_compute_array_t* array, pnanovdb_pipeline_type_t render_pipeline)
{
    const pnanovdb_uint32_t required_metadata = required_blind_metadata_count(render_pipeline);
    return required_metadata > 0u && is_voxelbvh_metadata(array, required_metadata);
}

pnanovdb_vec3_t world_dir_to_index_dir(const pnanovdb_compute_array_t* src_nanovdb, pnanovdb_vec3_t world_dir)
{
    const pnanovdb_vec3_t fallback = { 0.f, 0.f, -1.f };
    if (!src_nanovdb || !src_nanovdb->data || src_nanovdb->element_size == 0u || src_nanovdb->element_count == 0u)
    {
        return fallback;
    }

    const pnanovdb_uint64_t uint32_count = (src_nanovdb->element_size * src_nanovdb->element_count) / 4u;
    if (uint32_count == 0u)
    {
        return fallback;
    }

    pnanovdb_buf_t buf = pnanovdb_make_buf(static_cast<pnanovdb_uint32_t*>(src_nanovdb->data), uint32_count);
    pnanovdb_grid_handle_t grid = { pnanovdb_address_null() };

    pnanovdb_vec3_t index_dir = pnanovdb_grid_world_to_index_dirf(buf, grid, PNANOVDB_REF(world_dir));
    const float len = sqrtf(index_dir.x * index_dir.x + index_dir.y * index_dir.y + index_dir.z * index_dir.z);
    if (!(len > 1e-6f))
    {
        return fallback;
    }
    index_dir.x /= len;
    index_dir.y /= len;
    index_dir.z /= len;
    return index_dir;
}

pnanovdb_compute_array_t* merge_grids(const pnanovdb_compute_t* compute,
                                      pnanovdb_compute_array_t* const* grids,
                                      pnanovdb_uint32_t grid_count)
{
    if (!compute || !compute->create_array || !grids || grid_count == 0u)
    {
        return nullptr;
    }

    pnanovdb_uint64_t total_size = 0llu;
    for (pnanovdb_uint32_t i = 0u; i < grid_count; ++i)
    {
        pnanovdb_compute_array_t* grid_array = grids[i];
        if (!grid_array || !grid_array->data || grid_array->element_size == 0u || grid_array->element_count == 0u)
        {
            return nullptr;
        }
        const pnanovdb_uint64_t words = (grid_array->element_size * grid_array->element_count) / 4u;
        pnanovdb_buf_t buf = pnanovdb_make_buf(static_cast<pnanovdb_uint32_t*>(grid_array->data), words);
        pnanovdb_grid_handle_t grid = { pnanovdb_address_null() };
        pnanovdb_grid_set_grid_index(buf, grid, i);
        pnanovdb_grid_set_grid_count(buf, grid, grid_count);
        total_size += pnanovdb_grid_get_grid_size(buf, grid);
    }
    if (total_size == 0llu)
    {
        return nullptr;
    }

    const pnanovdb_uint64_t merged_uint64_count = (total_size + 7u) / 8u;
    pnanovdb_compute_array_t* merged = compute->create_array(8u, merged_uint64_count, nullptr);
    if (!merged || !merged->data)
    {
        if (merged && compute->destroy_array)
        {
            compute->destroy_array(merged);
        }
        return nullptr;
    }

    pnanovdb_uint64_t global_offset = 0llu;
    for (pnanovdb_uint32_t i = 0u; i < grid_count; ++i)
    {
        pnanovdb_compute_array_t* grid_array = grids[i];
        const pnanovdb_uint64_t words = (grid_array->element_size * grid_array->element_count) / 4u;
        pnanovdb_buf_t buf = pnanovdb_make_buf(static_cast<pnanovdb_uint32_t*>(grid_array->data), words);
        pnanovdb_grid_handle_t grid = { pnanovdb_address_null() };
        const pnanovdb_uint64_t grid_size = pnanovdb_grid_get_grid_size(buf, grid);
        memcpy(static_cast<uint8_t*>(merged->data) + global_offset, grid_array->data, grid_size);
        global_offset += grid_size;
    }

    return merged;
}

bool nanovdb(EditorScene& editor_scene,
             const pnanovdb_compute_t* compute,
             pnanovdb_editor_token_t* scene,
             const char* filepath,
             pnanovdb_pipeline_type_t render_pipeline,
             pnanovdb_editor_token_t* name)
{
    if (!scene || !filepath || !compute)
    {
        return false;
    }

    pnanovdb_editor_token_t* target_name = name;
    if (!target_name)
    {
        const std::string stem = std::filesystem::path(filepath).stem().string();
        target_name = EditorToken::getInstance().getToken(stem.c_str());
    }
    if (!target_name)
    {
        return false;
    }

    const uint64_t reservation_id = editor_scene.reserve_async_load_target(scene, target_name, name != nullptr);
    if (!reservation_id)
    {
        Console::getInstance().addLog(Console::LogLevel::Error, "Cannot load NanoVDB: object name '%s' is already in use",
                                      target_name->str ? target_name->str : "?");
        return false;
    }
    uint64_t lifetime_id = 0;
    if (!editor_scene.resolve_async_load_target(reservation_id, nullptr, nullptr, &lifetime_id))
    {
        editor_scene.finish_async_load_target(reservation_id, false);
        return false;
    }

    pnanovdb_compute_array_t* array = compute->load_nanovdb(filepath);
    if (!array)
    {
        editor_scene.finish_async_load_target(reservation_id, false);
        Console::getInstance().addLog(Console::LogLevel::Error, "Failed to load '%s'", filepath);
        return false;
    }

    const pnanovdb_uint32_t required_metadata = required_blind_metadata_count(render_pipeline);
    if (required_metadata > 0u && !is_voxelbvh_metadata(array, required_metadata))
    {
        Console::getInstance().addLog(Console::LogLevel::Warning,
                                      "'%s' is not a VoxelBVH NanoVDB; falling back to standard NanoVDB render",
                                      filepath);
        render_pipeline = pnanovdb_pipeline_type_nanovdb_render;
    }

    const bool consumed =
        editor_scene.handle_nanovdb_data_load(scene, array, filepath, render_pipeline, target_name, lifetime_id);
    editor_scene.finish_async_load_target(reservation_id, consumed);
    if (!consumed)
    {
        if (compute->destroy_array)
        {
            compute->destroy_array(array);
        }
        Console::getInstance().addLog(Console::LogLevel::Error, "Failed to attach NanoVDB '%s'", filepath);
        return false;
    }
    Console::getInstance().addLog(
        "Loaded NanoVDB from '%s' (render_pipeline=%d)", filepath, static_cast<int>(render_pipeline));
    return true;
}

} // namespace nanovdb_import
} // namespace pnanovdb_editor
