// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

#include "editor/EditorSceneManager.h"
#include "raster/Raster.h"

#include <gtest/gtest.h>

#include <cstdint>

namespace pnanovdb_editor
{
namespace
{

int g_retained_array_destroy_count = 0;
int g_retained_gaussian_destroy_count = 0;

void count_retained_array_destroy(pnanovdb_compute_array_t*)
{
    ++g_retained_array_destroy_count;
}

void count_retained_gaussian_destroy(const pnanovdb_compute_t*, pnanovdb_compute_queue_t*, pnanovdb_raster_gaussian_data_t*)
{
    ++g_retained_gaussian_destroy_count;
}

TEST(SceneObjectReplacementTest, SourceResetClearsAliasesAndKeepsPipelineConfiguration)
{
    SceneObject obj;
    pnanovdb_compute_array_t nanovdb{};
    pnanovdb_compute_array_t named{};
    pnanovdb_compute_array_t converted{};
    pnanovdb_compute_array_t shader_params{};
    pnanovdb_raster_gaussian_data_t* gaussian = reinterpret_cast<pnanovdb_raster_gaussian_data_t*>(0x1);
    pnanovdb_camera_view_t camera{};

    obj.resources.nanovdb_array = &nanovdb;
    obj.resources.nanovdb_array_owner =
        std::shared_ptr<pnanovdb_compute_array_t>(&nanovdb, [](pnanovdb_compute_array_t*) {});
    obj.resources.gaussian_data = gaussian;
    obj.resources.gaussian_data_owner =
        std::shared_ptr<pnanovdb_raster_gaussian_data_t>(gaussian, [](pnanovdb_raster_gaussian_data_t*) {});
    obj.resources.converted_nanovdb = &converted;
    obj.resources.converted_nanovdb_owner =
        std::shared_ptr<pnanovdb_compute_array_t>(&converted, [](pnanovdb_compute_array_t*) {});
    obj.resources.named_arrays["positions"] = &named;
    obj.resources.named_array_owners["positions"] =
        std::shared_ptr<pnanovdb_compute_array_t>(&named, [](pnanovdb_compute_array_t*) {});
    obj.resources.source_filepath = "old-source.nvdb";
    obj.resources.camera_view = &camera;
    obj.resources.camera_view_owner = std::shared_ptr<pnanovdb_camera_view_t>(&camera, [](pnanovdb_camera_view_t*) {});
    obj.params.shader_params_array = &shader_params;
    obj.params.shader_params_array_owner =
        std::shared_ptr<pnanovdb_compute_array_t>(&shader_params, [](pnanovdb_compute_array_t*) {});
    obj.params.shader_params = shader_params.data;

    obj.pipeline.load().output.set_array(k_stage_output_nanovdb, &nanovdb, obj.resources.nanovdb_array_owner);
    obj.pipeline.process().type = pnanovdb_pipeline_type_voxelbvh_build;
    obj.pipeline.process().configured = true;
    obj.pipeline.process().params.size = 4;
    obj.pipeline.process().params.data = malloc(4);
    obj.pipeline.process().output.set_array(k_stage_output_nanovdb, &converted, obj.resources.converted_nanovdb_owner);
    const void* configured_params = obj.pipeline.process().params.data;

    obj.reset_source();

    EXPECT_EQ(obj.resources.nanovdb_array, nullptr);
    EXPECT_EQ(obj.resources.gaussian_data, nullptr);
    EXPECT_EQ(obj.resources.converted_nanovdb, nullptr);
    EXPECT_EQ(obj.resources.camera_view, nullptr);
    EXPECT_TRUE(obj.resources.named_arrays.empty());
    EXPECT_TRUE(obj.resources.named_array_owners.empty());
    EXPECT_FALSE(obj.resources.camera_view_owner);
    EXPECT_EQ(obj.params.shader_params_array, nullptr);
    EXPECT_FALSE(obj.params.shader_params_array_owner);
    EXPECT_EQ(obj.params.shader_params, nullptr);
    EXPECT_TRUE(obj.resources.source_filepath.empty());
    EXPECT_TRUE(obj.pipeline.load().output.empty());
    EXPECT_TRUE(obj.pipeline.process().output.empty());
    EXPECT_EQ(obj.pipeline.process().type, pnanovdb_pipeline_type_voxelbvh_build);
    EXPECT_TRUE(obj.pipeline.process().configured);
    EXPECT_EQ(obj.pipeline.process().params.data, configured_params);
    EXPECT_EQ(obj.pipeline.process().params.size, 4u);
}

TEST(SceneObjectReplacementTest, NamedArrayCopyPreservesBorrowedAndOwnedBindings)
{
    SceneObject source;
    SceneObject variant;
    pnanovdb_compute_array_t borrowed{};
    pnanovdb_compute_array_t owned{};
    auto owner = std::shared_ptr<pnanovdb_compute_array_t>(&owned, [](pnanovdb_compute_array_t*) {});

    source.resources.named_arrays["borrowed"] = &borrowed;
    source.resources.named_arrays["owned"] = &owned;
    source.resources.named_array_owners["owned"] = owner;

    variant.set_named_array_bindings(source.resources.named_arrays, source.resources.named_array_owners);

    ASSERT_EQ(variant.resources.named_arrays.size(), 2u);
    EXPECT_EQ(variant.resources.named_arrays.at("borrowed"), &borrowed);
    EXPECT_EQ(variant.resources.named_arrays.at("owned"), &owned);
    EXPECT_EQ(variant.resources.named_array_owners.count("borrowed"), 0u);
    ASSERT_EQ(variant.resources.named_array_owners.count("owned"), 1u);
    EXPECT_EQ(variant.resources.named_array_owners.at("owned").get(), &owned);
    EXPECT_EQ(variant.resources.named_array_owners.at("owned"), owner);
}

TEST(SceneObjectReplacementTest, AsyncLoadReservationCannotReplaceNewerObject)
{
    EditorSceneManager manager;
    pnanovdb_editor_token_t scene{ 201u, "scene" };
    pnanovdb_editor_token_t name{ 202u, "target" };
    pnanovdb_compute_array_t late{};
    pnanovdb_compute_array_t current{};
    uint64_t stale_lifetime = 0;
    uint64_t current_lifetime = 0;

    ASSERT_TRUE(manager.reserve_load_target(&scene, &name, &stale_lifetime));
    manager.cancel_load_target(&scene, &name, stale_lifetime);
    ASSERT_TRUE(manager.reserve_load_target(&scene, &name, &current_lifetime));

    EXPECT_FALSE(manager.commit_reserved_nanovdb(&scene, &name, stale_lifetime, &late, nullptr, nullptr, nullptr,
                                                 pnanovdb_pipeline_type_noop, pnanovdb_pipeline_type_nanovdb_render));
    ASSERT_TRUE(manager.commit_reserved_nanovdb(&scene, &name, current_lifetime, &current, nullptr, nullptr, nullptr,
                                                pnanovdb_pipeline_type_noop, pnanovdb_pipeline_type_nanovdb_render));
    manager.with_object(&scene, &name,
                        [&](SceneObject* obj)
                        {
                            ASSERT_NE(obj, nullptr);
                            EXPECT_EQ(obj->resources.nanovdb_array, &current);
                        });
}

TEST(SceneObjectReplacementTest, ExplicitReplacementReservationPreservesTargetUntilCommit)
{
    EditorSceneManager manager;
    pnanovdb_editor_token_t scene{ 221u, "scene" };
    pnanovdb_editor_token_t name{ 222u, "target" };
    pnanovdb_compute_array_t original{};
    pnanovdb_compute_array_t replacement{};
    ASSERT_TRUE(manager.add_nanovdb(&scene, &name, &original, nullptr, nullptr));

    uint64_t lifetime = 0;
    EXPECT_FALSE(manager.reserve_load_target(&scene, &name, &lifetime));
    bool replacing = false;
    ASSERT_TRUE(manager.reserve_load_target(&scene, &name, &lifetime, true, &replacing));
    EXPECT_TRUE(replacing);
    manager.with_object(&scene, &name,
                        [&](SceneObject* obj)
                        {
                            ASSERT_NE(obj, nullptr);
                            EXPECT_EQ(obj->resources.nanovdb_array, &original);
                        });

    ASSERT_TRUE(manager.commit_reserved_nanovdb(&scene, &name, lifetime, &replacement, nullptr, nullptr, nullptr,
                                                pnanovdb_pipeline_type_noop, pnanovdb_pipeline_type_nanovdb_render));
    manager.with_object(&scene, &name,
                        [&](SceneObject* obj)
                        {
                            ASSERT_NE(obj, nullptr);
                            EXPECT_EQ(obj->resources.nanovdb_array, &replacement);
                        });
}

TEST(SceneObjectReplacementTest, StaleLifetimeCleanupCannotRemoveReplacement)
{
    EditorSceneManager manager;
    pnanovdb_editor_token_t scene{ 211u, "scene" };
    pnanovdb_editor_token_t name{ 212u, "target" };
    uint64_t stale_lifetime = 0;
    uint64_t replacement_lifetime = 0;

    ASSERT_TRUE(manager.reserve_load_target(&scene, &name, &stale_lifetime));
    manager.cancel_load_target(&scene, &name, stale_lifetime);
    manager.with_object_or_create(&scene, &name,
                                  [&](SceneObject* obj)
                                  {
                                      ASSERT_NE(obj, nullptr);
                                      replacement_lifetime = obj->lifetime_id;
                                      obj->type = SceneObjectType::NanoVDB;
                                  });

    ASSERT_NE(stale_lifetime, replacement_lifetime);
    EXPECT_FALSE(manager.remove_if_lifetime(&scene, &name, stale_lifetime));
    manager.with_object(&scene, &name,
                        [&](SceneObject* obj)
                        {
                            ASSERT_NE(obj, nullptr);
                            EXPECT_EQ(obj->lifetime_id, replacement_lifetime);
                        });
    EXPECT_TRUE(manager.remove_if_lifetime(&scene, &name, replacement_lifetime));
}

TEST(SceneObjectReplacementTest, DeferredRemovalCannotRemoveSameKeyReplacement)
{
    EditorSceneManager manager;
    pnanovdb_editor_token_t scene{ 213u, "scene" };
    pnanovdb_editor_token_t name{ 214u, "target" };
    pnanovdb_compute_array_t original{};
    pnanovdb_compute_array_t replacement{};

    ASSERT_TRUE(manager.add_nanovdb(&scene, &name, &original, nullptr, nullptr));
    const uint64_t queued_lifetime = manager.object_lifetime(&scene, &name);
    ASSERT_NE(queued_lifetime, 0u);

    ASSERT_TRUE(manager.add_nanovdb(&scene, &name, &replacement, nullptr, nullptr));
    const uint64_t replacement_lifetime = manager.object_lifetime(&scene, &name);
    ASSERT_NE(replacement_lifetime, queued_lifetime);

    EXPECT_FALSE(manager.remove_if_lifetime(&scene, &name, queued_lifetime));
    manager.with_object(&scene, &name,
                        [&](SceneObject* obj)
                        {
                            ASSERT_NE(obj, nullptr);
                            EXPECT_EQ(obj->lifetime_id, replacement_lifetime);
                            EXPECT_EQ(obj->resources.nanovdb_array, &replacement);
                        });
}

TEST(SceneObjectReplacementTest, RestoreStateCannotApplyToSameKeyReplacement)
{
    EditorSceneManager manager;
    pnanovdb_editor_token_t scene{ 215u, "scene" };
    pnanovdb_editor_token_t name{ 216u, "target" };
    pnanovdb_compute_array_t imported{};
    pnanovdb_compute_array_t replacement{};
    uint64_t reservation_lifetime = 0;

    ASSERT_TRUE(manager.reserve_load_target(&scene, &name, &reservation_lifetime));
    ASSERT_TRUE(manager.commit_reserved_nanovdb(&scene, &name, reservation_lifetime, &imported, nullptr, nullptr, nullptr,
                                                pnanovdb_pipeline_type_noop, pnanovdb_pipeline_type_nanovdb_render));
    const uint64_t published_lifetime = manager.object_lifetime(&scene, &name);
    ASSERT_NE(published_lifetime, 0u);
    ASSERT_NE(published_lifetime, reservation_lifetime);

    ASSERT_TRUE(manager.add_nanovdb(&scene, &name, &replacement, nullptr, nullptr));
    ASSERT_NE(manager.object_lifetime(&scene, &name), published_lifetime);

    const bool imported_generation_exists = manager.object_lifetime(&scene, &name) == published_lifetime;
    if (imported_generation_exists)
    {
        manager.with_object(&scene, &name,
                            [](SceneObject* obj)
                            {
                                ASSERT_NE(obj, nullptr);
                                obj->visible = false;
                            });
    }

    EXPECT_FALSE(imported_generation_exists);
    manager.with_object(&scene, &name,
                        [&](SceneObject* obj)
                        {
                            ASSERT_NE(obj, nullptr);
                            EXPECT_EQ(obj->resources.nanovdb_array, &replacement);
                            EXPECT_TRUE(obj->visible);
                        });
}

TEST(SceneObjectReplacementTest, AsyncLoadReservationFollowsSceneRename)
{
    EditorSceneManager manager;
    pnanovdb_editor_token_t old_scene{ 301u, "old" };
    pnanovdb_editor_token_t new_scene{ 302u, "new" };
    pnanovdb_editor_token_t name{ 303u, "target" };
    pnanovdb_compute_array_t result{};
    uint64_t lifetime = 0;

    ASSERT_TRUE(manager.reserve_load_target(&old_scene, &name, &lifetime));
    ASSERT_TRUE(manager.rename_scene(&old_scene, &new_scene));
    ASSERT_TRUE(manager.commit_reserved_nanovdb(&new_scene, &name, lifetime, &result, nullptr, nullptr, nullptr,
                                                pnanovdb_pipeline_type_noop, pnanovdb_pipeline_type_nanovdb_render));

    manager.with_object(&old_scene, &name, [](SceneObject* obj) { EXPECT_EQ(obj, nullptr); });
    manager.with_object(&new_scene, &name,
                        [&](SceneObject* obj)
                        {
                            ASSERT_NE(obj, nullptr);
                            EXPECT_EQ(obj->resources.nanovdb_array, &result);
                        });
}

TEST(SceneObjectReplacementTest, LifetimeLookupResolvesTargetAcrossRename)
{
    EditorSceneManager manager;
    pnanovdb_editor_token_t old_scene{ 311u, "old" };
    pnanovdb_editor_token_t new_scene{ 312u, "new" };
    pnanovdb_editor_token_t name{ 313u, "name" };
    uint64_t lifetime = 0;

    manager.with_object_or_create(&old_scene, &name,
                                  [&](SceneObject* obj)
                                  {
                                      ASSERT_NE(obj, nullptr);
                                      lifetime = obj->lifetime_id;
                                  });

    ASSERT_TRUE(manager.rename_scene(&old_scene, &new_scene));

    bool resolved = false;
    manager.with_object_lifetime(&old_scene, &name, lifetime,
                                 [&](SceneObject* obj)
                                 {
                                     ASSERT_NE(obj, nullptr);
                                     resolved = true;
                                     EXPECT_EQ(obj->scene_token, &new_scene);
                                     EXPECT_EQ(obj->name_token, &name);
                                 });
    EXPECT_TRUE(resolved);
}

TEST(SceneObjectReplacementTest, AsyncLoadReservationRejectsCameraName)
{
    EditorSceneManager manager;
    pnanovdb_editor_token_t scene{ 401u, "scene" };
    pnanovdb_editor_token_t name{ 402u, "camera" };
    manager.with_object_or_create(&scene, &name,
                                  [](SceneObject* obj)
                                  {
                                      ASSERT_NE(obj, nullptr);
                                      obj->type = SceneObjectType::Camera;
                                  });

    uint64_t lifetime = 0;
    EXPECT_FALSE(manager.reserve_load_target(&scene, &name, &lifetime));
}

TEST(SceneObjectReplacementTest, ReplacementReservationPreservesOldObjectUntilPublish)
{
    EditorSceneManager manager;
    pnanovdb_editor_token_t scene{ 501u, "scene" };
    pnanovdb_editor_token_t name{ 502u, "target" };
    pnanovdb_compute_array_t old_array{};
    pnanovdb_compute_array_t replacement_array{};

    uint64_t initial_lifetime = 0;
    ASSERT_TRUE(manager.reserve_load_target(&scene, &name, &initial_lifetime));
    ASSERT_TRUE(manager.commit_reserved_nanovdb(&scene, &name, initial_lifetime, &old_array, nullptr, nullptr, nullptr,
                                                pnanovdb_pipeline_type_noop, pnanovdb_pipeline_type_nanovdb_render));
    uint64_t lifetime = 0;
    bool replacing = false;
    ASSERT_TRUE(manager.reserve_load_target(&scene, &name, &lifetime, true, &replacing));
    EXPECT_TRUE(replacing);

    manager.with_object(&scene, &name,
                        [&](SceneObject* obj)
                        {
                            ASSERT_NE(obj, nullptr);
                            EXPECT_EQ(obj->resources.nanovdb_array, &old_array);
                            EXPECT_EQ(obj->lifetime_id, lifetime);
                        });

    // A failed replacement only releases its reservation; it must not remove the old object.
    manager.cancel_load_target(&scene, &name, lifetime);
    manager.with_object(&scene, &name,
                        [&](SceneObject* obj)
                        {
                            ASSERT_NE(obj, nullptr);
                            EXPECT_EQ(obj->resources.nanovdb_array, &old_array);
                        });

    ASSERT_TRUE(manager.commit_reserved_nanovdb(&scene, &name, lifetime, &replacement_array, nullptr, nullptr, nullptr,
                                                pnanovdb_pipeline_type_noop, pnanovdb_pipeline_type_nanovdb_render));
    manager.with_object(&scene, &name,
                        [&](SceneObject* obj)
                        {
                            ASSERT_NE(obj, nullptr);
                            EXPECT_EQ(obj->resources.nanovdb_array, &replacement_array);
                            EXPECT_NE(obj->lifetime_id, lifetime);
                        });
}

TEST(SceneObjectReplacementTest, ReplacementReservationFollowsObjectRename)
{
    EditorSceneManager manager;
    pnanovdb_editor_token_t scene{ 511u, "scene" };
    pnanovdb_editor_token_t old_name{ 512u, "old" };
    pnanovdb_editor_token_t new_name{ 513u, "new" };
    pnanovdb_compute_array_t old_array{};
    pnanovdb_compute_array_t replacement_array{};

    uint64_t initial_lifetime = 0;
    ASSERT_TRUE(manager.reserve_load_target(&scene, &old_name, &initial_lifetime));
    ASSERT_TRUE(manager.commit_reserved_nanovdb(&scene, &old_name, initial_lifetime, &old_array, nullptr, nullptr, nullptr,
                                                pnanovdb_pipeline_type_noop, pnanovdb_pipeline_type_nanovdb_render));
    uint64_t lifetime = 0;
    ASSERT_TRUE(manager.reserve_load_target(&scene, &old_name, &lifetime, true));
    ASSERT_TRUE(manager.rename_object(&scene, &old_name, &new_name));
    ASSERT_TRUE(manager.commit_reserved_nanovdb(&scene, &new_name, lifetime, &replacement_array, nullptr, nullptr, nullptr,
                                                pnanovdb_pipeline_type_noop, pnanovdb_pipeline_type_nanovdb_render));

    manager.with_object(&scene, &old_name, [](SceneObject* obj) { EXPECT_EQ(obj, nullptr); });
    manager.with_object(&scene, &new_name,
                        [&](SceneObject* obj)
                        {
                            ASSERT_NE(obj, nullptr);
                            EXPECT_EQ(obj->resources.nanovdb_array, &replacement_array);
                        });
}

TEST(SceneObjectReplacementTest, FilePlaceholderRejectsOrdinaryCollisionButAllowsExplicitReplacement)
{
    EditorSceneManager manager;
    pnanovdb_editor_token_t scene{ 521u, "scene" };
    pnanovdb_editor_token_t name{ 522u, "target" };
    pnanovdb_compute_array_t original{};
    pnanovdb_compute_t compute{};

    ASSERT_TRUE(manager.add_nanovdb(&scene, &name, &original, nullptr, nullptr));
    EXPECT_FALSE(manager.add_file_object(
        &scene, &name, &compute, pnanovdb_pipeline_type_voxelbvh_build, pnanovdb_pipeline_type_noop));
    manager.with_object(&scene, &name,
                        [&](SceneObject* obj)
                        {
                            ASSERT_NE(obj, nullptr);
                            EXPECT_EQ(obj->type, SceneObjectType::NanoVDB);
                            EXPECT_EQ(obj->resources.nanovdb_array, &original);
                        });

    EXPECT_TRUE(manager.add_file_object(
        &scene, &name, &compute, pnanovdb_pipeline_type_voxelbvh_build, pnanovdb_pipeline_type_noop, true));
    manager.with_object(&scene, &name,
                        [&](SceneObject* obj)
                        {
                            ASSERT_NE(obj, nullptr);
                            EXPECT_EQ(obj->type, SceneObjectType::Array);
                            EXPECT_EQ(obj->resources.nanovdb_array, nullptr);
                        });
}

TEST(SceneObjectReplacementTest, FailedStagedGaussianFileReplacementRestoresOldObject)
{
    EditorSceneManager manager;
    pnanovdb_editor_token_t scene{ 523u, "scene" };
    pnanovdb_editor_token_t name{ 524u, "target" };
    pnanovdb_editor_token_t renamed{ 527u, "renamed target" };
    pnanovdb_compute_array_t original{};
    pnanovdb_compute_t compute{};

    ASSERT_TRUE(manager.add_nanovdb(&scene, &name, &original, nullptr, nullptr));
    uint64_t old_lifetime = 0;
    manager.with_object(&scene, &name,
                        [&](SceneObject* obj)
                        {
                            ASSERT_NE(obj, nullptr);
                            old_lifetime = obj->lifetime_id;
                            obj->resources.source_filepath = "old.nvdb";
                        });

    uint64_t reserved_lifetime = 0;
    bool replacing = false;
    ASSERT_TRUE(manager.reserve_load_target(&scene, &name, &reserved_lifetime, true, &replacing));
    ASSERT_TRUE(replacing);
    ASSERT_EQ(reserved_lifetime, old_lifetime);
    ASSERT_TRUE(manager.stage_file_object_replacement(
        &scene, &name, reserved_lifetime, &compute, pnanovdb_pipeline_type_voxelbvh_build, pnanovdb_pipeline_type_noop));
    EXPECT_TRUE(manager.has_file_object_replacement_in_progress());
    ASSERT_TRUE(manager.rename_object(&scene, &name, &renamed));

    // A missing/corrupt file completes as failure. The staged candidate is
    // discarded and the exact old generation becomes current again.
    ASSERT_TRUE(manager.finish_file_object_replacement(reserved_lifetime, false));
    EXPECT_FALSE(manager.has_file_object_replacement_in_progress());
    manager.with_object(&scene, &name, [](SceneObject* obj) { EXPECT_EQ(obj, nullptr); });
    manager.with_object(&scene, &renamed,
                        [&](SceneObject* obj)
                        {
                            ASSERT_NE(obj, nullptr);
                            EXPECT_EQ(obj->type, SceneObjectType::NanoVDB);
                            EXPECT_EQ(obj->resources.nanovdb_array, &original);
                            EXPECT_EQ(obj->resources.source_filepath, "old.nvdb");
                            EXPECT_EQ(obj->lifetime_id, old_lifetime);
                        });
}

TEST(SceneObjectReplacementTest, SuccessfulStagedGaussianFileReplacementCommitsResult)
{
    EditorSceneManager manager;
    pnanovdb_editor_token_t scene{ 525u, "scene" };
    pnanovdb_editor_token_t name{ 526u, "target" };
    pnanovdb_compute_array_t original{};
    pnanovdb_compute_array_t result{};
    pnanovdb_compute_t compute{};

    ASSERT_TRUE(manager.add_nanovdb(&scene, &name, &original, nullptr, nullptr));
    manager.with_object(&scene, &name,
                        [](SceneObject* obj)
                        {
                            ASSERT_NE(obj, nullptr);
                            obj->visible = false;
                        });
    uint64_t reserved_lifetime = 0;
    ASSERT_TRUE(manager.reserve_load_target(&scene, &name, &reserved_lifetime, true));
    ASSERT_TRUE(manager.stage_file_object_replacement(
        &scene, &name, reserved_lifetime, &compute, pnanovdb_pipeline_type_voxelbvh_build, pnanovdb_pipeline_type_noop));
    manager.with_object(&scene, &name,
                        [&](SceneObject* obj)
                        {
                            ASSERT_NE(obj, nullptr);
                            obj->resources.source_filepath = "replacement.ply";
                            obj->pipeline.process().output.set_array(k_stage_output_nanovdb, &result, {});
                            obj->resolve_resources();
                        });

    ASSERT_TRUE(manager.finish_file_object_replacement(reserved_lifetime, true));
    manager.with_object(&scene, &name,
                        [&](SceneObject* obj)
                        {
                            ASSERT_NE(obj, nullptr);
                            EXPECT_EQ(obj->type, SceneObjectType::Array);
                            EXPECT_EQ(obj->resources.converted_nanovdb, &result);
                            EXPECT_EQ(obj->resources.source_filepath, "replacement.ply");
                            EXPECT_NE(obj->lifetime_id, reserved_lifetime);
                            EXPECT_FALSE(obj->visible);
                        });
}

TEST(SceneObjectReplacementTest, RemovingStagedCandidateClearsRetainedBackup)
{
    EditorSceneManager manager;
    pnanovdb_editor_token_t scene{ 541u, "scene" };
    pnanovdb_editor_token_t name{ 542u, "target" };
    pnanovdb_compute_array_t original{};
    pnanovdb_compute_t compute{};

    ASSERT_TRUE(manager.add_nanovdb(&scene, &name, &original, nullptr, nullptr));
    uint64_t lifetime = 0;
    ASSERT_TRUE(manager.reserve_load_target(&scene, &name, &lifetime, true));
    ASSERT_TRUE(manager.stage_file_object_replacement(
        &scene, &name, lifetime, &compute, pnanovdb_pipeline_type_voxelbvh_build, pnanovdb_pipeline_type_noop));
    ASSERT_TRUE(manager.has_file_object_replacement_in_progress());

    EXPECT_TRUE(manager.remove(&scene, &name));
    EXPECT_FALSE(manager.has_file_object_replacement_in_progress());
    EXPECT_FALSE(manager.finish_file_object_replacement(lifetime, false));
}

TEST(SceneObjectReplacementTest, LifetimeRemovalOfStagedCandidateClearsRetainedBackup)
{
    EditorSceneManager manager;
    pnanovdb_editor_token_t scene{ 543u, "scene" };
    pnanovdb_editor_token_t name{ 544u, "target" };
    pnanovdb_compute_array_t original{};
    pnanovdb_compute_t compute{};

    ASSERT_TRUE(manager.add_nanovdb(&scene, &name, &original, nullptr, nullptr));
    uint64_t lifetime = 0;
    ASSERT_TRUE(manager.reserve_load_target(&scene, &name, &lifetime, true));
    ASSERT_TRUE(manager.stage_file_object_replacement(
        &scene, &name, lifetime, &compute, pnanovdb_pipeline_type_voxelbvh_build, pnanovdb_pipeline_type_noop));
    ASSERT_TRUE(manager.has_file_object_replacement_in_progress());

    EXPECT_TRUE(manager.remove_if_lifetime(&scene, &name, lifetime));
    EXPECT_FALSE(manager.has_file_object_replacement_in_progress());
    EXPECT_FALSE(manager.finish_file_object_replacement(lifetime, false));
}

TEST(SceneObjectReplacementTest, ArrayOwnerDeduplicationIncludesRetainedReplacementBackup)
{
    pnanovdb_editor_token_t scene{ 545u, "scene" };
    pnanovdb_editor_token_t original_name{ 546u, "original" };
    pnanovdb_editor_token_t alias_name{ 547u, "alias" };
    pnanovdb_compute_array_t shared_array{};
    pnanovdb_compute_t compute{};
    compute.destroy_array = count_retained_array_destroy;
    g_retained_array_destroy_count = 0;

    {
        EditorSceneManager manager;
        ASSERT_TRUE(manager.add_nanovdb(&scene, &original_name, &shared_array, nullptr, &compute));
        uint64_t lifetime = 0;
        ASSERT_TRUE(manager.reserve_load_target(&scene, &original_name, &lifetime, true));
        ASSERT_TRUE(manager.stage_file_object_replacement(&scene, &original_name, lifetime, &compute,
                                                          pnanovdb_pipeline_type_voxelbvh_build,
                                                          pnanovdb_pipeline_type_noop));

        ASSERT_TRUE(manager.add_nanovdb(&scene, &alias_name, &shared_array, nullptr, &compute));
        ASSERT_TRUE(manager.finish_file_object_replacement(lifetime, false));
        EXPECT_EQ(g_retained_array_destroy_count, 0);
    }

    EXPECT_EQ(g_retained_array_destroy_count, 1);
}

TEST(SceneObjectReplacementTest, GaussianOwnerDeduplicationIncludesRetainedReplacementBackup)
{
    pnanovdb_editor_token_t scene{ 548u, "scene" };
    pnanovdb_editor_token_t original_name{ 549u, "original" };
    pnanovdb_editor_token_t alias_name{ 550u, "alias" };
    pnanovdb_raster::gaussian_data_t shared_gaussian_storage{};
    pnanovdb_raster_gaussian_data_t* shared_gaussian = pnanovdb_raster::cast(&shared_gaussian_storage);
    pnanovdb_compute_t compute{};
    pnanovdb_raster_t raster{};
    raster.destroy_gaussian_data = count_retained_gaussian_destroy;
    auto* queue = reinterpret_cast<pnanovdb_compute_queue_t*>(uintptr_t{ 1 });
    g_retained_gaussian_destroy_count = 0;

    {
        EditorSceneManager manager;
        ASSERT_TRUE(manager.add_gaussian_data(
            &scene, &original_name, shared_gaussian, nullptr, nullptr, &compute, &raster, queue));
        uint64_t lifetime = 0;
        ASSERT_TRUE(manager.reserve_load_target(&scene, &original_name, &lifetime, true));
        ASSERT_TRUE(manager.stage_file_object_replacement(&scene, &original_name, lifetime, &compute,
                                                          pnanovdb_pipeline_type_voxelbvh_build,
                                                          pnanovdb_pipeline_type_noop));

        ASSERT_TRUE(manager.add_gaussian_data(
            &scene, &alias_name, shared_gaussian, nullptr, nullptr, &compute, &raster, queue));
        ASSERT_TRUE(manager.finish_file_object_replacement(lifetime, false));
        EXPECT_EQ(g_retained_gaussian_destroy_count, 0);
    }

    EXPECT_EQ(g_retained_gaussian_destroy_count, 1);
}

TEST(SceneObjectReplacementTest, CrossTypeReplacementHandsOffGaussianOwner)
{
    EditorSceneManager manager;
    pnanovdb_editor_token_t scene{ 531u, "scene" };
    pnanovdb_editor_token_t name{ 532u, "target" };
    pnanovdb_compute_array_t nanovdb{};
    pnanovdb_compute_t compute{};
    int destroyed = 0;
    int expected_destroyed = 0;
    uintptr_t gaussian_address = 1u;

    auto install_gaussian = [&]()
    {
        auto* gaussian = reinterpret_cast<pnanovdb_raster_gaussian_data_t*>(gaussian_address++);
        manager.with_object_or_create(
            &scene, &name,
            [&](SceneObject* obj)
            {
                ASSERT_NE(obj, nullptr);
                obj->reset_source();
                obj->type = SceneObjectType::GaussianData;
                obj->resources.gaussian_data = gaussian;
                obj->resources.gaussian_data_owner = std::shared_ptr<pnanovdb_raster_gaussian_data_t>(
                    gaussian, [&](pnanovdb_raster_gaussian_data_t*) { ++destroyed; });
                obj->pipeline.load().output.gaussian = gaussian;
                obj->pipeline.load().output.gaussian_owner = obj->resources.gaussian_data_owner;
            });
    };

    auto expect_deferred = [&](std::shared_ptr<pnanovdb_raster_gaussian_data_t>& owner)
    {
        ASSERT_TRUE(owner);
        EXPECT_EQ(destroyed, expected_destroyed);
        owner.reset();
        EXPECT_EQ(destroyed, ++expected_destroyed);
    };

    install_gaussian();
    std::shared_ptr<pnanovdb_raster_gaussian_data_t> old_owner;
    ASSERT_TRUE(manager.add_nanovdb(&scene, &name, &nanovdb, nullptr, nullptr, nullptr, &old_owner));
    expect_deferred(old_owner);

    install_gaussian();
    ASSERT_TRUE(manager.add_file_object(
        &scene, &name, &compute, pnanovdb_pipeline_type_voxelbvh_build, pnanovdb_pipeline_type_noop, true, &old_owner));
    expect_deferred(old_owner);
}

} // namespace
} // namespace pnanovdb_editor
