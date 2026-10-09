// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

#include "editor/Editor.h"
#include "editor/EditorSceneManager.h"

#include <gtest/gtest.h>
#include <nanovdb/tools/CreateNanoGrid.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>

namespace
{
using namespace std::chrono_literals;

class StreamingApiTest : public ::testing::Test
{
protected:
    pnanovdb_compiler_t compiler{};
    pnanovdb_compute_t compute{};
    pnanovdb_editor_t editor{};
    pnanovdb_editor_token_t* scene = nullptr;
    pnanovdb_editor_token_t* name = nullptr;
    pnanovdb_compute_array_t* array = nullptr;
    std::shared_ptr<pnanovdb_editor::EditorWorker> worker;

    void SetUp() override
    {
        pnanovdb_compiler_load(&compiler);
        pnanovdb_compute_load(&compute, &compiler);
        pnanovdb_editor_load(&editor, &compute, &compiler);
        ASSERT_NE(editor.impl, nullptr);
        scene = editor.get_token("streaming_api");
        name = editor.get_token("grid");
        std::array<unsigned char, 32> bytes{};
        array = compute.create_array(1u, bytes.size(), bytes.data());
        ASSERT_NE(array, nullptr);
        editor.add_nanovdb_2(&editor, scene, name, array);
        auto* schema = editor.get_token(R"({"SceneParams":{"Play":{"type":"bool","default":true}}})");
        ASSERT_TRUE(editor.set_custom_scene_params(&editor, scene, schema, nullptr, 0));
        worker = std::make_shared<pnanovdb_editor::EditorWorker>();
        editor.impl->editor_worker = worker;
    }

    void TearDown() override
    {
        worker->render_thread_tasks.close();
        if (editor.impl)
        {
            editor.impl->editor_worker.reset();
            pnanovdb_editor_free(&editor);
        }
        else
        {
            pnanovdb_free_library(editor.module);
        }
        compute.destroy_array(array);
        pnanovdb_compute_free(&compute);
        pnanovdb_compiler_free(&compiler);
    }

    template <typename Fn>
    void expect_without_render_frame(Fn fn)
    {
        auto caller = std::async(std::launch::async, fn);
        const auto result = caller.wait_for(5s);
        worker->render_thread_tasks.close();
        EXPECT_EQ(result, std::future_status::ready);
        caller.get();
    }

