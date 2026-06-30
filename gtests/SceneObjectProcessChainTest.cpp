// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

#include "editor/EditorImport.h"
#include "editor/EditorSceneManager.h"
#include "editor/EditorToken.h"
#include "editor/Pipeline.h"
#include "editor/PipelineRegistry.h"

#include <gtest/gtest.h>

#include <nanovdb/PNanoVDB.h>
#include <nanovdb/tools/CreatePrimitives.h>

#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace pnanovdb_editor
{
namespace
{

pnanovdb_compute_array_t* fake_array(uintptr_t value)
{
    return reinterpret_cast<pnanovdb_compute_array_t*>(value);
}

void set_step(SceneObject& obj, size_t index, pnanovdb_pipeline_type_t type, pnanovdb_compute_array_t* output)
{
    while (index >= obj.pipeline.process_count())
    {
        obj.pipeline.append_process_step();
    }
    PipelineStage& step = obj.pipeline.process_step(index);
    step.type = type;
    step.dirty = false;
    if (output)
    {
        step.output.set_array(k_stage_output_nanovdb, output, {});
    }
}

std::vector<uint32_t> make_metadata_grid(const std::vector<uint32_t>& value_sizes,
                                         const std::vector<uint64_t>& value_counts)
{
    if (value_sizes.size() != value_counts.size())
    {
        return {};
    }
    auto sphere = nanovdb::tools::createLevelSetSphere<float>(4.0f);
    pnanovdb_buf_t src = pnanovdb_make_buf(static_cast<uint32_t*>(sphere.data()), sphere.bufferSize() / sizeof(uint32_t));
    pnanovdb_grid_handle_t grid = { pnanovdb_address_null() };
    const uint64_t base_size = pnanovdb_grid_get_grid_size(src, grid);
    const uint32_t count = static_cast<uint32_t>(value_sizes.size());
    const uint64_t header_size = uint64_t(count) * PNANOVDB_GRIDBLINDMETADATA_SIZE;
    uint64_t total_size = base_size + header_size;
    for (uint32_t i = 0; i < count; ++i)
    {
        total_size += value_sizes[i] * value_counts[i];
    }

    std::vector<uint32_t> words((total_size + sizeof(uint32_t) - 1u) / sizeof(uint32_t), 0u);
    std::memcpy(words.data(), sphere.data(), base_size);
    pnanovdb_buf_t buf = pnanovdb_make_buf(words.data(), words.size());
    pnanovdb_grid_set_blind_metadata_offset(buf, grid, base_size);
    pnanovdb_grid_set_blind_metadata_count(buf, grid, count);

    uint64_t payload = base_size + header_size;
    for (uint32_t i = 0; i < count; ++i)
    {
        pnanovdb_gridblindmetadata_handle_t meta = pnanovdb_grid_get_gridblindmetadata(buf, grid, i);
        pnanovdb_address_t payload_address = { payload };
        pnanovdb_gridblindmetadata_set_data_offset(buf, meta, pnanovdb_address_diff(payload_address, meta.address));
        pnanovdb_gridblindmetadata_set_value_count(buf, meta, value_counts[i]);
        pnanovdb_gridblindmetadata_set_value_size(buf, meta, value_sizes[i]);
        pnanovdb_gridblindmetadata_set_data_class(buf, meta, 0u);
        payload += value_sizes[i] * value_counts[i];
    }
    pnanovdb_grid_set_grid_size(buf, grid, payload);
    return words;
}

std::vector<uint32_t> make_mesh_metadata_grid()
{
    return make_metadata_grid({ 8u, 4u, 4u, 4u, 4u }, { 1u, 1u, 3u, 9u, 9u });
}

uint8_t* metadata_payload(std::vector<uint32_t>& words, uint32_t metadata_index)
{
    pnanovdb_buf_t buf = pnanovdb_make_buf(words.data(), words.size());
    pnanovdb_grid_handle_t grid = { pnanovdb_address_null() };
    const pnanovdb_address_t address = pnanovdb_grid_get_gridblindmetadata_value_address(buf, grid, metadata_index);
    return reinterpret_cast<uint8_t*>(words.data()) + address.byte_offset;
}

PipelineContext validation_context(EditorSceneManager& scene_manager)
{
    static pnanovdb_compute_t compute{};
    static pnanovdb_voxelbvh_t voxelbvh{};
    PipelineContext ctx;
    ctx.compute = &compute;
    ctx.queue = reinterpret_cast<pnanovdb_compute_queue_t*>(uintptr_t{ 1 });
    ctx.voxelbvh = &voxelbvh;
    ctx.voxelbvh_ctx = reinterpret_cast<pnanovdb_voxelbvh_context_t*>(uintptr_t{ 1 });
    ctx.scene_manager = &scene_manager;
    return ctx;
}

TEST(SceneObjectProcessChainTest, VoxelBvhBuildParamsRejectInvalidSourceAndClampResolution)
{
    pnanovdb_pipeline_params_t params{};
    EXPECT_FALSE(pnanovdb_pipeline_voxelbvh_build_params_set_source_type(
        &params, static_cast<pnanovdb_pipeline_voxelbvh_source_t>(99)));
    EXPECT_EQ(params.data, nullptr);

    ASSERT_TRUE(pnanovdb_pipeline_voxelbvh_build_params_set_resolution(&params, 0u));
    uint32_t stored_resolution = 0u;
    std::memcpy(&stored_resolution, static_cast<const uint8_t*>(params.data) + sizeof(uint32_t), sizeof(uint32_t));
    EXPECT_EQ(stored_resolution, 1u);

    ASSERT_TRUE(pnanovdb_pipeline_voxelbvh_build_params_set_resolution(&params, std::numeric_limits<uint32_t>::max()));
    std::memcpy(&stored_resolution, static_cast<const uint8_t*>(params.data) + sizeof(uint32_t), sizeof(uint32_t));
    EXPECT_EQ(stored_resolution, PNANOVDB_VOXELBVH_MAX_RESOLUTION);
    free(params.data);
}

TEST(SceneObjectProcessChainTest, VoxelBvhExecuteRejectsMappedInvalidSource)
{
    EditorSceneManager manager;
    SceneObject object;
    object.pipeline.process().type = pnanovdb_pipeline_type_voxelbvh_build;
    object.pipeline.process().dirty = true;
    ASSERT_TRUE(pnanovdb_pipeline_voxelbvh_build_params_set_resolution(&object.pipeline.process().params, 64u));
    const uint32_t invalid_source = 99u;
    std::memcpy(object.pipeline.process().params.data, &invalid_source, sizeof(invalid_source));

    EXPECT_EQ(pipeline_execute_process(&object, validation_context(manager)), pnanovdb_pipeline_result_error);
}

TEST(SceneObjectProcessChainTest, RunRestoresDroppedPrerequisite)
{
    SceneObject obj;
    set_step(obj, 0, pnanovdb_pipeline_type_voxelbvh_build, nullptr);
    set_step(obj, 1, pnanovdb_pipeline_type_voxelbvh_rgba8, fake_array(2));
    obj.pipeline.drop_intermediate = true;

    scene_object_invalidate_process_from(&obj, 1);

    ASSERT_TRUE(obj.pipeline.process_run_snapshot.has_value());
    EXPECT_EQ(obj.pipeline.process_run_snapshot->from_step, 0);
    EXPECT_TRUE(obj.pipeline.process_step(0).dirty);
    EXPECT_TRUE(obj.pipeline.process_step(1).dirty);
    EXPECT_TRUE(obj.pipeline.process_step(1).output.empty());
}

TEST(SceneObjectProcessChainTest, RemoveMiddleStepRebuildsFromDroppedProducer)
{
    SceneObject obj;
    set_step(obj, 0, pnanovdb_pipeline_type_voxelbvh_build, nullptr);
    set_step(obj, 1, pnanovdb_pipeline_type_voxelbvh_rgba8, fake_array(2));
    set_step(obj, 2, pnanovdb_pipeline_type_voxelbvh_rgba8, fake_array(3));
    obj.pipeline.drop_intermediate = true;
    scene_object_resolve_resources(&obj);
    ASSERT_EQ(obj.nanovdb_array(), fake_array(3));

    ASSERT_TRUE(scene_object_remove_process_step(&obj, 1));

    ASSERT_EQ(obj.pipeline.process_count(), 2u);
    EXPECT_EQ(obj.pipeline.process_step(1).type, pnanovdb_pipeline_type_voxelbvh_rgba8);
    EXPECT_TRUE(obj.pipeline.process_step(0).dirty);
    EXPECT_TRUE(obj.pipeline.process_step(1).dirty);
    EXPECT_TRUE(obj.pipeline.process_step(1).output.empty());
    EXPECT_EQ(obj.nanovdb_array(), nullptr);
    EXPECT_EQ(obj.converted_nanovdb(), nullptr);
}

TEST(SceneObjectProcessChainTest, RemoveLastStepKeepsAvailableProducer)
{
    SceneObject obj;
    set_step(obj, 0, pnanovdb_pipeline_type_voxelbvh_build, fake_array(1));
    set_step(obj, 1, pnanovdb_pipeline_type_voxelbvh_rgba8, fake_array(2));
    scene_object_resolve_resources(&obj);

    ASSERT_TRUE(scene_object_remove_process_step(&obj, 1));

    ASSERT_EQ(obj.pipeline.process_count(), 1u);
    EXPECT_FALSE(obj.pipeline.process_step(0).dirty);
    EXPECT_EQ(obj.nanovdb_array(), fake_array(1));
    EXPECT_EQ(obj.converted_nanovdb(), fake_array(1));
}

TEST(SceneObjectProcessChainTest, RemovalKeepsRunningMissingProducerValid)
{
    SceneObject obj;
    set_step(obj, 0, pnanovdb_pipeline_type_voxelbvh_build, nullptr);
    set_step(obj, 1, pnanovdb_pipeline_type_voxelbvh_rgba8, fake_array(2));
    PipelineStage& producer = obj.pipeline.process_step(0);
    producer.dirty = true; // queued or running
    const uint64_t revision = producer.revision;

    ASSERT_TRUE(scene_object_remove_process_step(&obj, 1));

    EXPECT_TRUE(producer.dirty);
    EXPECT_EQ(producer.revision, revision);
}

TEST(SceneObjectProcessChainTest, MarkDirtyArmsFirstExecutableStepAfterNoop)
{
    SceneObject obj;
    set_step(obj, 0, pnanovdb_pipeline_type_noop, nullptr);
    set_step(obj, 1, pnanovdb_pipeline_type_voxelbvh_build, fake_array(1));
    set_step(obj, 2, pnanovdb_pipeline_type_voxelbvh_rgba8, fake_array(2));

    scene_object_mark_process_dirty(&obj);

    EXPECT_FALSE(obj.pipeline.process_step(0).dirty);
    EXPECT_TRUE(obj.pipeline.process_step(1).dirty);
    EXPECT_FALSE(obj.pipeline.process_step(2).dirty);
    EXPECT_EQ(scene_object_next_dirty_process_step(&obj), 1);
}

TEST(SceneObjectProcessChainTest, Image2DRenderAcceptsGeneratedRgba8Grid)
{
    const pnanovdb_pipeline_descriptor_t* desc = pnanovdb_pipeline_get_descriptor(pnanovdb_pipeline_type_image2d_render);
    ASSERT_NE(desc, nullptr);
    EXPECT_NE(desc->inputs & pnanovdb_pipeline_data_kind_nanovdb_rgba8, 0u);
}

TEST(SceneObjectProcessChainTest, OrdinaryNanoVdbIsNotCompatibleRgba8Input)
{
    auto sphere = nanovdb::tools::createLevelSetSphere<float>(4.0f);
    pnanovdb_compute_array_t array{};
    array.data = sphere.data();
    array.element_size = sizeof(uint32_t);
    array.element_count = sphere.bufferSize() / sizeof(uint32_t);
    ASSERT_FALSE(nanovdb_import::has_voxelbvh_mesh_metadata(&array));

    SceneObject obj;
    obj.type = SceneObjectType::NanoVDB;
    obj.pipeline.load().output.set_array(k_stage_output_nanovdb, &array, {});
    set_step(obj, 0, pnanovdb_pipeline_type_voxelbvh_rgba8, nullptr);
    obj.pipeline.process_step(0).dirty = true;
    scene_object_resolve_resources(&obj);

    EditorSceneManager scene_manager;
    EXPECT_EQ(pipeline_execute_process(&obj, validation_context(scene_manager)), pnanovdb_pipeline_result_no_data);
}

TEST(SceneObjectProcessChainTest, OversizedReportedGridSizeFallsBackToArrayBounds)
{
    std::vector<uint32_t> words = make_mesh_metadata_grid();
    pnanovdb_compute_array_t array{};
    array.data = words.data();
    array.element_size = sizeof(uint32_t);
    array.element_count = words.size();
    ASSERT_TRUE(nanovdb_import::has_voxelbvh_mesh_metadata(&array));

    pnanovdb_buf_t buf = pnanovdb_make_buf(words.data(), words.size());
    pnanovdb_grid_handle_t grid = { pnanovdb_address_null() };
    pnanovdb_grid_set_grid_size(buf, grid, uint64_t(words.size()) * sizeof(uint32_t) + 1u);
    EXPECT_TRUE(nanovdb_import::has_voxelbvh_mesh_metadata(&array));
}

TEST(SceneObjectProcessChainTest, MeshMetadataDetectionDoesNotValidateRelations)
{
    std::vector<uint32_t> words = make_mesh_metadata_grid();
    pnanovdb_compute_array_t array{ words.data(), sizeof(uint32_t), words.size() };
    ASSERT_TRUE(nanovdb_import::has_voxelbvh_mesh_metadata(&array));

    const uint64_t invalid_range = uint64_t{ 2u } << 32u;
    std::memcpy(metadata_payload(words, 0u), &invalid_range, sizeof(invalid_range));
    EXPECT_TRUE(nanovdb_import::has_voxelbvh_mesh_metadata(&array));

    words = make_mesh_metadata_grid();
    array.data = words.data();
    const uint32_t invalid_primitive = 1u;
    std::memcpy(metadata_payload(words, 1u), &invalid_primitive, sizeof(invalid_primitive));
    EXPECT_TRUE(nanovdb_import::has_voxelbvh_mesh_metadata(&array));

    words = make_mesh_metadata_grid();
    array.data = words.data();
    const uint32_t invalid_vertex = 3u;
    std::memcpy(metadata_payload(words, 2u), &invalid_vertex, sizeof(invalid_vertex));
    EXPECT_TRUE(nanovdb_import::has_voxelbvh_mesh_metadata(&array));
}

TEST(SceneObjectProcessChainTest, MeshMetadataDetectionDoesNotValidateVertexColorCardinality)
{
    std::vector<uint32_t> words = make_metadata_grid({ 8u, 4u, 4u, 4u, 4u }, { 1u, 1u, 3u, 9u, 6u });
    pnanovdb_compute_array_t array{ words.data(), sizeof(uint32_t), words.size() };
    EXPECT_TRUE(nanovdb_import::has_voxelbvh_mesh_metadata(&array));
}

TEST(SceneObjectProcessChainTest, RendererMetadataUsesRequiredCountAndValueSizeOnly)
{
    std::vector<uint32_t> gaussian_words =
        make_metadata_grid({ 8u, 4u, 4u, 4u, 4u, 4u, 4u, 4u }, { 1u, 1u, 3u, 1u, 4u, 3u, 3u, 45u });
    pnanovdb_compute_array_t gaussian_array{ gaussian_words.data(), sizeof(uint32_t), gaussian_words.size() };
    EXPECT_TRUE(nanovdb_import::has_voxelbvh_render_metadata(
        &gaussian_array, pnanovdb_pipeline_type_voxelbvh_gaussians_render));
    EXPECT_TRUE(nanovdb_import::has_voxelbvh_mesh_metadata(&gaussian_array));
    EXPECT_TRUE(nanovdb_import::has_voxelbvh_render_metadata(
        &gaussian_array, pnanovdb_pipeline_type_voxelbvh_triangles_render));

    std::vector<uint32_t> invalid_gaussian_words =
        make_metadata_grid({ 8u, 4u, 4u, 4u, 4u, 4u, 4u, 4u }, { 1u, 1u, 3u, 1u, 4u, 3u, 3u, 42u });
    pnanovdb_compute_array_t invalid_gaussian_array{ invalid_gaussian_words.data(), sizeof(uint32_t),
                                                     invalid_gaussian_words.size() };
    EXPECT_TRUE(nanovdb_import::has_voxelbvh_render_metadata(
        &invalid_gaussian_array, pnanovdb_pipeline_type_voxelbvh_gaussians_render));

    std::vector<uint32_t> line_words = make_metadata_grid({ 8u, 4u, 8u, 4u, 4u }, { 1u, 1u, 1u, 6u, 6u });
    pnanovdb_compute_array_t line_array{ line_words.data(), sizeof(uint32_t), line_words.size() };
    EXPECT_FALSE(
        nanovdb_import::has_voxelbvh_render_metadata(&line_array, pnanovdb_pipeline_type_voxelbvh_lines_render));
    EXPECT_FALSE(
        nanovdb_import::has_voxelbvh_render_metadata(&line_array, pnanovdb_pipeline_type_voxelbvh_triangles_render));

    std::vector<uint32_t> packed_triangle_words = make_metadata_grid({ 8u, 4u, 12u, 4u, 4u }, { 1u, 1u, 1u, 9u, 9u });
    pnanovdb_compute_array_t packed_triangle_array{ packed_triangle_words.data(), sizeof(uint32_t),
                                                    packed_triangle_words.size() };
    EXPECT_FALSE(nanovdb_import::has_voxelbvh_render_metadata(
        &packed_triangle_array, pnanovdb_pipeline_type_voxelbvh_triangles_render));
    EXPECT_FALSE(nanovdb_import::has_voxelbvh_render_metadata(
        &packed_triangle_array, pnanovdb_pipeline_type_voxelbvh_lines_render));

    std::vector<uint32_t> flat_line_words = make_metadata_grid({ 8u, 4u, 4u, 4u, 4u }, { 1u, 1u, 2u, 6u, 6u });
    pnanovdb_compute_array_t flat_line_array{ flat_line_words.data(), sizeof(uint32_t), flat_line_words.size() };
    EXPECT_TRUE(
        nanovdb_import::has_voxelbvh_render_metadata(&flat_line_array, pnanovdb_pipeline_type_voxelbvh_lines_render));
    EXPECT_TRUE(nanovdb_import::has_voxelbvh_render_metadata(
        &flat_line_array, pnanovdb_pipeline_type_voxelbvh_triangles_render));
}

TEST(SceneObjectProcessChainTest, LineSourceCannotEnterTriangleRgba8Worker)
{
    std::vector<uint32_t> words = make_mesh_metadata_grid();
    pnanovdb_compute_array_t array{};
    array.data = words.data();
    array.element_size = sizeof(uint32_t);
    array.element_count = words.size();
    ASSERT_TRUE(nanovdb_import::has_voxelbvh_mesh_metadata(&array));

    pnanovdb_compute_array_t positions{};
    pnanovdb_compute_array_t line_indices{};
    line_indices.element_size = 2u * sizeof(uint32_t);
    SceneObject obj;
    obj.type = SceneObjectType::Array;
    obj.resources.named_arrays["positions"] = &positions;
    obj.resources.named_arrays["indices"] = &line_indices;
    set_step(obj, 0, pnanovdb_pipeline_type_voxelbvh_build, &array);
    set_step(obj, 1, pnanovdb_pipeline_type_voxelbvh_rgba8, nullptr);
    obj.pipeline.process_step(1).dirty = true;
    scene_object_resolve_resources(&obj);

    EditorSceneManager scene_manager;
    EXPECT_EQ(pipeline_execute_process(&obj, validation_context(scene_manager)), pnanovdb_pipeline_result_no_data);
}

TEST(SceneObjectProcessChainTest, TriangleSourcePassesRgba8ExecutionGate)
{
    std::vector<uint32_t> words = make_mesh_metadata_grid();
    pnanovdb_compute_array_t array{};
    array.data = words.data();
    array.element_size = sizeof(uint32_t);
    array.element_count = words.size();

    pnanovdb_compute_array_t positions{};
    pnanovdb_compute_array_t triangle_indices{};
    triangle_indices.element_size = sizeof(uint32_t);
    SceneObject obj;
    obj.type = SceneObjectType::Array;
    obj.resources.named_arrays["positions"] = &positions;
    obj.resources.named_arrays["indices"] = &triangle_indices;
    set_step(obj, 0, pnanovdb_pipeline_type_voxelbvh_build, &array);
    set_step(obj, 1, pnanovdb_pipeline_type_voxelbvh_rgba8, nullptr);
    obj.pipeline.process_step(1).dirty = true;
    scene_object_resolve_resources(&obj);

    EditorSceneManager scene_manager;
    EXPECT_EQ(pipeline_execute_process(&obj, validation_context(scene_manager)), pnanovdb_pipeline_result_pending);
}

TEST(SceneObjectProcessChainTest, CancelWithoutSnapshotPreservesNewerDownstreamWork)
{
    SceneObject obj;
    set_step(obj, 0, pnanovdb_pipeline_type_voxelbvh_build, nullptr);
    set_step(obj, 1, pnanovdb_pipeline_type_voxelbvh_rgba8, nullptr);
    obj.pipeline.process_step(0).dirty = true;
    obj.pipeline.process_step(1).dirty = true;
    obj.pipeline.process_step(1).bump_revision();
    obj.pipeline.process_run_snapshot.reset();

    scene_object_cancel_running_process_step_without_snapshot(&obj, 0);

    EXPECT_FALSE(obj.pipeline.process_step(0).dirty);
    EXPECT_TRUE(obj.pipeline.process_step(1).dirty);
}

TEST(SceneObjectProcessChainTest, CancelRestoresDirtyWorkThatPredatedRun)
{
    SceneObject obj;
    set_step(obj, 0, pnanovdb_pipeline_type_voxelbvh_build, fake_array(1));
    set_step(obj, 1, pnanovdb_pipeline_type_voxelbvh_rgba8, fake_array(2));
    obj.pipeline.process_step(1).dirty = true;

    scene_object_invalidate_process_from(&obj, 0);
    ASSERT_TRUE(obj.pipeline.process_run_snapshot.has_value());
    obj.pipeline.active_process_step = 0;
    scene_object_restore_process_run_snapshot(&obj);

    EXPECT_FALSE(obj.pipeline.process_step(0).dirty);
    EXPECT_TRUE(obj.pipeline.process_step(1).dirty);
    EXPECT_EQ(obj.pipeline.process_step(0).output.get_array(k_stage_output_nanovdb), fake_array(1));
    EXPECT_EQ(obj.pipeline.process_step(1).output.get_array(k_stage_output_nanovdb), fake_array(2));
}

TEST(SceneObjectProcessChainTest, CancelRestoresRenderConfigurationAndShaderState)
{
    SceneObject obj;
    obj.type = SceneObjectType::NanoVDB;
    obj.nanovdb_array() = fake_array(1);
    set_step(obj, 0, pnanovdb_pipeline_type_voxelbvh_build, fake_array(2));
    scene_object_resolve_resources(&obj);

    PipelineStage& render = obj.pipeline.render();
    render.type = pnanovdb_pipeline_type_voxelbvh_debug_render;
    render.configured = true;
    render.dirty = false;
    render.params.size = sizeof(uint32_t);
    render.params.data = malloc(render.params.size);
    ASSERT_NE(render.params.data, nullptr);
    const uint32_t expected_render_params = 0xa5b6c7d8u;
    std::memcpy(render.params.data, &expected_render_params, sizeof(expected_render_params));

    const auto original_shader_storage = obj.params.shader_name_storage;
    ASSERT_TRUE(original_shader_storage);
    original_shader_storage->object_key = 42u;
    pnanovdb_editor_token_t* expected_shader = EditorToken::getInstance().getToken("cancel_restore_custom_shader.slang");
    obj.shader_name() = expected_shader;

    const auto shader_value = std::make_shared<uint32_t>(0x12345678u);
    const auto shader_params_owner = std::shared_ptr<pnanovdb_compute_array_t>(
        new pnanovdb_compute_array_t{}, [shader_value](pnanovdb_compute_array_t* array) { delete array; });
    shader_params_owner->data = shader_value.get();
    shader_params_owner->element_size = sizeof(*shader_value);
    shader_params_owner->element_count = 1u;
    const pnanovdb_reflect_data_type_t* expected_shader_type = PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_uint32_t);
    obj.params.shader_params_array = shader_params_owner.get();
    obj.params.shader_params_array_owner = shader_params_owner;
    obj.shader_params() = shader_params_owner->data;
    obj.shader_params_data_type() = expected_shader_type;

    scene_object_invalidate_process_from(&obj, 0);
    ASSERT_TRUE(obj.pipeline.process_run_snapshot.has_value());
    ASSERT_EQ(obj.render_pipeline(), pnanovdb_pipeline_type_nanovdb_render);
    EXPECT_EQ(obj.shader_name(), nullptr);
    EXPECT_EQ(obj.shader_params(), nullptr);
    EXPECT_EQ(obj.shader_params_data_type(), nullptr);

    scene_object_restore_process_run_snapshot(&obj);

    EXPECT_EQ(obj.render_pipeline(), pnanovdb_pipeline_type_voxelbvh_debug_render);
    ASSERT_EQ(obj.render_params().size, sizeof(expected_render_params));
    ASSERT_NE(obj.render_params().data, nullptr);
    uint32_t restored_render_params = 0u;
    std::memcpy(&restored_render_params, obj.render_params().data, sizeof(restored_render_params));
    EXPECT_EQ(restored_render_params, expected_render_params);
    EXPECT_EQ(obj.params.shader_name_storage, original_shader_storage);
    EXPECT_EQ(obj.params.shader_name_storage->object_key, 42u);
    EXPECT_EQ(obj.shader_name(), expected_shader);
    EXPECT_EQ(obj.params.shader_params_array, shader_params_owner.get());
    EXPECT_EQ(obj.params.shader_params_array_owner, shader_params_owner);
    EXPECT_EQ(obj.shader_params(), shader_value.get());
    EXPECT_EQ(obj.shader_params_data_type(), expected_shader_type);
}

TEST(SceneObjectProcessChainTest, CancelCompletionKeepsRestoredActiveStepDirty)
{
    SceneObject obj;
    set_step(obj, 0, pnanovdb_pipeline_type_voxelbvh_build, fake_array(1));
    set_step(obj, 1, pnanovdb_pipeline_type_voxelbvh_rgba8, fake_array(2));
    obj.pipeline.process_step(0).dirty = true;
    obj.pipeline.process_step(1).dirty = true;

    scene_object_invalidate_process_from(&obj, 0);
    ASSERT_TRUE(obj.pipeline.process_run_snapshot.has_value());
    obj.pipeline.active_process_step = 0;

    // request_user_cancel restores the snapshot immediately and records that
    // it did so. Completion must not run the no-snapshot cleanup a second time.
    const bool snapshot_restored = obj.pipeline.process_run_snapshot.has_value();
    scene_object_restore_process_run_snapshot(&obj);
    if (!snapshot_restored)
    {
        scene_object_cancel_running_process_step_without_snapshot(&obj, 0);
    }

    EXPECT_TRUE(obj.pipeline.process_step(0).dirty);
    EXPECT_TRUE(obj.pipeline.process_step(1).dirty);
    EXPECT_EQ(obj.pipeline.process_step(0).output.get_array(k_stage_output_nanovdb), fake_array(1));
    EXPECT_EQ(obj.pipeline.process_step(1).output.get_array(k_stage_output_nanovdb), fake_array(2));
}

TEST(SceneObjectProcessChainTest, FailedStepDisarmsDownstreamChain)
{
    SceneObject obj;
    set_step(obj, 0, pnanovdb_pipeline_type_voxelbvh_build, nullptr);
    set_step(obj, 1, pnanovdb_pipeline_type_voxelbvh_rgba8, nullptr);
    obj.pipeline.process_step(0).dirty = true;
    obj.pipeline.process_step(1).dirty = true;
    obj.pipeline.active_process_step = 0;

    scene_object_advance_process_chain(&obj, false);

    EXPECT_FALSE(obj.pipeline.process_step(0).dirty);
    EXPECT_FALSE(obj.pipeline.process_step(1).dirty);
    EXPECT_EQ(scene_object_next_dirty_process_step(&obj), -1);
}

} // namespace
} // namespace pnanovdb_editor
