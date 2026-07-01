// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <nanovdb_editor/putil/Compiler.h>
#include <nanovdb_editor/putil/Compute.h>
#include <nanovdb_editor/putil/Editor.h>

#include "editor/Editor.h"
#include "editor/EditorSceneManager.h"
#include "editor/Pipeline.h"
#include "editor/PipelineTypes.h"

#include <nanovdb/tools/CreatePrimitives.h>

#include "EditorTestSupport.h"
#include "GpuTestSupport.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

namespace
{

class PipelineApiTest : public ::testing::Test
{
protected:
    pnanovdb_compiler_t compiler{};
    pnanovdb_compute_t compute{};
    pnanovdb_editor_t editor{};
    pnanovdb_editor::EditorWorker worker{};

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
        worker.is_starting.store(false, std::memory_order_release);
        editor.impl->editor_worker = &worker;

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
            editor.impl->editor_worker = nullptr;
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

}

TEST_F(PipelineApiTest, SetGetRoundTripPerStage)
{
    editor.set_pipeline(
        &editor, scene_token, name_token, pnanovdb_pipeline_stage_load, pnanovdb_pipeline_type_gaussian_load);
    editor.set_pipeline(
        &editor, scene_token, name_token, pnanovdb_pipeline_stage_process, pnanovdb_pipeline_type_gaussian_voxelize);
    editor.set_pipeline(
        &editor, scene_token, name_token, pnanovdb_pipeline_stage_render, pnanovdb_pipeline_type_nanovdb_render);

    EXPECT_EQ(editor.get_pipeline(&editor, scene_token, name_token, pnanovdb_pipeline_stage_load),
              pnanovdb_pipeline_type_gaussian_load);
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
        &editor, scene_token, name_token, pnanovdb_pipeline_stage_load, pnanovdb_pipeline_type_gaussian_load);
    EXPECT_EQ(snapshot_dirty(), before_load_change);