    void expect_shutdown_preserves_pipeline_map(bool process_step)
    {
        editor.impl->editor_worker.reset();
        auto* params = process_step ? editor.map_process_step_params(&editor, scene, name, 0) :
                                     editor.map_pipeline_params(&editor, scene, name, pnanovdb_pipeline_stage_render);
        ASSERT_NE(params, nullptr);
        auto* impl = editor.impl;
        editor.shutdown(&editor);
        ASSERT_EQ(editor.impl, impl);
        if (process_step)
            editor.unmap_process_step_params(&editor, scene, name, 0);
        else
            editor.unmap_pipeline_params(&editor, scene, name, pnanovdb_pipeline_stage_render);
        editor.shutdown(&editor);
        EXPECT_EQ(editor.impl, nullptr);
    }
};

TEST_F(StreamingApiTest, RepeatedAddBatchDoesNotWaitForRenderFrames)
{
    expect_without_render_frame(
        [&]()
        {
            for (int i = 0; i < 100; ++i)
                editor.add_nanovdb_2(&editor, scene, name, array);
        });
}

TEST_F(StreamingApiTest, AddNewObjectBatchDoesNotWaitForRenderFrames)
{
    expect_without_render_frame(
        [&]()
        {
            for (int i = 0; i < 100; ++i)
            {
                auto* added_name = editor.get_token(("grid_" + std::to_string(i)).c_str());
                editor.add_nanovdb_2(&editor, scene, added_name, array);
                editor.impl->scene_manager->with_object(scene, added_name,
                                                        [](pnanovdb_editor::SceneObject* obj)
                                                        {
                                                            ASSERT_NE(obj, nullptr);
                                                            EXPECT_NE(obj->nanovdb_array(), nullptr);
                                                        });
            }
        });
}

TEST_F(StreamingApiTest, AddWhileSceneControlsAreMappedDoesNotWaitForRender)
{
    expect_without_render_frame(
        [&]()
        {
            const auto* type = editor.get_custom_scene_params_data_type(&editor, scene);
            ASSERT_NE(editor.map_params(&editor, scene, nullptr, type), nullptr);
            editor.add_nanovdb_2(&editor, scene, name, array);
            editor.add_nanovdb_2(&editor, scene, editor.get_token("new_grid"), array);
            editor.unmap_params(&editor, scene, nullptr);
        });
}

TEST_F(StreamingApiTest, SaveWhileSceneControlsAreMappedFailsWithoutWaiting)
{
    expect_without_render_frame(
        [&]()
        {
            const auto* type = editor.get_custom_scene_params_data_type(&editor, scene);
            ASSERT_NE(editor.map_params(&editor, scene, nullptr, type), nullptr);
            EXPECT_FALSE(editor.save_scene(&editor, "mapped-scene-must-not-save.json"));
            editor.unmap_params(&editor, scene, nullptr);
        });
}

TEST_F(StreamingApiTest, LoadBeforeStartupWhileSceneControlsAreMappedIsDeferred)
{
    editor.impl->editor_worker.reset();
    ASSERT_EQ(editor.impl->editor_scene, nullptr);
    const auto path = std::filesystem::temp_directory_path() / "pnanovdb_streaming_api_deferred_scene.json";
    {
        std::ofstream file(path);
        file << R"({"version":1,"objects":[],"scenes":[{"name":"deferred_scene"}]})";
    }
    ASSERT_TRUE(editor.load_scene(&editor, path.string().c_str(), PNANOVDB_FALSE));
    EXPECT_EQ(editor.impl->pending_scene_path, path.string());
    EXPECT_FALSE(editor.impl->pending_scene_overwrite);
    editor.impl->pending_scene_path.clear();

    const auto* type = editor.get_custom_scene_params_data_type(&editor, scene);
    ASSERT_NE(editor.map_params(&editor, scene, nullptr, type), nullptr);
    EXPECT_TRUE(editor.load_scene(&editor, path.string().c_str(), PNANOVDB_TRUE));
    EXPECT_EQ(editor.impl->pending_scene_path, path.string());
    EXPECT_TRUE(editor.impl->pending_scene_overwrite);
    editor.unmap_params(&editor, scene, nullptr);
    std::filesystem::remove(path);
}

TEST_F(StreamingApiTest, ShutdownWithPipelineParamsMappedBeforeStartupIsDeferred)
{
    expect_shutdown_preserves_pipeline_map(false);
}

TEST_F(StreamingApiTest, ShutdownWithProcessStepParamsMappedBeforeStartupIsDeferred)
{
    expect_shutdown_preserves_pipeline_map(true);
}

TEST_F(StreamingApiTest, ShaderMapWhileSceneControlsAreMappedDoesNotWaitForRender)
{
    expect_without_render_frame(
        [&]()
        {
            const auto* type = editor.get_custom_scene_params_data_type(&editor, scene);
            ASSERT_NE(editor.map_params(&editor, scene, nullptr, type), nullptr);
            auto* shader = static_cast<pnanovdb_editor_shader_t*>(editor.map_params(
                &editor, scene, name, PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_editor_shader_t)));
            ASSERT_NE(shader, nullptr);
            shader->shader_params[0] ^= 1u;
            editor.unmap_params(&editor, scene, name);
            editor.unmap_params(&editor, scene, nullptr);
        });
}

TEST_F(StreamingApiTest, RepeatedAddCopiesSourceAndKeepsMaterialAndPipelines)
{
    auto* shader = editor.get_token("editor/flow_smoke.slang");
    void* material = nullptr;
    uint64_t registration = 0;
    uint64_t lifetime = 0;
    editor.impl->scene_manager->with_object(scene, name,
                                            [&](pnanovdb_editor::SceneObject* obj)
                                            {
                                                obj->shader_name() = shader;
                                                material = obj->shader_params();
                                                registration = obj->registration_id;
                                                lifetime = obj->lifetime_id;
                                                obj->pipeline.process().type = pnanovdb_pipeline_type_voxelbvh_build;
                                                obj->pipeline.process().configured = true;
                                                obj->visible = false;
                                            });
    static_cast<unsigned char*>(array->data)[0] = 42;
    editor.add_nanovdb_2(&editor, scene, name, array);
    static_cast<unsigned char*>(array->data)[0] = 0;
    editor.impl->scene_manager->with_object(
        scene, name,
        [&](pnanovdb_editor::SceneObject* obj)
        {
            ASSERT_NE(obj, nullptr);
            EXPECT_EQ(obj->shader_name(), shader);
            EXPECT_EQ(obj->shader_params(), material);
            EXPECT_EQ(obj->registration_id, registration);
            EXPECT_NE(obj->lifetime_id, lifetime);
            EXPECT_EQ(obj->pipeline.process().type, pnanovdb_pipeline_type_voxelbvh_build);
            EXPECT_FALSE(obj->visible);
            ASSERT_NE(obj->nanovdb_array(), nullptr);
            EXPECT_EQ(static_cast<unsigned char*>(obj->nanovdb_array()->data)[0], 42);
        });
}

