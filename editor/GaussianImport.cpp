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
#include "PipelineRuntime.h"
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

    pnanovdb_pipeline_params_t process_params{};
    pnanovdb_pipeline_get_default_params(process_pipeline, &process_params);
    pipeline_params_set_voxels_per_unit(&process_params, voxels_per_unit);

    PipelineLoadRequest request;
    request.load_pipeline = pnanovdb_pipeline_type_gaussian_load;
    request.process_pipeline = process_pipeline;
    request.render_pipeline = render_pipeline;
    request.source_filepath = filepath;
    request.process_params = &process_params;

    const bool started = pipeline_load(&scene_manager, scene, request);
    pipeline_params_release(&process_params);

    return started;
}

} // namespace gaussian_import
} // namespace pnanovdb_editor
