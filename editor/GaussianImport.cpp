// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/editor/GaussianImport.cpp

    \author Petra Hapalova

    \brief
*/

#include "EditorImport.h"

#include "EditorScene.h"
#include "EditorSceneManager.h"
#include "EditorToken.h"
#include "Editor.h"
#include "Console.h"
#include "Pipeline.h"
#include "PipelineTypes.h"

#include "nanovdb_editor/putil/Compute.h"

#include <filesystem>
#include <string>

namespace pnanovdb_editor
{
namespace gaussian_import
{

bool gaussian(EditorScene& editor_scene,
              EditorSceneManager& scene_manager,
              const pnanovdb_compute_t* compute,
              pnanovdb_editor_token_t* scene,
              const char* filepath,
              pnanovdb_pipeline_type_t process_pipeline,
              pnanovdb_pipeline_type_t render_pipeline,
              float voxels_per_unit)
{
    if (!scene || !filepath || !compute)
    {
        return false;
    }

    pnanovdb_editor_t* editor = editor_scene.get_editor();
    if (!editor || !editor->impl)
    {
        return false;
    }

    if (process_pipeline == pnanovdb_pipeline_type_voxelbvh_build)
    {
        std::filesystem::path fs_path(filepath);
        std::string view_name = fs_path.stem().string();
        pnanovdb_editor_token_t* name_token = EditorToken::getInstance().getToken(view_name.c_str());

        scene_manager.add_file_object(scene, name_token, compute, process_pipeline, render_pipeline);

        const std::string filepath_copy = filepath;
        scene_manager.with_object(scene, name_token,
                                  [filepath_copy](SceneObject* obj)
                                  {
                                      if (!obj)
                                          return;
                                      obj->resources.source_filepath = filepath_copy;
                                      auto& process_params = obj->process_params();
                                      pnanovdb_pipeline_voxelbvh_build_params_set_source_type(
                                          &process_params, pnanovdb_pipeline_voxelbvh_source_gaussian_file);
                                      obj->process_dirty() = true;
                                  });

        editor_scene.add_nanovdb_placeholder(scene, name_token);
        editor_scene.select_render_view(scene, name_token);

        Console::getInstance().addLog("Loaded Gaussian file '%s' (VoxelBVH build pipeline)", filepath);
        return true;
    }

    bool rasterize_to_nanovdb = (process_pipeline == pnanovdb_pipeline_type_raster3d);

    PipelineContext ctx;
    ctx.compute = editor->impl->compute;
    ctx.device = editor->impl->device;
    ctx.queue = editor->impl->device_queue;
    ctx.compute_queue = editor->impl->compute_queue;
    ctx.raster = editor->impl->raster;
    ctx.raster_ctx = editor->impl->raster_ctx;
    ctx.voxelbvh = editor->impl->voxelbvh;
    ctx.voxelbvh_ctx = editor->impl->voxelbvh_ctx;
    ctx.renderer = editor->impl->renderer;
    ctx.scene_manager = &scene_manager;

    if (!pipeline_start_rasterization(filepath, voxels_per_unit, rasterize_to_nanovdb, process_pipeline,
                                      render_pipeline, &editor_scene, &scene_manager, scene, ctx))
    {
        return false;
    }

    return true;
}

} // namespace gaussian_import
} // namespace pnanovdb_editor
