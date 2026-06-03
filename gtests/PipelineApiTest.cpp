// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <nanovdb_editor/putil/Compiler.h>
#include <nanovdb_editor/putil/Compute.h>
#include <nanovdb_editor/putil/Editor.h>

#include "editor/PipelineTypes.h"

#include <nanovdb/tools/CreatePrimitives.h>

#include "EditorTestSupport.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <thread>

namespace
{

class PipelineApiTest : public ::testing::Test
{
protected:
    pnanovdb_compiler_t compiler{};
    pnanovdb_compute_t compute{};
    pnanovdb_editor_t editor{};

    pnanovdb_editor_token_t* scene_token = nullptr;
    pnanovdb_editor_token_t* name_token = nullptr;
    pnanovdb_compute_array_t* owned_array = nullptr;

    void SetUp() override
    {
        pnanovdb_compiler_load(&compiler);
        ASSERT_NE(compiler.module, nullptr);

        pnanovdb_compute_load(&compute, &compiler);
        ASSERT_NE(compute.module, nullptr);

        pnanovdb_editor_load(&editor, &compute, &compiler);
        ASSERT_NE(editor.module, nullptr);
        ASSERT_NE(editor.impl, nullptr);

        scene_token = editor.get_token("pipeline_api_scene");
        name_token = editor.get_token("pipeline_api_object");
        ASSERT_NE(scene_token, nullptr);
        ASSERT_NE(name_token, nullptr);

        std::array<uint8_t, 16> bytes{};
        owned_array = compute.create_array(sizeof(uint8_t), bytes.size(), bytes.data());
        ASSERT_NE(owned_array, nullptr);

        editor.add_nanovdb_2(&editor, scene_token, name_token, owned_array);
    }

    void TearDown() override
    {
        if (editor.impl)
        {
            editor.remove(&editor, scene_token, name_token);
            pnanovdb_editor_free(&editor);
        }
        if (owned_array)
        {
            compute.destroy_array(owned_array);
        }
        pnanovdb_compute_free(&compute);
        pnanovdb_compiler_free(&compiler);
    }
};

} // namespace

TEST_F(PipelineApiTest, SetGetRoundTripPerStage)
{
    editor.set_pipeline(
        &editor, scene_token, name_token, pnanovdb_pipeline_stage_load, pnanovdb_pipeline_type_gaussian_splat);
    editor.set_pipeline(
        &editor, scene_token, name_token, pnanovdb_pipeline_stage_process, pnanovdb_pipeline_type_gaussian_voxelize);
    editor.set_pipeline(
        &editor, scene_token, name_token, pnanovdb_pipeline_stage_render, pnanovdb_pipeline_type_nanovdb_render);

    EXPECT_EQ(editor.get_pipeline(&editor, scene_token, name_token, pnanovdb_pipeline_stage_load),
              pnanovdb_pipeline_type_gaussian_splat);
    EXPECT_EQ(editor.get_pipeline(&editor, scene_token, name_token, pnanovdb_pipeline_stage_process),
              pnanovdb_pipeline_type_gaussian_voxelize);
    EXPECT_EQ(editor.get_pipeline(&editor, scene_token, name_token, pnanovdb_pipeline_stage_render),
              pnanovdb_pipeline_type_nanovdb_render);
}

TEST_F(PipelineApiTest, SetPipelineOnlyDirtiesOnProcessStageChange)
{
    auto snapshot_dirty = [&]() -> pnanovdb_bool_t
    { return pnanovdb_editor_test::get_object_process_dirty(&editor, scene_token, name_token); };

    editor.set_pipeline(
        &editor, scene_token, name_token, pnanovdb_pipeline_stage_render, pnanovdb_pipeline_type_nanovdb_render);
    const pnanovdb_bool_t before_render_change = snapshot_dirty();
    editor.set_pipeline(
        &editor, scene_token, name_token, pnanovdb_pipeline_stage_render, pnanovdb_pipeline_type_gaussian_splat);
    EXPECT_EQ(snapshot_dirty(), before_render_change);

    editor.set_pipeline(&editor, scene_token, name_token, pnanovdb_pipeline_stage_load, pnanovdb_pipeline_type_noop);
    const pnanovdb_bool_t before_load_change = snapshot_dirty();
    editor.set_pipeline(
        &editor, scene_token, name_token, pnanovdb_pipeline_stage_load, pnanovdb_pipeline_type_gaussian_splat);
    EXPECT_EQ(snapshot_dirty(), before_load_change);

    editor.set_pipeline(&editor, scene_token, name_token, pnanovdb_pipeline_stage_process, pnanovdb_pipeline_type_noop);
    editor.set_pipeline(&editor, scene_token, name_token, pnanovdb_pipeline_stage_process, pnanovdb_pipeline_type_noop);
    editor.set_pipeline(
        &editor, scene_token, name_token, pnanovdb_pipeline_stage_process, pnanovdb_pipeline_type_nanovdb_render);
    EXPECT_EQ(snapshot_dirty(), PNANOVDB_TRUE);
}

