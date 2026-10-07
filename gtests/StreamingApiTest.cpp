// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

#include "editor/Editor.h"
#include "editor/EditorSceneManager.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
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
        editor.impl->editor_worker.reset();
        pnanovdb_editor_free(&editor);
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

TEST_F(StreamingApiTest, ShaderChangeWhileSceneControlsAreMappedReportsError)
{
    expect_without_render_frame(
        [&]()
        {
            const auto* type = editor.get_custom_scene_params_data_type(&editor, scene);
            ASSERT_NE(editor.map_params(&editor, scene, nullptr, type), nullptr);
            char error[256]{};
            EXPECT_FALSE(editor.set_shader(&editor, scene, name, "editor/editor.slang", "{}", error, sizeof(error)));
            EXPECT_NE(std::string(error).find("mapped"), std::string::npos);
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

TEST_F(StreamingApiTest, PipelineMapRejectsBlockingCalls)
{
    expect_without_render_frame(
        [&]()
        {
            auto* params = editor.map_pipeline_params(&editor, scene, name, pnanovdb_pipeline_stage_render);
            ASSERT_NE(params, nullptr);
            EXPECT_FALSE(editor.save_scene(&editor, "mapped-pipeline-must-not-save.json"));
            char error[256]{};
            EXPECT_FALSE(editor.set_shader(&editor, scene, name, "editor/editor.slang", "{}", error, sizeof(error)));
            EXPECT_NE(std::string(error).find("mapped"), std::string::npos);
            editor.unmap_pipeline_params(&editor, scene, name, pnanovdb_pipeline_stage_render);
        });
}

}
