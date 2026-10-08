// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

#include "editor/Editor.h"
#include "editor/EditorSceneManager.h"

#include <gtest/gtest.h>

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

TEST_F(StreamingApiTest, AddBatchDoesNotWaitForRenderFrames)
{
    expect_without_render_frame(
        [&]()
        {
            for (int i = 0; i < 100; ++i)
                editor.add_nanovdb_2(&editor, scene, name, array);
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

TEST_F(StreamingApiTest, StreamingUpdateCopiesSourceAndKeepsMaterialAndPipelines)
{
    auto* shader = editor.get_token("editor/flow_smoke.slang");
    void* material = nullptr;
    editor.impl->scene_manager->with_object(scene, name,
                                            [&](pnanovdb_editor::SceneObject* obj)
                                            {
                                                obj->shader_name() = shader;
                                                material = obj->shader_params();
                                                obj->pipeline.process().type = pnanovdb_pipeline_type_voxelbvh_build;
                                                obj->pipeline.process().configured = true;
                                                obj->visible = false;
                                            });
    static_cast<unsigned char*>(array->data)[0] = 42;
    EXPECT_TRUE(editor.update_nanovdb_buffer(&editor, scene, name, array));
    static_cast<unsigned char*>(array->data)[0] = 0;
    editor.impl->scene_manager->with_object(
        scene, name,
        [&](pnanovdb_editor::SceneObject* obj)
        {
            ASSERT_NE(obj, nullptr);
            EXPECT_EQ(obj->shader_name(), shader);
            EXPECT_EQ(obj->shader_params(), material);
            EXPECT_EQ(obj->pipeline.process().type, pnanovdb_pipeline_type_voxelbvh_build);
            EXPECT_FALSE(obj->visible);
            ASSERT_NE(obj->nanovdb_array(), nullptr);
            EXPECT_EQ(static_cast<unsigned char*>(obj->nanovdb_array()->data)[0], 42);
        });
    EXPECT_FALSE(editor.update_nanovdb_buffer(&editor, scene, editor.get_token("missing"), array));
}

TEST_F(StreamingApiTest, OrdinaryRegistrationResetsMaterialAndHonorsConfiguredPipelines)
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
                        EXPECT_STREQ(obj->shader_name()->str, "editor/editor.slang");
                        EXPECT_EQ(obj->pipeline.process().type, explicit_pipelines ?
                                                                    pnanovdb_pipeline_type_noop :
                                                                    pnanovdb_pipeline_type_voxelbvh_build);
                        EXPECT_FALSE(obj->visible);
                    });
            }
        });
}

TEST_F(StreamingApiTest, StreamingWhileSceneControlsAreMappedDoesNotWaitForRender)
{
    expect_without_render_frame(
        [&]()
        {
            const auto* type = editor.get_custom_scene_params_data_type(&editor, scene);
            ASSERT_NE(editor.map_params(&editor, scene, nullptr, type), nullptr);
            EXPECT_TRUE(editor.update_nanovdb_buffer(&editor, scene, name, array));
            editor.unmap_params(&editor, scene, nullptr);
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