TEST_F(PipelineApiTest, SetPipelineResetsOnTypeChange)
{
    ASSERT_EQ(editor.get_pipeline(&editor, scene_token, name_token, pnanovdb_pipeline_stage_render),
              pnanovdb_pipeline_type_nanovdb_render);
    const size_t initial_size = pnanovdb_editor_test::get_object_pipeline_params_size(
        &editor, scene_token, name_token, pnanovdb_pipeline_stage_render);
    const void* initial_data = pnanovdb_editor_test::get_object_pipeline_params_data(
        &editor, scene_token, name_token, pnanovdb_pipeline_stage_render);
    ASSERT_GT(initial_size, 0u);
    ASSERT_NE(initial_data, nullptr);

    ASSERT_EQ(pnanovdb_editor_test::append_blank_shader_override(
                  &editor, scene_token, name_token, pnanovdb_pipeline_stage_render),
              PNANOVDB_TRUE);

    editor.set_pipeline(
        &editor, scene_token, name_token, pnanovdb_pipeline_stage_render, pnanovdb_pipeline_type_nanovdb_render);
    EXPECT_EQ(pnanovdb_editor_test::get_object_pipeline_params_data(
                  &editor, scene_token, name_token, pnanovdb_pipeline_stage_render),
              initial_data);
    EXPECT_EQ(pnanovdb_editor_test::get_object_pipeline_shader_override_count(
                  &editor, scene_token, name_token, pnanovdb_pipeline_stage_render),
              1u);

    editor.set_pipeline(
        &editor, scene_token, name_token, pnanovdb_pipeline_stage_render, pnanovdb_pipeline_type_gaussian_splat);
    EXPECT_EQ(pnanovdb_editor_test::get_object_pipeline_params_size(
                  &editor, scene_token, name_token, pnanovdb_pipeline_stage_render),
              0u);
    EXPECT_EQ(pnanovdb_editor_test::get_object_pipeline_shader_override_count(
                  &editor, scene_token, name_token, pnanovdb_pipeline_stage_render),
              0u);

    editor.set_pipeline(
        &editor, scene_token, name_token, pnanovdb_pipeline_stage_render, pnanovdb_pipeline_type_nanovdb_render);
    EXPECT_EQ(pnanovdb_editor_test::get_object_pipeline_params_size(
                  &editor, scene_token, name_token, pnanovdb_pipeline_stage_render),
              initial_size);
}

TEST_F(PipelineApiTest, ConfiguredStageSurvivesResourceReAdd)
{
    ASSERT_EQ(editor.get_pipeline(&editor, scene_token, name_token, pnanovdb_pipeline_stage_process),
              pnanovdb_pipeline_type_noop);

    editor.set_pipeline(
        &editor, scene_token, name_token, pnanovdb_pipeline_stage_process, pnanovdb_pipeline_type_gaussian_voxelize);
    const size_t configured_size = pnanovdb_editor_test::get_object_pipeline_params_size(
        &editor, scene_token, name_token, pnanovdb_pipeline_stage_process);
    ASSERT_GT(configured_size, 0u);

    editor.add_nanovdb_2(&editor, scene_token, name_token, owned_array);

    EXPECT_EQ(editor.get_pipeline(&editor, scene_token, name_token, pnanovdb_pipeline_stage_process),
              pnanovdb_pipeline_type_gaussian_voxelize);
    EXPECT_EQ(pnanovdb_editor_test::get_object_pipeline_params_size(
                  &editor, scene_token, name_token, pnanovdb_pipeline_stage_process),
              configured_size);
}

TEST_F(PipelineApiTest, ConfigureBeforeAddCreatesObjectAndPersists)
{
    pnanovdb_editor_token_t* pre_scene = editor.get_token("pipeline_api_pre_scene");
    pnanovdb_editor_token_t* pre_name = editor.get_token("pipeline_api_pre_object");
    ASSERT_NE(pre_scene, nullptr);
    ASSERT_NE(pre_name, nullptr);

    ASSERT_EQ(editor.get_pipeline(&editor, pre_scene, pre_name, pnanovdb_pipeline_stage_process),
              pnanovdb_pipeline_type_noop);

    editor.set_pipeline(
        &editor, pre_scene, pre_name, pnanovdb_pipeline_stage_process, pnanovdb_pipeline_type_gaussian_voxelize);
    EXPECT_EQ(editor.get_pipeline(&editor, pre_scene, pre_name, pnanovdb_pipeline_stage_process),
              pnanovdb_pipeline_type_gaussian_voxelize);
    const size_t configured_size = pnanovdb_editor_test::get_object_pipeline_params_size(
        &editor, pre_scene, pre_name, pnanovdb_pipeline_stage_process);
    ASSERT_GT(configured_size, 0u);

    editor.add_nanovdb_2(&editor, pre_scene, pre_name, owned_array);
    EXPECT_EQ(editor.get_pipeline(&editor, pre_scene, pre_name, pnanovdb_pipeline_stage_process),
              pnanovdb_pipeline_type_gaussian_voxelize);
    EXPECT_EQ(pnanovdb_editor_test::get_object_pipeline_params_size(
                  &editor, pre_scene, pre_name, pnanovdb_pipeline_stage_process),
              configured_size);

    editor.remove(&editor, pre_scene, pre_name);
}