TEST_F(StreamingApiTest, RepeatedAddPreservesMaterialAcrossGridTypesUntilExplicitReplacement)
{
    nanovdb::tools::build::Grid<float> float_grid(0.f);
    float_grid.getAccessor().setValue(nanovdb::Coord(0), 1.f);
    auto float_handle = nanovdb::tools::createNanoGrid(float_grid);
    pnanovdb_compute_array_t float_array{ float_handle.data(), 1u, float_handle.bufferSize() };
    editor.add_nanovdb_2(&editor, scene, name, &float_array);

    auto* shader = editor.get_token("editor/flow_smoke.slang");
    void* material = nullptr;
    editor.impl->scene_manager->with_object(scene, name,
                                            [&](pnanovdb_editor::SceneObject* obj)
                                            {
                                                obj->shader_name() = shader;
                                                material = obj->shader_params();
                                            });

    nanovdb::tools::build::Grid<nanovdb::math::Rgba8> rgba_grid(nanovdb::math::Rgba8(uint8_t(0)));
    rgba_grid.getAccessor().setValue(nanovdb::Coord(0), nanovdb::math::Rgba8(uint8_t(255)));
    auto rgba_handle = nanovdb::tools::createNanoGrid(rgba_grid);
    ASSERT_EQ(float_handle.gridType(), nanovdb::GridType::Float);
    ASSERT_EQ(rgba_handle.gridType(), nanovdb::GridType::RGBA8);
    pnanovdb_compute_array_t rgba_array{ rgba_handle.data(), 1u, rgba_handle.bufferSize() };
    editor.add_nanovdb_2(&editor, scene, name, &rgba_array);
    editor.impl->scene_manager->with_object(scene, name,
                                            [&](pnanovdb_editor::SceneObject* obj)
                                            {
                                                EXPECT_EQ(obj->shader_name(), shader);
                                                EXPECT_EQ(obj->shader_params(), material);
                                            });

    editor.add_nanovdb_3(&editor, scene, name, &rgba_array, pnanovdb_pipeline_type_noop,
                         pnanovdb_pipeline_type_nanovdb_render);
    editor.impl->scene_manager->with_object(scene, name,
                                            [&](pnanovdb_editor::SceneObject* obj)
                                            {
                                                EXPECT_STREQ(obj->shader_name()->str, "editor/editor.slang");
                                            });
}

TEST_F(StreamingApiTest, AddRegistersNewObjectsAndReplacesFileSourcesAndOtherTypes)
{
    using pnanovdb_editor::EditorSceneManager;
    using pnanovdb_editor::SceneObject;
    using pnanovdb_editor::SceneObjectType;
    auto* manager = editor.impl->scene_manager;
    auto* added_name = editor.get_token("new_grid");
    EXPECT_EQ(manager->add_nanovdb(scene, added_name, compute.duplicate_array(array), &compute),
              EditorSceneManager::NanoVDBAddResult::Registered);

    for (const auto type : { SceneObjectType::NanoVDB, SceneObjectType::GaussianData, SceneObjectType::Array,
                             SceneObjectType::Uninitialized })
    {
        uint64_t registration = 0;
        uint64_t lifetime = 0;
        manager->with_object(scene, name,
                             [&](SceneObject* obj)
                             {
                                 obj->type = type;
                                 obj->resources.source_filepath = type == SceneObjectType::NanoVDB ? "old.nvdb" : "";
                                 obj->shader_name() = editor.get_token("editor/flow_smoke.slang");
                                 registration = obj->registration_id;
                                 lifetime = obj->lifetime_id;
                             });
        EXPECT_EQ(manager->add_nanovdb(scene, name, compute.duplicate_array(array), &compute),
                  EditorSceneManager::NanoVDBAddResult::Registered);
        manager->with_object(scene, name,
                             [&](SceneObject* obj)
                             {
                                 ASSERT_NE(obj, nullptr);
                                 EXPECT_EQ(obj->type, SceneObjectType::NanoVDB);
                                 EXPECT_TRUE(obj->resources.source_filepath.empty());
                                 EXPECT_STREQ(obj->shader_name()->str, "editor/editor.slang");
                                 EXPECT_NE(obj->registration_id, registration);
                                 EXPECT_NE(obj->lifetime_id, lifetime);
                                 EXPECT_NE(obj->shader_params(), nullptr);
                                 EXPECT_NE(obj->nanovdb_array(), nullptr);
                             });
    }
}