    editor.set_pipeline(&editor, scene_token, name_token, pnanovdb_pipeline_stage_process, pnanovdb_pipeline_type_noop);
    editor.set_pipeline(&editor, scene_token, name_token, pnanovdb_pipeline_stage_process, pnanovdb_pipeline_type_noop);
    editor.set_pipeline(
        &editor, scene_token, name_token, pnanovdb_pipeline_stage_process, pnanovdb_pipeline_type_gaussian_voxelize);
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

TEST_F(PipelineApiTest, ConfigureBeforeAddLeavesObjectTypeUndefined)
{
    pnanovdb_editor_token_t* pending_scene = editor.get_token("pipeline_api_pending_scene");
    pnanovdb_editor_token_t* pending_name = editor.get_token("pipeline_api_pending_object");
    editor.set_pipeline(&editor, pending_scene, pending_name, pnanovdb_pipeline_stage_process,
                        pnanovdb_pipeline_type_gaussian_voxelize);

    bool is_uninitialized = false;
    editor.impl->scene_manager->with_object(
        pending_scene, pending_name,
        [&](pnanovdb_editor::SceneObject* obj)
        { is_uninitialized = obj && obj->type == pnanovdb_editor::SceneObjectType::Uninitialized; });
    ASSERT_TRUE(is_uninitialized);

    editor.remove(&editor, pending_scene, pending_name);
}


TEST_F(PipelineApiTest, SetProcessChainExpandsIntoSteps)
{
    ASSERT_EQ(pnanovdb_editor_test::get_object_process_step_count(&editor, scene_token, name_token), 1u);

    editor.set_pipeline(
        &editor, scene_token, name_token, pnanovdb_pipeline_stage_process, pnanovdb_pipeline_type_voxelbvh_rgba8_chain);

    EXPECT_EQ(pnanovdb_editor_test::get_object_process_step_count(&editor, scene_token, name_token), 2u);
    EXPECT_EQ(pnanovdb_editor_test::get_object_process_step_type(&editor, scene_token, name_token, 0),
              pnanovdb_pipeline_type_voxelbvh_build);
    EXPECT_EQ(pnanovdb_editor_test::get_object_process_step_type(&editor, scene_token, name_token, 1),
              pnanovdb_pipeline_type_voxelbvh_rgba8);

    EXPECT_EQ(editor.get_pipeline(&editor, scene_token, name_token, pnanovdb_pipeline_stage_process),
              pnanovdb_pipeline_type_voxelbvh_build);
    EXPECT_EQ(pnanovdb_editor_test::get_object_process_dirty(&editor, scene_token, name_token), PNANOVDB_TRUE);

    editor.set_pipeline(
        &editor, scene_token, name_token, pnanovdb_pipeline_stage_process, pnanovdb_pipeline_type_voxelbvh_rgba8_chain);
    EXPECT_EQ(pnanovdb_editor_test::get_object_process_step_count(&editor, scene_token, name_token), 2u);
    EXPECT_EQ(pnanovdb_editor_test::get_object_process_step_type(&editor, scene_token, name_token, 1),
              pnanovdb_pipeline_type_voxelbvh_rgba8);
}

TEST_F(PipelineApiTest, ReapplyingUnchangedProcessConfigurationPreservesParamsAndOutputs)
{
    editor.set_pipeline(
        &editor, scene_token, name_token, pnanovdb_pipeline_stage_process, pnanovdb_pipeline_type_voxelbvh_rgba8_chain);

    pnanovdb_pipeline_params_t* params = editor.map_process_step_params(&editor, scene_token, name_token, 0);
    ASSERT_NE(params, nullptr);
    ASSERT_GE(params->size, 2u * sizeof(pnanovdb_uint32_t));
    const pnanovdb_uint32_t resolution = 1234u;
    std::memcpy(static_cast<unsigned char*>(params->data) + sizeof(pnanovdb_uint32_t), &resolution, sizeof(resolution));
    std::vector<unsigned char> expected((unsigned char*)params->data, (unsigned char*)params->data + params->size);
    editor.unmap_process_step_params(&editor, scene_token, name_token, 0);

    pnanovdb_compute_array_t* step0_out = compute.create_array(
        sizeof(uint8_t), owned_array->element_count, static_cast<const uint8_t*>(owned_array->data));
    ASSERT_NE(step0_out, nullptr);
    pnanovdb_editor_test::set_process_step_nanovdb_output(&editor, scene_token, name_token, 0, step0_out, &compute);
    ASSERT_EQ(pnanovdb_editor_test::get_object_renderable_data_kind(&editor, scene_token, name_token),
              (pnanovdb_uint32_t)pnanovdb_pipeline_data_kind_voxelbvh);

    editor.set_pipeline(
        &editor, scene_token, name_token, pnanovdb_pipeline_stage_process, pnanovdb_pipeline_type_voxelbvh_rgba8_chain);
    EXPECT_EQ(pnanovdb_editor_test::get_object_renderable_data_kind(&editor, scene_token, name_token),
              (pnanovdb_uint32_t)pnanovdb_pipeline_data_kind_voxelbvh);
    params = editor.map_process_step_params(&editor, scene_token, name_token, 0);
    ASSERT_NE(params, nullptr);
    ASSERT_EQ(params->size, expected.size());
    EXPECT_EQ(std::memcmp(params->data, expected.data(), expected.size()), 0);
    editor.unmap_process_step_params(&editor, scene_token, name_token, 0);

    // The individual step setter follows the same idempotent contract.
    step0_out = compute.create_array(
        sizeof(uint8_t), owned_array->element_count, static_cast<const uint8_t*>(owned_array->data));
    ASSERT_NE(step0_out, nullptr);
    pnanovdb_editor_test::set_process_step_nanovdb_output(&editor, scene_token, name_token, 0, step0_out, &compute);
    editor.set_process_step(&editor, scene_token, name_token, 0, pnanovdb_pipeline_type_voxelbvh_build);
    EXPECT_EQ(pnanovdb_editor_test::get_object_renderable_data_kind(&editor, scene_token, name_token),
              (pnanovdb_uint32_t)pnanovdb_pipeline_data_kind_voxelbvh);
    params = editor.map_process_step_params(&editor, scene_token, name_token, 0);
    ASSERT_NE(params, nullptr);
    EXPECT_EQ(std::memcmp(params->data, expected.data(), expected.size()), 0);
    editor.unmap_process_step_params(&editor, scene_token, name_token, 0);
}

TEST_F(PipelineApiTest, SetSingleProcessPipelineCollapsesChain)
{
    editor.set_pipeline(
        &editor, scene_token, name_token, pnanovdb_pipeline_stage_process, pnanovdb_pipeline_type_voxelbvh_rgba8_chain);
    ASSERT_EQ(pnanovdb_editor_test::get_object_process_step_count(&editor, scene_token, name_token), 2u);

    editor.set_pipeline(
        &editor, scene_token, name_token, pnanovdb_pipeline_stage_process, pnanovdb_pipeline_type_voxelbvh_rgba8);
    EXPECT_EQ(pnanovdb_editor_test::get_object_process_step_count(&editor, scene_token, name_token), 1u);
    EXPECT_EQ(pnanovdb_editor_test::get_object_process_step_type(&editor, scene_token, name_token, 0),
              pnanovdb_pipeline_type_voxelbvh_rgba8);
}

TEST_F(PipelineApiTest, SetProcessPipelineCollapsesChainEvenWhenTypeUnchanged)
{
    editor.set_pipeline(
        &editor, scene_token, name_token, pnanovdb_pipeline_stage_process, pnanovdb_pipeline_type_voxelbvh_rgba8_chain);
    ASSERT_EQ(pnanovdb_editor_test::get_object_process_step_count(&editor, scene_token, name_token), 2u);
    ASSERT_EQ(pnanovdb_editor_test::get_object_process_step_type(&editor, scene_token, name_token, 0),
              pnanovdb_pipeline_type_voxelbvh_build);

    editor.set_pipeline(
        &editor, scene_token, name_token, pnanovdb_pipeline_stage_process, pnanovdb_pipeline_type_voxelbvh_build);
    EXPECT_EQ(pnanovdb_editor_test::get_object_process_step_count(&editor, scene_token, name_token), 1u);
    EXPECT_EQ(pnanovdb_editor_test::get_object_process_step_type(&editor, scene_token, name_token, 0),
              pnanovdb_pipeline_type_voxelbvh_build);
}

TEST_F(PipelineApiTest, AppendedProcessStepDefaultsToNoop)
{
    ASSERT_EQ(pnanovdb_editor_test::get_object_process_step_count(&editor, scene_token, name_token), 1u);

    const size_t idx = pnanovdb_editor_test::append_process_step(&editor, scene_token, name_token);
    ASSERT_EQ(idx, 1u);

    EXPECT_EQ(pnanovdb_editor_test::get_object_process_step_count(&editor, scene_token, name_token), 2u);
    EXPECT_EQ(pnanovdb_editor_test::get_object_process_step_type(&editor, scene_token, name_token, 1),
              pnanovdb_pipeline_type_noop);
}

TEST_F(PipelineApiTest, ProcessStepApiAddsEditsAndMapsSteps)
{
    EXPECT_EQ(editor.get_process_step_count(&editor, scene_token, name_token), 1u);
    EXPECT_EQ(editor.get_process_step(&editor, scene_token, name_token, 0), pnanovdb_pipeline_type_noop);

    editor.set_process_step(&editor, scene_token, name_token, 1, pnanovdb_pipeline_type_voxelbvh_build);
    EXPECT_EQ(editor.get_process_step_count(&editor, scene_token, name_token), 2u);
    EXPECT_EQ(editor.get_process_step(&editor, scene_token, name_token, 1), pnanovdb_pipeline_type_voxelbvh_build);

    editor.set_process_step(&editor, scene_token, name_token, 0, pnanovdb_pipeline_type_voxelbvh_rgba8);
    EXPECT_EQ(editor.get_process_step_count(&editor, scene_token, name_token), 2u);
    EXPECT_EQ(editor.get_process_step(&editor, scene_token, name_token, 0), pnanovdb_pipeline_type_voxelbvh_rgba8);

    EXPECT_EQ(editor.get_pipeline(&editor, scene_token, name_token, pnanovdb_pipeline_stage_process),
              pnanovdb_pipeline_type_voxelbvh_rgba8);

    editor.set_process_step(&editor, scene_token, name_token, 99, pnanovdb_pipeline_type_voxelbvh_build);
    EXPECT_EQ(editor.get_process_step_count(&editor, scene_token, name_token), 2u);

    editor.set_process_step(&editor, scene_token, name_token, 1, pnanovdb_pipeline_type_voxelbvh_rgba8_chain);
    EXPECT_EQ(editor.get_process_step(&editor, scene_token, name_token, 1), pnanovdb_pipeline_type_voxelbvh_build);

    pnanovdb_pipeline_params_t* params = editor.map_process_step_params(&editor, scene_token, name_token, 1);
    ASSERT_NE(params, nullptr);
    EXPECT_GT(params->size, 0u);
    editor.unmap_process_step_params(&editor, scene_token, name_token, 1);
    EXPECT_EQ(pnanovdb_editor_test::get_process_step_dirty(&editor, scene_token, name_token, 1), PNANOVDB_TRUE);

    EXPECT_EQ(editor.map_process_step_params(&editor, scene_token, name_token, 99), nullptr);
    editor.unmap_process_step_params(&editor, scene_token, name_token, 99);

    EXPECT_EQ(editor.get_process_step(&editor, scene_token, name_token, 99), pnanovdb_pipeline_type_noop);
}


TEST_F(PipelineApiTest, RenderComboUsesNewestProcessOutputNotConfiguredDownstream)
{
    editor.set_process_step(&editor, scene_token, name_token, 0, pnanovdb_pipeline_type_voxelbvh_build);
    pnanovdb_compute_array_t* step0_out = compute.create_array(
        sizeof(uint8_t), owned_array->element_count, static_cast<const uint8_t*>(owned_array->data));
    ASSERT_NE(step0_out, nullptr);
    pnanovdb_editor_test::set_process_step_nanovdb_output(&editor, scene_token, name_token, 0, step0_out, &compute);

    EXPECT_EQ(pnanovdb_editor_test::get_object_renderable_data_kind(&editor, scene_token, name_token),
              (pnanovdb_uint32_t)pnanovdb_pipeline_data_kind_voxelbvh);

    editor.set_process_step(&editor, scene_token, name_token, 1, pnanovdb_pipeline_type_voxelbvh_rgba8);
    EXPECT_EQ(pnanovdb_editor_test::get_object_renderable_data_kind(&editor, scene_token, name_token),
              (pnanovdb_uint32_t)pnanovdb_pipeline_data_kind_voxelbvh);

    editor.set_pipeline(
        &editor, scene_token, name_token, pnanovdb_pipeline_stage_render, pnanovdb_pipeline_type_nanovdb_render);
    pnanovdb_editor_test::sync_object_render_to_chain(&editor, scene_token, name_token);
    EXPECT_NE(pnanovdb_editor_test::get_object_render_pipeline(&editor, scene_token, name_token),
              pnanovdb_pipeline_type_nanovdb_render);

    pnanovdb_compute_array_t* step1_out = compute.create_array(
        sizeof(uint8_t), owned_array->element_count, static_cast<const uint8_t*>(owned_array->data));
    ASSERT_NE(step1_out, nullptr);
    pnanovdb_editor_test::set_process_step_nanovdb_output(&editor, scene_token, name_token, 1, step1_out, &compute);
    EXPECT_EQ(pnanovdb_editor_test::get_object_renderable_data_kind(&editor, scene_token, name_token),
              (pnanovdb_uint32_t)pnanovdb_pipeline_data_kind_nanovdb_rgba8);

    pnanovdb_editor_test::sync_object_render_to_chain(&editor, scene_token, name_token);
    EXPECT_EQ(pnanovdb_editor_test::get_object_render_pipeline(&editor, scene_token, name_token),
              pnanovdb_pipeline_type_nanovdb_render);
}

TEST_F(PipelineApiTest, ReplacingProcessStepClearsItsOutputAndDownstreamOutputs)
{
    editor.set_process_step(&editor, scene_token, name_token, 0, pnanovdb_pipeline_type_voxelbvh_build);
    pnanovdb_compute_array_t* step0_out = compute.create_array(
        sizeof(uint8_t), owned_array->element_count, static_cast<const uint8_t*>(owned_array->data));
    ASSERT_NE(step0_out, nullptr);
    pnanovdb_editor_test::set_process_step_nanovdb_output(&editor, scene_token, name_token, 0, step0_out, &compute);

    editor.set_process_step(&editor, scene_token, name_token, 1, pnanovdb_pipeline_type_voxelbvh_rgba8);
    pnanovdb_compute_array_t* step1_out = compute.create_array(
        sizeof(uint8_t), owned_array->element_count, static_cast<const uint8_t*>(owned_array->data));
    ASSERT_NE(step1_out, nullptr);
    pnanovdb_editor_test::set_process_step_nanovdb_output(&editor, scene_token, name_token, 1, step1_out, &compute);
    ASSERT_EQ(pnanovdb_editor_test::get_object_renderable_data_kind(&editor, scene_token, name_token),
              (pnanovdb_uint32_t)pnanovdb_pipeline_data_kind_nanovdb_rgba8);

    editor.set_process_step(&editor, scene_token, name_token, 0, pnanovdb_pipeline_type_voxelbvh_rgba8);

    // Until the replacement chain runs, rendering must fall back to the load-stage NanoVDB.
    EXPECT_EQ(pnanovdb_editor_test::get_object_renderable_data_kind(&editor, scene_token, name_token),
              (pnanovdb_uint32_t)pnanovdb_pipeline_data_kind_nanovdb);
}

TEST_F(PipelineApiTest, CreateVariantRejectsExistingDestination)
{
    pnanovdb_editor_token_t* destination = editor.get_token("variant_collision");
    editor.add_nanovdb_2(&editor, scene_token, destination, owned_array);

    pnanovdb_compute_array_t* original = nullptr;
    editor.impl->scene_manager->with_object(scene_token, destination,
                                            [&](pnanovdb_editor::SceneObject* obj)
                                            {
                                                ASSERT_NE(obj, nullptr);
                                                original = obj->resources.nanovdb_array;
                                            });

    EXPECT_FALSE(pnanovdb_editor::pipeline_create_variant(
        editor.impl->scene_manager, scene_token, name_token, destination->str));
    editor.impl->scene_manager->with_object(scene_token, destination,
                                            [&](pnanovdb_editor::SceneObject* obj)
                                            {
                                                ASSERT_NE(obj, nullptr);
                                                EXPECT_EQ(obj->type, pnanovdb_editor::SceneObjectType::NanoVDB);
                                                EXPECT_EQ(obj->resources.nanovdb_array, original);
                                            });
    editor.remove(&editor, scene_token, destination);
}

TEST_F(PipelineApiTest, ConcurrentVariantCreationHasSingleWinner)
{
    constexpr const char* destination_name = "variant_race";
    std::atomic<bool> start{ false };
    bool created[2] = { false, false };
    std::thread threads[2];
    for (int i = 0; i < 2; ++i)
    {
        threads[i] = std::thread(
            [&, i]()
            {
                while (!start.load(std::memory_order_acquire))
                    std::this_thread::yield();
                created[i] = pnanovdb_editor::pipeline_create_variant(
                    editor.impl->scene_manager, scene_token, name_token, destination_name);
            });
    }
    start.store(true, std::memory_order_release);
    for (auto& thread : threads)
        thread.join();

    EXPECT_EQ(static_cast<int>(created[0]) + static_cast<int>(created[1]), 1);
    pnanovdb_editor_token_t* destination = editor.get_token(destination_name);
    editor.impl->scene_manager->with_object(scene_token, destination,
                                            [](pnanovdb_editor::SceneObject* obj)
                                            {
                                                ASSERT_NE(obj, nullptr);
                                                EXPECT_NE(obj->type, pnanovdb_editor::SceneObjectType::Uninitialized);
                                            });
    editor.remove(&editor, scene_token, destination);
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

    if (pnanovdb_editor_test::should_skip_on_software_renderer(phys_desc.device_name))
    {
        const std::string skip_reason = pnanovdb_editor_test::software_renderer_skip_reason(
            phys_desc.device_name, "pipeline scheduler-kick test (headless render loop too slow)");
        compute.device_interface.destroy_device_manager(device_manager);
        pnanovdb_compute_free(&compute);
        pnanovdb_compiler_free(&compiler);
        GTEST_SKIP() << skip_reason;
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
    pnanovdb_compute_array_t* nanovdb_array = compute.create_array(4u, sphere.bufferSize() / 4u, sphere.data());
    ASSERT_NE(nanovdb_array, nullptr);
    editor.add_nanovdb_2(&editor, scene, name, nanovdb_array);

    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    EXPECT_EQ(editor.get_pipeline(&editor, scene, name, pnanovdb_pipeline_stage_process), pnanovdb_pipeline_type_noop);
    EXPECT_EQ(pnanovdb_editor_test::get_object_process_dirty(&editor, scene, name), PNANOVDB_TRUE);

    editor.set_pipeline(&editor, scene, name, pnanovdb_pipeline_stage_process, pnanovdb_pipeline_type_voxelbvh_build);

    bool kicked = false;
    for (int i = 0; i < 100 && !kicked; ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        kicked = (pnanovdb_editor_test::get_object_process_dirty(&editor, scene, name) == PNANOVDB_FALSE);
    }
    EXPECT_TRUE(kicked) << "Scheduler did not clear process_dirty after set_pipeline(stage_process, !noop) (device='"
                        << phys_desc.device_name << "')";

    ASSERT_EQ(pnanovdb_editor_test::get_object_process_dirty(&editor, scene, name), PNANOVDB_FALSE);
    editor.mark_pipeline_dirty(&editor, scene, name);

    bool re_kicked = false;
    for (int i = 0; i < 100 && !re_kicked; ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        re_kicked = (pnanovdb_editor_test::get_object_process_dirty(&editor, scene, name) == PNANOVDB_FALSE);
    }
    EXPECT_TRUE(re_kicked) << "Scheduler did not re-run the process pipeline after mark_pipeline_dirty (device='"
                           << phys_desc.device_name << "')";

    editor.stop(&editor);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    pnanovdb_editor_free(&editor);

    compute.destroy_array(nanovdb_array);
    compute.device_interface.destroy_device(device_manager, device);
    compute.device_interface.destroy_device_manager(device_manager);
    pnanovdb_compute_free(&compute);
    pnanovdb_compiler_free(&compiler);
}
