// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/editor/MeshImport.cpp

    \author Petra Hapalova

    \brief
*/

#include "EditorImport.h"

#include "EditorScene.h"
#include "Console.h"
#include "PipelineTypes.h"

#include "nanovdb_editor/putil/Compute.h"
#include "nanovdb_editor/putil/FileFormat.h"

#include <cstdint>
#include <string>
#include <vector>

namespace pnanovdb_editor
{
namespace mesh_import
{
namespace
{

bool load_ply_arrays(const pnanovdb_compute_t* compute,
                     const char* filepath,
                     pnanovdb_compute_array_t** out_positions,
                     pnanovdb_compute_array_t** out_indices,
                     pnanovdb_compute_array_t** out_colors)
{
    *out_positions = nullptr;
    *out_indices = nullptr;
    *out_colors = nullptr;

    pnanovdb_fileformat_t fileformat = {};
    pnanovdb_fileformat_load(&fileformat, compute);
    if (!fileformat.load_file)
    {
        Console::getInstance().addLog(Console::LogLevel::Error, "Import Mesh: file format module unavailable");
        return false;
    }

    static const char* array_names[] = { "positions", "indices", "colors" };
    pnanovdb_compute_array_t* arrays[3] = {};
    pnanovdb_bool_t loaded = fileformat.load_file(filepath, 3u, array_names, arrays);
    pnanovdb_fileformat_free(&fileformat);

    if (!loaded || !arrays[0] || !arrays[1])
    {
        for (int i = 0; i < 3; ++i)
        {
            if (arrays[i])
                compute->destroy_array(arrays[i]);
        }
        Console::getInstance().addLog(
            Console::LogLevel::Error, "Import Mesh: failed to read positions+indices from '%s'", filepath);
        return false;
    }

    *out_positions = arrays[0];
    *out_indices = arrays[1];
    *out_colors = arrays[2]; // may be nullptr if the file has no per-vertex color
    return true;
}

pnanovdb_compute_array_t* synthesize_white_colors(const pnanovdb_compute_t* compute,
                                                  const pnanovdb_compute_array_t* positions)
{
    std::vector<float> white(positions->element_count, 1.0f);
    return compute->create_array(sizeof(float), positions->element_count, white.data());
}

} // namespace

bool mesh(EditorScene& editor_scene,
          const pnanovdb_compute_t* compute,
          pnanovdb_editor_token_t* scene,
          const char* filepath,
          const Options& options)
{
    if (!scene || !filepath || !compute)
        return false;

    pnanovdb_compute_array_t* positions = nullptr;
    pnanovdb_compute_array_t* indices = nullptr;
    pnanovdb_compute_array_t* colors = nullptr;
    if (!load_ply_arrays(compute, filepath, &positions, &indices, &colors))
        return false;

    const uint64_t index_element_count = indices->element_count;
    const uint32_t index_element_size = indices->element_size;
    const bool is_line_indices = (index_element_size == 2u * sizeof(uint32_t));
    const uint64_t prim_count = is_line_indices ? index_element_count : (index_element_count / 3u);
    const bool malformed = (index_element_count == 0u) || (!is_line_indices && (index_element_count % 3u != 0u));
    if (malformed)
    {
        compute->destroy_array(positions);
        compute->destroy_array(indices);
        if (colors)
            compute->destroy_array(colors);
        Console::getInstance().addLog(
            Console::LogLevel::Error,
            "Import Mesh: index buffer is not a triangle stream and not a line stream (%llu indices, "
            "element_size=%u) in '%s'",
            (unsigned long long)index_element_count, (unsigned)index_element_size, filepath);
        return false;
    }

    const bool has_ply_colors = (colors != nullptr);
    if (!has_ply_colors)
    {
        colors = synthesize_white_colors(compute, positions);
    }
    else if (colors->element_count != positions->element_count)
    {
        Console::getInstance().addLog(
            Console::LogLevel::Warning,
            "Import Mesh: '%s' has 'colors' with %llu floats but 'positions' has %llu; falling back to white", filepath,
            (unsigned long long)colors->element_count, (unsigned long long)positions->element_count);
        compute->destroy_array(colors);
        colors = synthesize_white_colors(compute, positions);
    }

    pnanovdb_pipeline_type_t effective_render_pipeline;
    if (is_line_indices)
    {
        effective_render_pipeline = pnanovdb_pipeline_type_voxelbvh_lines_render;
        if (options.show_debug)
        {
            Console::getInstance().addLog(
                "Import Mesh: '%s' is a line PLY; the lines render pipeline has no debug variant, "
                "Show Debug is ignored",
                filepath);
        }
    }
    else
    {
        effective_render_pipeline = options.show_debug ? pnanovdb_pipeline_type_voxelbvh_triangles_debug_render :
                                                         pnanovdb_pipeline_type_voxelbvh_triangles_render;
    }

    editor_scene.handle_mesh_data_load(scene, indices, positions, colors, filepath, effective_render_pipeline,
                                       is_line_indices, options.inflation_radius, options.resolution);

    Console::getInstance().addLog(
        "Loaded mesh from '%s' (vertices=%llu, %s=%llu, colors=%s, render_pipeline=%d, inflation=%.5f, int_space=%u)",
        filepath, (unsigned long long)(positions->element_count / 3u), is_line_indices ? "lines" : "triangles",
        (unsigned long long)prim_count, has_ply_colors ? "ply" : "white", static_cast<int>(effective_render_pipeline),
        options.inflation_radius, options.resolution);

    return true;
}

} // namespace mesh_import
} // namespace pnanovdb_editor