TEST(NanoVDBEditor, MarkPipelineDirtyKicksScheduler)
{
    pnanovdb_compiler_t compiler{};
    pnanovdb_compiler_load(&compiler);
    ASSERT_NE(compiler.module, nullptr);

    pnanovdb_compute_t compute{};
    pnanovdb_compute_load(&compute, &compiler);
    ASSERT_NE(compute.module, nullptr);

    pnanovdb_compute_device_desc_t device_desc{};
    pnanovdb_compute_device_manager_t* device_manager = compute.device_interface.create_device_manager(PNANOVDB_FALSE);
    ASSERT_NE(device_manager, nullptr);

    pnanovdb_compute_physical_device_desc_t phys_desc{};
    if (!compute.device_interface.enumerate_devices(device_manager, 0u, &phys_desc))
    {
        compute.device_interface.destroy_device_manager(device_manager);
        pnanovdb_compute_free(&compute);
        pnanovdb_compiler_free(&compiler);
        GTEST_SKIP() << "No Vulkan-compatible device available on this machine";
    }

    pnanovdb_compute_device_t* device = compute.device_interface.create_device(device_manager, &device_desc);
    ASSERT_NE(device, nullptr);

    pnanovdb_editor_t editor{};
    pnanovdb_editor_load(&editor, &compute, &compiler);
    ASSERT_NE(editor.module, nullptr);

    pnanovdb_editor_config_t cfg{};
    cfg.ip_address = "127.0.0.1";
    cfg.port = 8093;
    cfg.headless = PNANOVDB_TRUE;
    cfg.streaming = PNANOVDB_FALSE;

    editor.start(&editor, device, &cfg);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    pnanovdb_editor_token_t* scene = editor.get_token("kick_scene");
    pnanovdb_editor_token_t* name = editor.get_token("kick_object");
    ASSERT_NE(scene, nullptr);
    ASSERT_NE(name, nullptr);

    auto sphere = nanovdb::tools::createLevelSetSphere<float>(2.0f);
    pnanovdb_compute_array_t* nanovdb_array = compute.create_array(4u, sphere.size() / 4u, sphere.data());
    ASSERT_NE(nanovdb_array, nullptr);
    editor.add_nanovdb_2(&editor, scene, name, nanovdb_array);

    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    EXPECT_EQ(editor.get_pipeline(&editor, scene, name, pnanovdb_pipeline_stage_process), pnanovdb_pipeline_type_noop);
    EXPECT_EQ(pnanovdb_editor_test::get_object_process_dirty(&editor, scene, name), PNANOVDB_TRUE);

    editor.set_pipeline(&editor, scene, name, pnanovdb_pipeline_stage_process, pnanovdb_pipeline_type_nanovdb_render);

    bool kicked = false;
    for (int i = 0; i < 20 && !kicked; ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        kicked = (pnanovdb_editor_test::get_object_process_dirty(&editor, scene, name) == PNANOVDB_FALSE);
    }
    EXPECT_TRUE(kicked) << "Scheduler did not clear process_dirty after set_pipeline(stage_process, !noop)";

    ASSERT_EQ(pnanovdb_editor_test::get_object_process_dirty(&editor, scene, name), PNANOVDB_FALSE);
    editor.mark_pipeline_dirty(&editor, scene, name);

    bool re_kicked = false;
    for (int i = 0; i < 20 && !re_kicked; ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        re_kicked = (pnanovdb_editor_test::get_object_process_dirty(&editor, scene, name) == PNANOVDB_FALSE);
    }
    EXPECT_TRUE(re_kicked) << "Scheduler did not re-run the process pipeline after mark_pipeline_dirty";

    editor.stop(&editor);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    pnanovdb_editor_free(&editor);

    compute.destroy_array(nanovdb_array);
    compute.device_interface.destroy_device(device_manager, device);
    compute.device_interface.destroy_device_manager(device_manager);
    pnanovdb_compute_free(&compute);
    pnanovdb_compiler_free(&compiler);
}