TEST_F(StreamingApiTest, AddPreservesExistingCamera)
{
    auto* camera_name = editor.get_token("camera");
    pnanovdb_camera_view_t camera{};
    ASSERT_TRUE(editor.impl->scene_manager->add_camera(scene, camera_name, &camera));
    const auto lifetime = editor.impl->scene_manager->object_lifetime(scene, camera_name);
    EXPECT_EQ(editor.impl->scene_manager->add_nanovdb(scene, camera_name, compute.duplicate_array(array), &compute),
              pnanovdb_editor::EditorSceneManager::NanoVDBAddResult::Failed);
    editor.impl->scene_manager->with_object(scene, camera_name,
                                            [&](pnanovdb_editor::SceneObject* obj)
                                            {
                                                ASSERT_NE(obj, nullptr);
                                                EXPECT_EQ(obj->type, pnanovdb_editor::SceneObjectType::Camera);
                                                EXPECT_EQ(obj->lifetime_id, lifetime);
                                                EXPECT_NE(obj->resources.camera_view, nullptr);
                                            });
}

TEST_F(StreamingApiTest, RepeatedAddKeepsMaterialUnlessPipelinesAreExplicit)
{
    expect_without_render_frame(
        [&]()
        {
            editor.impl->shader_name = "editor/flow_smoke.slang";
            for (bool explicit_pipelines : { false, true })
            {
                editor.impl->scene_manager->with_object(
                    scene, name,
                    [&](pnanovdb_editor::SceneObject* obj)
                    {
                        obj->shader_name() = editor.get_token("editor/flow_smoke.slang");
                        obj->pipeline.process().type = pnanovdb_pipeline_type_voxelbvh_build;
                        obj->pipeline.process().configured = true;
                        obj->visible = false;
                    });
                if (explicit_pipelines)
                    editor.add_nanovdb_3(&editor, scene, name, array, pnanovdb_pipeline_type_noop,
                                         pnanovdb_pipeline_type_nanovdb_render);
                else
                    editor.add_nanovdb_2(&editor, scene, name, array);
                editor.impl->scene_manager->with_object(
                    scene, name,
                    [&](pnanovdb_editor::SceneObject* obj)
                    {
                        ASSERT_NE(obj, nullptr);
                        EXPECT_STREQ(obj->shader_name()->str, explicit_pipelines ?
                                                                "editor/editor.slang" : "editor/flow_smoke.slang");
                        EXPECT_EQ(obj->pipeline.process().type, explicit_pipelines ?
                                                                    pnanovdb_pipeline_type_noop :
                                                                    pnanovdb_pipeline_type_voxelbvh_build);
                        EXPECT_FALSE(obj->visible);
                    });
            }
        });
}

TEST_F(StreamingApiTest, PipelineMapAllowsShaderMapWithoutBlocking)
{
    expect_without_render_frame(
        [&]()
        {
            auto* params = editor.map_pipeline_params(&editor, scene, name, pnanovdb_pipeline_stage_render);
            ASSERT_NE(params, nullptr);
            EXPECT_FALSE(editor.save_scene(&editor, "mapped-pipeline-must-not-save.json"));
            auto* shader = static_cast<pnanovdb_editor_shader_t*>(editor.map_params(
                &editor, scene, name, PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_editor_shader_t)));
            ASSERT_NE(shader, nullptr);
            shader->shader_params[0] ^= 1u;
            editor.unmap_params(&editor, scene, name);
            editor.unmap_pipeline_params(&editor, scene, name, pnanovdb_pipeline_stage_render);
        });
}

}
