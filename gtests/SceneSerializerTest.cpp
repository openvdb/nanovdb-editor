// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

#include "editor/EditorSceneManager.h"
#include "editor/Editor.h"
#include "editor/EditorToken.h"
#include "editor/SceneSerializer.h"
#include "editor/SceneView.h"
#include "editor/ParamWidget.h"
#include "editor/PipelineRegistry.h"
#include "editor/PipelineRuntime.h"

#include "nanovdb_editor/putil/Shader.hpp"
#include "nanovdb_editor/putil/Compiler.h"
#include "nanovdb_editor/putil/Compute.h"
#include "nanovdb_editor/putil/Editor.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <vector>

namespace pnanovdb_editor
{
namespace
{

int g_rejected_array_destroys = 0;
int g_rejected_gaussian_destroys = 0;

struct ScopedFileRemove
{
    std::filesystem::path path;
    ~ScopedFileRemove()
    {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
};

void destroy_rejected_array(pnanovdb_compute_array_t*)
{
    ++g_rejected_array_destroys;
}

void destroy_rejected_gaussian(const pnanovdb_compute_t*, pnanovdb_compute_queue_t*, pnanovdb_raster_gaussian_data_t*)
{
    ++g_rejected_gaussian_destroys;
}

void destroy_test_array(pnanovdb_compute_array_t*)
{
}

const pnanovdb_pipeline_descriptor_t k_test_noop_descriptor = { pnanovdb_pipeline_type_noop, pnanovdb_pipeline_stage_load,
                                                                "No Operation", "pnanovdb_pipeline_type_noop" };
const pnanovdb_pipeline_descriptor_t k_test_voxelbvh_build_descriptor = { pnanovdb_pipeline_type_voxelbvh_build,
                                                                          pnanovdb_pipeline_stage_process,
                                                                          "VoxelBVH Build",
                                                                          "pnanovdb_pipeline_type_voxelbvh_build" };
const pnanovdb_pipeline_shader_entry_t k_test_nanovdb_shader = { "editor/editor.slang", nullptr, PNANOVDB_TRUE };
const pnanovdb_pipeline_shader_entry_t k_test_gaussian_shader = { "raster/gaussian_rasterize_2d.slang",
                                                                  "raster/raster2d_group", PNANOVDB_FALSE };
const pnanovdb_pipeline_descriptor_t k_test_nanovdb_render_descriptor = {
    pnanovdb_pipeline_type_nanovdb_render,   pnanovdb_pipeline_stage_render, "NanoVDB Render",
    "pnanovdb_pipeline_type_nanovdb_render", &k_test_nanovdb_shader,         1u,
};
const pnanovdb_pipeline_descriptor_t k_test_gaussian_render_descriptor = {
    pnanovdb_pipeline_type_gaussian_splat,   pnanovdb_pipeline_stage_render, "Gaussian 2D Splatting",
    "pnanovdb_pipeline_type_gaussian_splat", &k_test_gaussian_shader,        1u,
};

void register_test_pipeline_descriptors()
{
    pnanovdb_pipeline_register(&k_test_noop_descriptor);
    pnanovdb_pipeline_register(&k_test_voxelbvh_build_descriptor);
}

void register_test_editor_startup_pipeline_descriptors()
{
    pnanovdb_pipeline_register(&k_test_nanovdb_render_descriptor);
    pnanovdb_pipeline_register(&k_test_gaussian_render_descriptor);
}

TEST(SceneSerializer, PreservesEmptyScenesAndCameras)
{
    EditorSceneManager manager;
    SceneView views;
    pnanovdb_editor_token_t* scene = EditorToken::getInstance().getToken("empty scene");
    ASSERT_NE(views.get_or_create_scene(scene), nullptr);

    pnanovdb_editor_token_t* camera = views.add_new_camera(scene, "User Camera");
    ASSERT_NE(camera, nullptr);
    pnanovdb_camera_view_t* camera_view = views.get_camera(scene, camera);
    ASSERT_NE(camera_view, nullptr);
    camera_view->axis_length = 42.f;
    camera_view->states[0].position = { 1.f, 2.f, 3.f };
    views.set_viewport_camera(scene, camera);

    const nlohmann::ordered_json doc = serialize_scenes(manager, views);
    EXPECT_EQ(doc.at("version"), k_scene_file_version);
    ASSERT_EQ(doc.at("objects").size(), 0);
    ASSERT_EQ(doc.at("scenes").size(), 1);
    const auto& saved_scene = doc.at("scenes").at(0);
    EXPECT_EQ(saved_scene.at("name"), "empty scene");
    ASSERT_EQ(saved_scene.at("cameras").size(), 2);

    const auto user_camera = std::find_if(saved_scene.at("cameras").begin(), saved_scene.at("cameras").end(),
                                          [](const nlohmann::json& value) { return value.at("name") == "User Camera"; });
    ASSERT_NE(user_camera, saved_scene.at("cameras").end());
    EXPECT_TRUE(user_camera->at("viewport").get<bool>());
    EXPECT_FLOAT_EQ(user_camera->at("axis_length").get<float>(), 42.f);
    EXPECT_FLOAT_EQ(user_camera->at("entries").at(0).at("state").at("position").at(2).get<float>(), 3.f);
}

TEST(SceneSerializer, FreshRestoreSceneDoesNotCreateCameraThatCollidesWithSerializedDefault)
{
    pnanovdb_editor_token_t* scene = EditorToken::getInstance().getToken("fresh camera restore scene");

    SceneView fresh;
    SceneViewData* fresh_data = fresh.get_or_create_scene(scene, false);
    ASSERT_NE(fresh_data, nullptr);
    EXPECT_TRUE(fresh_data->cameras.empty());

    // Existing scenes retain their genuine camera even when queried through
    // the restore path; only creation of a fresh restore scene suppresses the
    // synthetic default.
    SceneView existing;
    SceneViewData* existing_data = existing.get_or_create_scene(scene);
    ASSERT_NE(existing_data, nullptr);
    ASSERT_EQ(existing_data->cameras.size(), 1u);
    const auto original_camera = existing_data->cameras.begin()->second.camera_view;
    EXPECT_EQ(existing.get_or_create_scene(scene, false), existing_data);
    ASSERT_EQ(existing_data->cameras.size(), 1u);
    EXPECT_EQ(existing_data->cameras.begin()->second.camera_view, original_camera);
}

TEST(SceneSerializer, RestoredSceneWithoutCamerasCreatesRegisteredViewport)
{
    EditorSceneManager manager;
    SceneView views;
    pnanovdb_editor_token_t* scene = EditorToken::getInstance().getToken("restore missing cameras scene");
    SceneViewData* scene_data = views.get_or_create_scene(scene, false);
    ASSERT_NE(scene_data, nullptr);
    ASSERT_TRUE(scene_data->cameras.empty());

    ASSERT_TRUE(normalize_scene_viewport_camera(manager, views, scene));

    pnanovdb_editor_token_t* viewport = views.get_viewport_camera_token(scene);
    ASSERT_NE(viewport, nullptr);
    ASSERT_NE(views.get_camera(scene, viewport), nullptr);
    bool registered = false;
    manager.with_object(
        scene, viewport, [&](SceneObject* obj) { registered = obj && obj->type == SceneObjectType::Camera; });
    EXPECT_TRUE(registered);
}

TEST(SceneSerializer, RestoredSceneWithInvalidCameraCreatesCollisionFreeViewport)
{
    EditorSceneManager manager;
    SceneView views;
    pnanovdb_editor_token_t* scene = EditorToken::getInstance().getToken("restore invalid camera scene");
    SceneViewData* scene_data = views.get_or_create_scene(scene, false);
    ASSERT_NE(scene_data, nullptr);
    pnanovdb_editor_token_t* invalid = EditorToken::getInstance().getToken("Broken Camera");
    scene_data->cameras[invalid->id] = CameraViewContext{};
    views.set_viewport_camera(scene, invalid);

    pnanovdb_editor_token_t* occupied_default = EditorToken::getInstance().getToken("Camera");
    manager.with_object_or_create(scene, occupied_default,
                                  [](SceneObject* obj)
                                  {
                                      ASSERT_NE(obj, nullptr);
                                      obj->type = SceneObjectType::Array;
                                  });

    ASSERT_TRUE(normalize_scene_viewport_camera(manager, views, scene));

    pnanovdb_editor_token_t* viewport = views.get_viewport_camera_token(scene);
    ASSERT_NE(viewport, nullptr);
    EXPECT_STREQ(viewport->str, "Camera 1");
    ASSERT_NE(views.get_camera(scene, viewport), nullptr);
    EXPECT_EQ(scene_data->cameras.count(invalid->id), 1u);
}

TEST(SceneSerializer, ViewportCameraNameRecoveryHasBoundedFailure)
{
    EditorSceneManager manager;
    SceneView views;
    pnanovdb_editor_token_t* scene = EditorToken::getInstance().getToken("exhausted viewport camera names scene");
    SceneViewData* scene_data = views.get_or_create_scene(scene, false);
    ASSERT_NE(scene_data, nullptr);
    ASSERT_TRUE(scene_data->cameras.empty());

    constexpr unsigned int k_candidate_count = 256u;
    for (unsigned int suffix = 0; suffix < k_candidate_count; ++suffix)
    {
        const std::string name = suffix == 0 ? "Camera" : "Camera " + std::to_string(suffix);
        pnanovdb_editor_token_t* name_token = EditorToken::getInstance().getToken(name.c_str());
        manager.with_object_or_create(scene, name_token,
                                      [](SceneObject* object)
                                      {
                                          ASSERT_NE(object, nullptr);
                                          object->type = SceneObjectType::Array;
                                      });
    }

    EXPECT_FALSE(normalize_scene_viewport_camera(manager, views, scene));
    EXPECT_EQ(views.get_viewport_camera_token(scene), nullptr);
    EXPECT_TRUE(scene_data->cameras.empty());
}

TEST(SceneSerializer, RestoredSceneWithoutViewportMarkerPromotesDeterministicCamera)
{
    EditorSceneManager manager;
    SceneView views;
    pnanovdb_editor_token_t* scene = EditorToken::getInstance().getToken("restore unmarked camera scene");
    SceneViewData* scene_data = views.get_or_create_scene(scene, false);
    ASSERT_NE(scene_data, nullptr);
    pnanovdb_editor_token_t* first = views.add_new_camera(scene, "First restored camera");
    pnanovdb_editor_token_t* second = views.add_new_camera(scene, "Second restored camera");
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    ASSERT_EQ(scene_data->viewport_camera_token_id, 0u);
    const uint64_t expected_id = scene_data->cameras.begin()->first;

    ASSERT_TRUE(normalize_scene_viewport_camera(manager, views, scene));

    pnanovdb_editor_token_t* viewport = views.get_viewport_camera_token(scene);
    ASSERT_NE(viewport, nullptr);
    EXPECT_EQ(viewport->id, expected_id);
    EXPECT_EQ(scene_data->cameras.size(), 2u);
}

TEST(SceneSerializer, StartupCameraIsRegisteredBeforeRenderInsertions)
{
    EditorSceneManager manager;
    SceneView views;
    views.initialize_for_startup(false);

    pnanovdb_editor_token_t* scene = EditorToken::getInstance().getToken(DEFAULT_SCENE_NAME);
    ASSERT_TRUE(ensure_scene_with_registered_cameras(manager, views, scene));
    pnanovdb_editor_token_t* camera = EditorToken::getInstance().getToken("Camera");
    bool registered_camera = false;
    manager.with_object(
        scene, camera, [&](SceneObject* obj) { registered_camera = obj && obj->type == SceneObjectType::Camera; });
    ASSERT_TRUE(registered_camera);

    pnanovdb_compute_array_t conflicting_renderable{};
    EXPECT_FALSE(manager.add_nanovdb(scene, camera, &conflicting_renderable, nullptr, nullptr));

    const SceneViewData* scene_data = views.get_scene_data(scene);
    ASSERT_NE(scene_data, nullptr);
    EXPECT_EQ(scene_data->cameras.count(camera->id), 1u);
    EXPECT_EQ(scene_data->nanovdbs.count(camera->id), 0u);
    EXPECT_EQ(scene_data->gaussians.count(camera->id), 0u);
    EXPECT_EQ(std::count(scene_data->renderable_order.begin(), scene_data->renderable_order.end(), camera->id), 0);

    const std::filesystem::path output =
        std::filesystem::temp_directory_path() / "pnanovdb_scene_serializer_startup_camera.json";
    std::error_code ec;
    std::filesystem::remove(output, ec);
    std::string error;
    EXPECT_TRUE(save_scenes_to_file(manager, views, output.string(), &error)) << error;
    EXPECT_TRUE(std::filesystem::is_regular_file(output));

    EXPECT_TRUE(manager.remove(scene, camera));
    EXPECT_TRUE(views.remove(scene, camera));
    bool camera_still_registered = false;
    manager.with_object(scene, camera, [&](SceneObject* object) { camera_still_registered = object != nullptr; });
    EXPECT_FALSE(camera_still_registered);
    EXPECT_EQ(scene_data->cameras.count(camera->id), 0u);
    EXPECT_EQ(scene_data->nanovdbs.count(camera->id), 0u);
    EXPECT_EQ(scene_data->gaussians.count(camera->id), 0u);
    EXPECT_EQ(std::count(scene_data->renderable_order.begin(), scene_data->renderable_order.end(), camera->id), 0);
    std::filesystem::remove(output, ec);
}

TEST(SceneSerializer, EmptyPublicCameraNormalizesForRoundTripAndViewportSelection)
{
    EditorSceneManager manager;
    SceneView views;
    pnanovdb_editor_token_t* scene = EditorToken::getInstance().getToken("empty public camera scene");
    pnanovdb_editor_token_t* name = EditorToken::getInstance().getToken("Empty Camera");

    pnanovdb_camera_view_t empty_camera{};
    pnanovdb_camera_view_default(&empty_camera);
    empty_camera.name = name;
    ASSERT_TRUE(manager.add_camera(scene, name, &empty_camera));

    std::shared_ptr<pnanovdb_camera_view_t> owner;
    manager.with_object(scene, name,
                        [&](SceneObject* obj)
                        {
                            ASSERT_NE(obj, nullptr);
                            owner = obj->resources.camera_view_owner;
                        });
    ASSERT_TRUE(owner);
    ASSERT_EQ(owner->num_cameras, 1u);
    ASSERT_NE(owner->states, nullptr);
    ASSERT_NE(owner->configs, nullptr);

    views.sync_camera_owner(scene, name, owner);
    views.set_viewport_camera(scene, name);
    EXPECT_EQ(views.get_viewport_camera_token(scene), name);

    const nlohmann::ordered_json doc = serialize_scenes(manager, views);
    const auto& cameras = doc.at("scenes").at(0).at("cameras");
    const auto restored = std::find_if(
        cameras.begin(), cameras.end(), [](const nlohmann::json& value) { return value.at("name") == "Empty Camera"; });
    ASSERT_NE(restored, cameras.end());
    EXPECT_TRUE(restored->at("viewport").get<bool>());
    EXPECT_EQ(restored->at("entries").size(), 1u);
}

TEST(SceneSerializer, EmptyObjectTypeAndConfiguredPipelineRoundTripWithoutSource)
{
    register_test_pipeline_descriptors();
    pnanovdb_editor_token_t* scene = EditorToken::getInstance().getToken("empty object roundtrip scene");
    pnanovdb_editor_token_t* name = EditorToken::getInstance().getToken("empty configured object");

    EditorSceneManager source_manager;
    SceneView source_views;
    source_views.get_or_create_scene(scene);
    ASSERT_TRUE(restore_empty_scene_object(source_manager, source_views, scene, name, "array"));
    source_manager.with_object(scene, name,
                               [](SceneObject* obj)
                               {
                                   ASSERT_NE(obj, nullptr);
                                   obj->pipeline.process().type = pnanovdb_pipeline_type_voxelbvh_build;
                                   obj->pipeline.process().configured = true;
                               });

    const nlohmann::ordered_json doc = serialize_scenes(source_manager, source_views);
    ASSERT_EQ(doc.at("objects").size(), 1u);
    const nlohmann::json& saved = doc.at("objects").at(0);
    EXPECT_EQ(saved.at("type"), "array");
    EXPECT_TRUE(saved.at("source_filepath").get<std::string>().empty());

    EditorSceneManager restored_manager;
    SceneView restored_views;
    restored_views.get_or_create_scene(scene, false);
    ASSERT_TRUE(
        restore_empty_scene_object(restored_manager, restored_views, scene, name, saved.at("type").get<std::string>()));

    pnanovdb_pipeline_type_t process_type = pnanovdb_pipeline_type_noop;
    ASSERT_TRUE(pipeline_type_from_json(saved.at("pipeline").at("process").at(0), process_type));
    restored_manager.with_object(scene, name,
                                 [process_type](SceneObject* obj)
                                 {
                                     ASSERT_NE(obj, nullptr);
                                     EXPECT_EQ(obj->type, SceneObjectType::Array);
                                     obj->pipeline.process().type = process_type;
                                     EXPECT_EQ(obj->pipeline.process().type, pnanovdb_pipeline_type_voxelbvh_build);
                                 });
    EXPECT_NE(restored_views.get_nanovdb(scene, name), nullptr);
}

TEST(SceneSerializer, EmptyObjectRestoreRejectsUnsupportedType)
{
    EditorSceneManager manager;
    SceneView views;
    pnanovdb_editor_token_t* scene = EditorToken::getInstance().getToken("unsupported empty type scene");
    pnanovdb_editor_token_t* name = EditorToken::getInstance().getToken("unsupported empty type");
    views.get_or_create_scene(scene, false);

    EXPECT_FALSE(restore_empty_scene_object(manager, views, scene, name, "camera"));
    bool exists = false;
    manager.with_object(scene, name, [&](SceneObject* obj) { exists = obj != nullptr; });
    EXPECT_FALSE(exists);
}

TEST(PipelineParams, FloatIsBoolHintUsesCheckboxControl)
{
    ParamWidgetHints hints;
    hints.is_bool = true;
    EXPECT_TRUE(param_widget_uses_bool_control(ImGuiDataType_Float, &hints));
    EXPECT_FALSE(param_widget_uses_bool_control(ImGuiDataType_Float, nullptr));
}

TEST(SceneSerializer, RejectsUnbackedInMemoryDataBeforeWriting)
{
    EditorSceneManager manager;
    SceneView views;
    pnanovdb_editor_token_t* scene = EditorToken::getInstance().getToken("scene");
    pnanovdb_editor_token_t* name = EditorToken::getInstance().getToken("memory only");
    views.get_or_create_scene(scene);

    pnanovdb_compute_array_t array = {};
    manager.add_nanovdb(scene, name, &array, nullptr, nullptr);

    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "pnanovdb_scene_serializer_should_not_write.json";
    std::error_code ec;
    std::filesystem::remove(path, ec);
    std::string error;
    EXPECT_FALSE(save_scenes_to_file(manager, views, path.string(), &error));
    EXPECT_NE(error.find("memory only"), std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(path));
}

TEST(SceneSerializer, RejectsConfiguredObjectWhoseTypeIsUndefined)
{
    EditorSceneManager manager;
    SceneView views;
    pnanovdb_editor_token_t* scene = EditorToken::getInstance().getToken("configured placeholder scene");
    pnanovdb_editor_token_t* name = EditorToken::getInstance().getToken("configured placeholder");
    manager.with_object_or_create(scene, name,
                                  [](SceneObject* obj)
                                  {
                                      ASSERT_NE(obj, nullptr);
                                      obj->pipeline.process().configured = true;
                                      obj->pipeline.process().type = pnanovdb_pipeline_type_noop;
                                  });

    const std::filesystem::path output =
        std::filesystem::temp_directory_path() / "pnanovdb_scene_serializer_untyped_placeholder.json";
    std::error_code ec;
    std::filesystem::remove(output, ec);
    std::string error;
    EXPECT_FALSE(save_scenes_to_file(manager, views, output.string(), &error));
    EXPECT_NE(error.find("object type is not defined"), std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(output));
}

TEST(SceneSerializer, RejectsSaveWhileFileReplacementIsStaged)
{
    EditorSceneManager manager;
    SceneView views;
    pnanovdb_editor_token_t* scene = EditorToken::getInstance().getToken("staged replacement scene");
    pnanovdb_editor_token_t* name = EditorToken::getInstance().getToken("staged replacement object");
    views.get_or_create_scene(scene);

    pnanovdb_compute_array_t original{};
    pnanovdb_compute_t compute{};
    ASSERT_TRUE(manager.add_nanovdb(scene, name, &original, nullptr, nullptr));
    uint64_t lifetime_id = 0;
    ASSERT_TRUE(manager.reserve_load_target(scene, name, &lifetime_id, true));
    ASSERT_TRUE(manager.stage_file_object_replacement(
        scene, name, lifetime_id, &compute, pnanovdb_pipeline_type_voxelbvh_build, pnanovdb_pipeline_type_noop));

    const std::filesystem::path output =
        std::filesystem::temp_directory_path() / "pnanovdb_scene_serializer_staged_replacement.json";
    std::error_code ec;
    std::filesystem::remove(output, ec);
    std::string error;
    EXPECT_FALSE(save_scenes_to_file(manager, views, output.string(), &error));
    EXPECT_NE(error.find("replacement is still in progress"), std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(output));

    EXPECT_TRUE(manager.finish_file_object_replacement(lifetime_id, false));
}

TEST(SceneSerializer, InvalidObjectRecordsDoNotCreateGhostScenes)
{
    const std::filesystem::path missing =
        std::filesystem::temp_directory_path() / "pnanovdb_scene_serializer_missing_ghost_source.nvdb";
    std::error_code ec;
    std::filesystem::remove(missing, ec);

    SceneView views;
    const std::vector<nlohmann::json> invalid_records = {
        { { "scene", "missing source ghost" },
          { "name", "object" },
          { "type", "nanovdb" },
          { "source_filepath", missing.string() } },
        { { "name", "fallback pollution" }, { "type", "array" }, { "source_filepath", "" } },
        { { "scene", "unsupported ghost" }, { "name", "object" }, { "type", "unsupported" }, { "source_filepath", "" } },
    };

    for (const nlohmann::json& record : invalid_records)
    {
        std::string scene_name, object_name, object_type, source, error;
        if (parse_scene_object_record(record, scene_name, object_name, object_type, source, &error))
        {
            views.get_or_create_scene(EditorToken::getInstance().getToken(scene_name.c_str()));
        }
        EXPECT_FALSE(error.empty());
    }
    EXPECT_FALSE(views.has_scenes());
}

TEST(SceneSerializer, AcceptedEmptyObjectRecordMayCreateItsScene)
{
    const nlohmann::json record = {
        { "scene", "accepted empty scene" },
        { "name", "empty object" },
        { "type", "array" },
        { "source_filepath", "" },
    };
    std::string scene_name, object_name, object_type, source, error;
    ASSERT_TRUE(parse_scene_object_record(record, scene_name, object_name, object_type, source, &error)) << error;
    SceneView views;
    EXPECT_NE(views.get_or_create_scene(EditorToken::getInstance().getToken(scene_name.c_str())), nullptr);
    EXPECT_TRUE(views.has_scenes());
}

TEST(SceneSerializer, RejectsMissingFileBackedSourceBeforeWriting)
{
    EditorSceneManager manager;
    SceneView views;
    pnanovdb_editor_token_t* scene = EditorToken::getInstance().getToken("missing source scene");
    pnanovdb_editor_token_t* name = EditorToken::getInstance().getToken("missing source object");
    views.get_or_create_scene(scene);

    pnanovdb_compute_array_t array = {};
    manager.add_nanovdb(scene, name, &array, nullptr, nullptr);
    const std::filesystem::path missing =
        std::filesystem::temp_directory_path() / "pnanovdb_scene_serializer_missing_source.nvdb";
    std::error_code ec;
    std::filesystem::remove(missing, ec);
    manager.with_object(scene, name, [&](SceneObject* obj) { obj->resources.source_filepath = missing.string(); });

    const std::filesystem::path output =
        std::filesystem::temp_directory_path() / "pnanovdb_scene_serializer_missing_source.json";
    std::filesystem::remove(output, ec);
    std::string error;
    EXPECT_FALSE(save_scenes_to_file(manager, views, output.string(), &error));
    EXPECT_NE(error.find("missing source object"), std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(output));
}

TEST(SceneSerializer, ValidatesSourceOfFileBackedObjectBeforeItProducesData)
{
    EditorSceneManager manager;
    SceneView views;
    pnanovdb_editor_token_t* scene = EditorToken::getInstance().getToken("pending source scene");
    pnanovdb_editor_token_t* name = EditorToken::getInstance().getToken("pending source object");
    views.get_or_create_scene(scene);

    manager.with_object_or_create(scene, name,
                                  [&](SceneObject* obj)
                                  {
                                      obj->type = SceneObjectType::Array;
                                      obj->pipeline.load().type = pnanovdb_pipeline_type_gaussian_load;
                                  });

    const std::filesystem::path output =
        std::filesystem::temp_directory_path() / "pnanovdb_scene_serializer_pending_source.json";
    std::error_code ec;
    std::filesystem::remove(output, ec);
    std::string error;

    // A genuinely empty, source-less placeholder remains representable.
    EXPECT_TRUE(save_scenes_to_file(manager, views, output.string(), &error)) << error;
    EXPECT_TRUE(std::filesystem::exists(output));

    const std::filesystem::path missing =
        std::filesystem::temp_directory_path() / "pnanovdb_scene_serializer_pending_missing.ply";
    std::filesystem::remove(missing, ec);
    manager.with_object(scene, name, [&](SceneObject* obj) { obj->resources.source_filepath = missing.string(); });
    std::filesystem::remove(output, ec);
    error.clear();

    EXPECT_FALSE(save_scenes_to_file(manager, views, output.string(), &error));
    EXPECT_NE(error.find("pending source object"), std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(output));
}

TEST(SceneSerializer, SourcePathValidationDoesNotThrowForInvalidPath)
{
    EXPECT_FALSE(scene_source_path_is_available(std::string(10000, 'x')));
}

TEST(SceneSerializer, RejectsCameraAndRenderableWithSameName)
{
    EditorSceneManager manager;
    SceneView views;
    pnanovdb_editor_token_t* scene = EditorToken::getInstance().getToken("collision scene");
    ASSERT_NE(views.get_or_create_scene(scene), nullptr);
    pnanovdb_editor_token_t* camera_name = EditorToken::getInstance().getToken("Camera");

    const std::filesystem::path source =
        std::filesystem::temp_directory_path() / "pnanovdb_scene_serializer_collision_source.nvdb";
    {
        std::ofstream file(source);
        file << "test";
    }
    pnanovdb_compute_array_t array = {};
    manager.add_nanovdb(scene, camera_name, &array, nullptr, nullptr);
    manager.with_object(scene, camera_name, [&](SceneObject* obj) { obj->resources.source_filepath = source.string(); });

    const std::filesystem::path output =
        std::filesystem::temp_directory_path() / "pnanovdb_scene_serializer_collision.json";
    std::error_code ec;
    std::filesystem::remove(output, ec);
    std::string error;
    EXPECT_FALSE(save_scenes_to_file(manager, views, output.string(), &error));
    EXPECT_NE(error.find("camera already uses the same name"), std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(output));
    std::filesystem::remove(source, ec);
}

TEST(SceneSerializer, RenderInsertionRejectsCameraAndConsumesTransferredInputs)
{
    EditorSceneManager manager;
    pnanovdb_editor_token_t* scene = EditorToken::getInstance().getToken("namespace scene");
    pnanovdb_editor_token_t* camera_name = EditorToken::getInstance().getToken("namespace camera");
    manager.with_object_or_create(scene, camera_name,
                                  [](SceneObject* obj)
                                  {
                                      ASSERT_NE(obj, nullptr);
                                      obj->type = SceneObjectType::Camera;
                                  });

    pnanovdb_compute_t compute{};
    compute.destroy_array = destroy_rejected_array;
    pnanovdb_raster_t raster{};
    raster.destroy_gaussian_data = destroy_rejected_gaussian;
    auto* queue = reinterpret_cast<pnanovdb_compute_queue_t*>(uintptr_t{ 1 });
    auto* gaussian = reinterpret_cast<pnanovdb_raster_gaussian_data_t*>(uintptr_t{ 1 });
    pnanovdb_compute_array_t a{}, b{}, c{};
    g_rejected_array_destroys = 0;
    g_rejected_gaussian_destroys = 0;

    manager.add_nanovdb(scene, camera_name, &a, &b, &compute);
    EXPECT_EQ(g_rejected_array_destroys, 2);
    manager.add_mesh(
        scene, camera_name, &a, &b, &c, &compute, pnanovdb_pipeline_type_noop, pnanovdb_pipeline_type_nanovdb_render);
    EXPECT_EQ(g_rejected_array_destroys, 5);
    manager.add_gaussian_data(scene, camera_name, gaussian, &a, nullptr, &compute, &raster, queue);
    EXPECT_EQ(g_rejected_array_destroys, 6);
    EXPECT_EQ(g_rejected_gaussian_destroys, 1);
    manager.add_file_object(
        scene, camera_name, &compute, pnanovdb_pipeline_type_noop, pnanovdb_pipeline_type_nanovdb_render);

    manager.with_object(scene, camera_name,
                        [](SceneObject* obj)
                        {
                            ASSERT_NE(obj, nullptr);
                            EXPECT_EQ(obj->type, SceneObjectType::Camera);
                        });
}

TEST(SceneSerializer, RejectedCameraCollisionDoesNotDoubleOwnSharedInput)
{
    g_rejected_array_destroys = 0;
    {
        EditorSceneManager manager;
        pnanovdb_editor_token_t* scene = EditorToken::getInstance().getToken("shared input scene");
        pnanovdb_editor_token_t* owner_name = EditorToken::getInstance().getToken("owner");
        pnanovdb_editor_token_t* camera_name = EditorToken::getInstance().getToken("camera target");
        pnanovdb_compute_t compute{};
        compute.destroy_array = destroy_rejected_array;
        pnanovdb_compute_array_t shared{};

        manager.add_nanovdb(scene, owner_name, &shared, nullptr, &compute);
        manager.with_object_or_create(scene, camera_name, [](SceneObject* obj) { obj->type = SceneObjectType::Camera; });
        manager.add_nanovdb(scene, camera_name, &shared, nullptr, &compute);
        EXPECT_EQ(g_rejected_array_destroys, 0);
    }
    EXPECT_EQ(g_rejected_array_destroys, 1);
}

TEST(SceneSerializer, ScalarHelpersRespectStorageWidth)
{
    ImGuiDataType type = ImGuiDataType_COUNT;
    size_t size = 0;
    EXPECT_TRUE(reflect_type_to_imgui_type(PNANOVDB_REFLECT_TYPE_UINT8, type, size));
    EXPECT_EQ(type, ImGuiDataType_U8);
    EXPECT_EQ(size, sizeof(uint8_t));

    std::array<unsigned char, 3> bytes = { 0xaa, 0xbb, 0xcc };
    imgui_write_scalar(type, &bytes[1], 7.0);
    EXPECT_EQ(bytes[0], 0xaa);
    EXPECT_EQ(bytes[1], 7u);
    EXPECT_EQ(bytes[2], 0xcc);
    EXPECT_EQ(imgui_read_scalar(type, &bytes[1]), 7.0);

    EXPECT_TRUE(reflect_type_to_imgui_type(PNANOVDB_REFLECT_TYPE_UINT16, type, size));
    EXPECT_EQ(type, ImGuiDataType_U16);
    EXPECT_EQ(size, sizeof(uint16_t));
    uint16_t words[2] = { 0u, 0xbeefu };
    imgui_write_scalar(type, &words[0], 513.0);
    EXPECT_EQ(words[0], 513u);
    EXPECT_EQ(words[1], 0xbeefu);
    EXPECT_EQ(imgui_read_scalar(type, &words[0]), 513.0);

    EXPECT_TRUE(reflect_type_to_imgui_type(PNANOVDB_REFLECT_TYPE_CHAR, type, size));
    EXPECT_EQ(type, ImGuiDataType_S8);
    EXPECT_EQ(size, sizeof(int8_t));
    std::array<int8_t, 3> chars = { 11, 0, 22 };
    imgui_write_scalar(type, &chars[1], 1.0);
    EXPECT_EQ(chars[0], 11);
    EXPECT_EQ(chars[1], 1);
    EXPECT_EQ(chars[2], 22);
    EXPECT_EQ(imgui_read_scalar(type, &chars[1]), 1.0);
}

TEST(SceneSerializer, Float16ShaderScalarsUseRawBitSerialization)
{
    EXPECT_TRUE(shader_scalar_serialization_supported(ImGuiDataType_Float, sizeof(uint16_t)));
    EXPECT_TRUE(shader_scalar_serialization_supported(ImGuiDataType_Float, sizeof(float)));
    EXPECT_TRUE(shader_scalar_serialization_supported(ImGuiDataType_Float, sizeof(double)));

    constexpr uint16_t bits = 0x7bcd;
    const nlohmann::ordered_json encoded = bits;
    const nlohmann::json decoded = nlohmann::json::parse(encoded.dump());
    ASSERT_TRUE(decoded.is_number_unsigned());
    EXPECT_EQ(decoded.get<uint16_t>(), bits);
}

TEST(SceneSerializer, HalfConversionUsesTwoByteStorageWithoutLosingRawRestore)
{
    EXPECT_EQ(float_to_half_bits(1.0f), 0x3c00u);
    EXPECT_FLOAT_EQ(half_bits_to_float(0x3c00u), 1.0f);
    EXPECT_FLOAT_EQ(half_bits_to_float(0x0001u), 0x1p-24f);
    EXPECT_EQ(float_to_half_bits(-0.0f), 0x8000u);

    ShaderParam half;
    half.name = "half_value";
    half.type = ImGuiDataType_Float;
    half.resizeData(sizeof(uint16_t), 1u);
    std::vector<unsigned char> bytes(sizeof(uint16_t), 0u);
    ASSERT_TRUE(apply_shader_params_json({ half }, nlohmann::json{ { "half_value", 0x7bcdu } }, bytes));
    uint16_t restored = 0;
    std::memcpy(&restored, bytes.data(), sizeof(restored));
    EXPECT_EQ(restored, 0x7bcdu);
}

TEST(SceneSerializer, DoubleShaderScalarUsesEightByteStorage)
{
    ShaderParam value;
    value.name = "double_value";
    value.type = ImGuiDataType_Double;
    value.resizeData(sizeof(double), 1u);
    std::vector<unsigned char> bytes(sizeof(double), 0u);
    ASSERT_TRUE(apply_shader_params_json({ value }, nlohmann::json{ { "double_value", 3.25 } }, bytes));
    double restored = 0.0;
    std::memcpy(&restored, bytes.data(), sizeof(restored));
    EXPECT_DOUBLE_EQ(restored, 3.25);
}

TEST(SceneSerializer, GaussianVoxelsPerUnitIsSanitizedCentrally)
{
    GaussianVoxelizeParams storage;
    pnanovdb_pipeline_params_t params{};
    params.data = &storage;
    params.size = sizeof(storage);

    storage.voxels_per_unit = 0.0f;
    EXPECT_FLOAT_EQ(pipeline_params_get_voxels_per_unit(&params), 1.0f);
    storage.voxels_per_unit = std::numeric_limits<float>::infinity();
    EXPECT_FLOAT_EQ(pipeline_params_get_voxels_per_unit(&params), k_default_voxels_per_unit);
    ASSERT_TRUE(pipeline_params_set_voxels_per_unit(&params, 1000.0f));
    EXPECT_FLOAT_EQ(storage.voxels_per_unit, 512.0f);
}

TEST(SceneSerializer, RejectsNamedBindingsChangedAfterFileLoad)
{
    EditorSceneManager manager;
    SceneView views;
    pnanovdb_editor_token_t* scene = EditorToken::getInstance().getToken("modified mesh scene");
    pnanovdb_editor_token_t* name = EditorToken::getInstance().getToken("modified mesh");
    views.get_or_create_scene(scene);

    pnanovdb_compute_t compute{};
    compute.destroy_array = destroy_test_array;
    pnanovdb_compute_array_t indices{}, positions{}, colors{}, replacement{};
    ASSERT_TRUE(manager.add_mesh(scene, name, &indices, &positions, &colors, &compute, pnanovdb_pipeline_type_noop,
                                 pnanovdb_pipeline_type_noop));
    const std::filesystem::path source =
        std::filesystem::temp_directory_path() / "pnanovdb_scene_serializer_mesh_source.ply";
    {
        std::ofstream file(source);
        file << "ply";
    }
    manager.with_object(scene, name,
                        [&](SceneObject* obj)
                        {
                            obj->resources.source_filepath = source.string();
                            obj->pipeline.load().type = pnanovdb_pipeline_type_mesh_load;
                            obj->resources.named_arrays["colors"] = &replacement;
                        });

    const std::filesystem::path output =
        std::filesystem::temp_directory_path() / "pnanovdb_scene_serializer_modified_mesh.json";
    std::error_code ec;
    std::filesystem::remove(output, ec);
    std::string error;
    EXPECT_FALSE(save_scenes_to_file(manager, views, output.string(), &error));
    EXPECT_NE(error.find("modified mesh"), std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(output));
    std::filesystem::remove(source, ec);
}

TEST(SceneSerializer, MalformedReflectedScalarsPreserveExistingValues)
{
    GaussianVoxelizeParams params{};
    const float default_vpu = params.voxels_per_unit;

    json_to_reflect_params(nlohmann::json{ { "voxels_per_unit", "bad" } },
                           PNANOVDB_REFLECT_DATA_TYPE(GaussianVoxelizeParams), &params, sizeof(params));
    EXPECT_FLOAT_EQ(params.voxels_per_unit, default_vpu);

    json_to_reflect_params(nlohmann::json{ { "voxels_per_unit", std::numeric_limits<double>::max() } },
                           PNANOVDB_REFLECT_DATA_TYPE(GaussianVoxelizeParams), &params, sizeof(params));
    EXPECT_FLOAT_EQ(params.voxels_per_unit, default_vpu);

    uint8_t narrow = 17u;
    EXPECT_FALSE(reflect_write_scalar_json(PNANOVDB_REFLECT_TYPE_UINT8, &narrow, nlohmann::json(256u)));
    EXPECT_EQ(narrow, 17u);
}

TEST(SceneSerializer, ShortShaderArraysPreserveRemainingDefaults)
{
    ShaderParam slice_plane;
    slice_plane.name = "slice_plane";
    slice_plane.type = ImGuiDataType_Float;
    slice_plane.resizeData(sizeof(float), 4u);
    const std::vector<ShaderParam> params = { slice_plane };

    const std::array<float, 4> defaults = { 1.f, 2.f, 3.f, 4.f };
    std::vector<unsigned char> restored(sizeof(defaults));
    std::memcpy(restored.data(), defaults.data(), sizeof(defaults));
    const nlohmann::json saved = { { "slice_plane", nlohmann::json::array({ 9.f, "bad" }) } };
    ASSERT_TRUE(apply_shader_params_json(params, saved, restored));

    std::array<float, 4> actual{};
    std::memcpy(actual.data(), restored.data(), restored.size());
    EXPECT_EQ(actual, (std::array<float, 4>{ 9.f, 2.f, 3.f, 4.f }));
}

TEST(SceneSerializer, PartialShaderRestoreUsesDeclaredDefaultsNotSharedPool)
{
    ShaderParams shader_params;
    const std::string shader_name =
        "scene_serializer_restore_defaults_" + std::to_string(reinterpret_cast<uintptr_t>(&shader_params)) + ".slang";
    const std::filesystem::path compiled_params = pnanovdb_shader::getCompiledShaderParamsFilePath(shader_name.c_str());
    const std::filesystem::path declared_params = pnanovdb_shader::getShaderParamsFilePath(shader_name.c_str());
    ScopedFileRemove remove_compiled_params{ compiled_params };
    ScopedFileRemove remove_declared_params{ declared_params };
    {
        nlohmann::ordered_json compiled_json;
        auto& params = compiled_json[pnanovdb_shader::SHADER_PARAM_JSON];
        params["fixture_scalar"] = { { "type", "float" }, { "elementCount", 1 } };
        params["fixture_vector"] = { { "type", "float" }, { "elementCount", 3 } };
        std::ofstream compiled_file(compiled_params, std::ios::out | std::ios::trunc);
        ASSERT_TRUE(compiled_file.good());
        compiled_file << compiled_json.dump(4) << '\n';
    }
    {
        nlohmann::ordered_json declared_json;
        auto& params = declared_json[pnanovdb_shader::SHADER_PARAM_JSON];
        params["fixture_scalar"] = { { "value", 1.25f } };
        params["fixture_vector"] = { { "value", nlohmann::ordered_json::array({ 2.f, 3.f, 4.f }) } };
        std::ofstream declared_file(declared_params, std::ios::out | std::ios::trunc);
        ASSERT_TRUE(declared_file.good());
        declared_file << declared_json.dump(4) << '\n';
    }
    ASSERT_TRUE(shader_params.load(shader_name, false));

    std::vector<unsigned char> contaminated(PNANOVDB_COMPUTE_CONSTANT_BUFFER_MAX_SIZE, 0x5au);
    pnanovdb_compute_array_t contaminated_array{};
    contaminated_array.data = contaminated.data();
    contaminated_array.element_size = 1u;
    contaminated_array.element_count = contaminated.size();
    shader_params.set_compute_array_for_shader(shader_name, &contaminated_array);

    std::vector<unsigned char> restored;
    ASSERT_TRUE(json_to_shader_params(
        shader_params, shader_name, nlohmann::json{ { "fixture_vector", nlohmann::json::array({ 9.f }) } }, restored));
    ASSERT_GE(restored.size(), 4u * sizeof(float));
    std::array<float, 4> values{};
    std::memcpy(values.data(), restored.data(), sizeof(values));
    EXPECT_EQ(values, (std::array<float, 4>{ 1.25f, 9.f, 3.f, 4.f }));
}

TEST(SceneSerializer, PipelineStagesPersistAndResolveByTypeName)
{
    register_test_pipeline_descriptors();
    SceneObject obj;
    obj.type = SceneObjectType::Array;
    obj.scene_token = EditorToken::getInstance().getToken("stable type scene");
    obj.name_token = EditorToken::getInstance().getToken("stable type object");
    obj.pipeline.process().type = pnanovdb_pipeline_type_voxelbvh_build;

    EditorSceneManager manager;
    manager.with_object_or_create(obj.scene_token, obj.name_token,
                                  [&](SceneObject* stored)
                                  {
                                      ASSERT_NE(stored, nullptr);
                                      stored->type = obj.type;
                                      stored->pipeline.process().type = obj.pipeline.process().type;
                                  });
    SceneView views;
    views.get_or_create_scene(obj.scene_token);
    const nlohmann::ordered_json doc = serialize_scenes(manager, views);
    const auto& stage = doc.at("objects").at(0).at("pipeline").at("process").at(0);
    EXPECT_EQ(stage.at("type_id"), pipeline_type_id(pnanovdb_pipeline_type_voxelbvh_build));
    EXPECT_FALSE(stage.contains("type"));

    pnanovdb_pipeline_type_t decoded = pnanovdb_pipeline_type_noop;
    EXPECT_TRUE(pipeline_type_from_json(stage, decoded));
    EXPECT_EQ(decoded, pnanovdb_pipeline_type_voxelbvh_build);

    decoded = pnanovdb_pipeline_type_nanovdb_render;
    EXPECT_FALSE(pipeline_type_from_json(
        nlohmann::json{ { "type_id", pipeline_type_name(pnanovdb_pipeline_type_voxelbvh_build) } }, decoded));
    EXPECT_EQ(decoded, pnanovdb_pipeline_type_nanovdb_render);
}

TEST(SceneSerializer, TypeIdsDistinguishNoopFromUnknown)
{
    register_test_pipeline_descriptors();
    pnanovdb_pipeline_type_t decoded = pnanovdb_pipeline_type_nanovdb_render;
    EXPECT_TRUE(pipeline_type_from_json(
        nlohmann::json{ { "type_id", pipeline_type_id(pnanovdb_pipeline_type_noop) } }, decoded));
    EXPECT_EQ(decoded, pnanovdb_pipeline_type_noop);

    decoded = pnanovdb_pipeline_type_nanovdb_render;
    EXPECT_FALSE(pipeline_type_from_json(nlohmann::json{ { "type_id", "unknown_pipeline_id" } }, decoded));
    EXPECT_EQ(decoded, pnanovdb_pipeline_type_nanovdb_render);
}

TEST(SceneSerializer, ObjectWithoutLocalShaderParamsDoesNotCaptureSharedPool)
{
    EditorSceneManager manager;
    SceneView views;
    pnanovdb_editor_token_t* scene = EditorToken::getInstance().getToken("shared shader scene");
    pnanovdb_editor_token_t* local_name = EditorToken::getInstance().getToken("local params");
    pnanovdb_editor_token_t* default_name = EditorToken::getInstance().getToken("declared defaults");
    const std::string shader_name =
        "scene_serializer_shared_pool_" + std::to_string(reinterpret_cast<uintptr_t>(&manager)) + ".slang";
    const std::filesystem::path compiled_params = pnanovdb_shader::getCompiledShaderParamsFilePath(shader_name.c_str());
    ScopedFileRemove remove_compiled_params{ compiled_params };
    {
        nlohmann::ordered_json compiled_json;
        compiled_json[pnanovdb_shader::SHADER_PARAM_JSON]["fixture_value"] = { { "type", "float" },
                                                                               { "elementCount", 1 } };
        std::ofstream compiled_file(compiled_params, std::ios::out | std::ios::trunc);
        ASSERT_TRUE(compiled_file.good());
        compiled_file << compiled_json.dump(4) << '\n';
        ASSERT_TRUE(compiled_file.good());
    }
    ASSERT_TRUE(manager.shader_params.load(shader_name, false));

    std::vector<unsigned char> shared_pool(PNANOVDB_COMPUTE_CONSTANT_BUFFER_MAX_SIZE, 0x5au);
    pnanovdb_compute_array_t pool_array{};
    pool_array.data = shared_pool.data();
    pool_array.element_size = 1u;
    pool_array.element_count = shared_pool.size();
    manager.shader_params.set_compute_array_for_shader(shader_name, &pool_array);

    pnanovdb_editor_token_t* shader_token = EditorToken::getInstance().getToken(shader_name.c_str());
    manager.with_object_or_create(scene, local_name,
                                  [&](SceneObject* obj)
                                  {
                                      obj->type = SceneObjectType::Array;
                                      obj->shader_name() = shader_token;
                                      obj->params.shader_params_array = &pool_array;
                                  });
    manager.with_object_or_create(scene, default_name,
                                  [&](SceneObject* obj)
                                  {
                                      obj->type = SceneObjectType::Array;
                                      obj->shader_name() = shader_token;
                                      ASSERT_EQ(obj->params.shader_params_array, nullptr);
                                  });

    const nlohmann::ordered_json doc = serialize_scenes(manager, views);
    const auto default_object =
        std::find_if(doc.at("objects").begin(), doc.at("objects").end(),
                     [](const nlohmann::json& value) { return value.at("name") == "declared defaults"; });
    ASSERT_NE(default_object, doc.at("objects").end());
    EXPECT_FALSE(default_object->contains("shader_params"));
}

TEST(SceneSerializer, SaveAtomicallyReplacesExistingFileAndLeavesNoTemporary)
{
    EditorSceneManager manager;
    SceneView views;
    const std::filesystem::path directory = std::filesystem::temp_directory_path();
    const std::filesystem::path output = directory / "pnanovdb_scene_serializer_atomic.json";
    {
        std::ofstream old_file(output, std::ios::out | std::ios::trunc);
        ASSERT_TRUE(old_file.good());
        old_file << "old scene contents\n";
    }

    std::string error;
    ASSERT_TRUE(save_scenes_to_file(manager, views, output.string(), &error)) << error;

    nlohmann::json restored;
    {
        std::ifstream saved_file(output);
        ASSERT_TRUE(saved_file.good());
        saved_file >> restored;
    }
    EXPECT_EQ(restored.at("version"), k_scene_file_version);
    EXPECT_TRUE(restored.at("objects").empty());

    const std::string temporary_prefix = output.filename().string() + ".tmp.";
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(directory))
    {
        EXPECT_NE(entry.path().filename().string().rfind(temporary_prefix, 0u), 0u)
            << "temporary scene file was not cleaned up: " << entry.path();
    }

    std::error_code ec;
    std::filesystem::remove(output, ec);
}

#if !defined(_WIN32)
TEST(SceneSerializer, InvalidUtf8SavePreservesDestinationAndCleansTemporary)
{
    EditorSceneManager manager;
    SceneView views;
    const char invalid_scene_name[] = { 'i', 'n', 'v', 'a', 'l', 'i', 'd', '-', static_cast<char>(0xff), '\0' };
    pnanovdb_editor_token_t* scene = EditorToken::getInstance().getToken(invalid_scene_name);
    ASSERT_NE(views.get_or_create_scene(scene), nullptr);

    const std::filesystem::path directory = std::filesystem::temp_directory_path();
    const std::filesystem::path output = directory / "pnanovdb_scene_serializer_invalid_utf8.json";
    const std::string old_contents = "old scene contents\n";
    {
        std::ofstream old_file(output, std::ios::out | std::ios::trunc);
        ASSERT_TRUE(old_file.good());
        old_file << old_contents;
    }

    std::string error;
    bool saved = true;
    EXPECT_NO_THROW(saved = save_scenes_to_file(manager, views, output.string(), &error));
    EXPECT_FALSE(saved);
    EXPECT_FALSE(error.empty());

    std::ifstream preserved_file(output);
    ASSERT_TRUE(preserved_file.good());
    const std::string preserved((std::istreambuf_iterator<char>(preserved_file)), std::istreambuf_iterator<char>());
    EXPECT_EQ(preserved, old_contents);

    const std::string temporary_prefix = output.filename().string() + ".tmp.";
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(directory))
    {
        EXPECT_NE(entry.path().filename().string().rfind(temporary_prefix, 0u), 0u)
            << "temporary scene file was not cleaned up: " << entry.path();
    }

    std::error_code ec;
    std::filesystem::remove(output, ec);
}

TEST(SceneSerializer, AtomicOverwritePreservesDestinationPermissions)
{
    EditorSceneManager manager;
    SceneView views;
    const std::filesystem::path output =
        std::filesystem::temp_directory_path() / "pnanovdb_scene_serializer_permissions.json";
    {
        std::ofstream old_file(output, std::ios::out | std::ios::trunc);
        ASSERT_TRUE(old_file.good());
        old_file << "old scene contents\n";
    }
    constexpr std::filesystem::perms expected = std::filesystem::perms::owner_read | std::filesystem::perms::owner_write;
    std::error_code ec;
    std::filesystem::permissions(output, expected, std::filesystem::perm_options::replace, ec);
    ASSERT_FALSE(ec) << ec.message();

    std::string error;
    ASSERT_TRUE(save_scenes_to_file(manager, views, output.string(), &error)) << error;
    const std::filesystem::perms actual = std::filesystem::status(output, ec).permissions();
    ASSERT_FALSE(ec) << ec.message();
    EXPECT_EQ(actual, expected);

    std::filesystem::remove(output, ec);
}
#endif

TEST(SceneSerializer, FailedAtomicReplacePreservesDestinationAndCleansTemporary)
{
    EditorSceneManager manager;
    SceneView views;
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / "pnanovdb_scene_serializer_replace_failure";
    const std::filesystem::path marker = directory / "old_destination_marker";
    std::error_code ec;
    std::filesystem::remove_all(directory, ec);
    ASSERT_TRUE(std::filesystem::create_directory(directory));
    {
        std::ofstream marker_file(marker);
        ASSERT_TRUE(marker_file.good());
        marker_file << "preserve me\n";
    }

    std::string error;
    EXPECT_FALSE(save_scenes_to_file(manager, views, directory.string(), &error));
    EXPECT_TRUE(std::filesystem::is_directory(directory));
    EXPECT_TRUE(std::filesystem::is_regular_file(marker));

    const std::string temporary_prefix = directory.filename().string() + ".tmp.";
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(directory.parent_path()))
    {
        EXPECT_NE(entry.path().filename().string().rfind(temporary_prefix, 0u), 0u)
            << "temporary scene file was not cleaned up: " << entry.path();
    }

    std::filesystem::remove_all(directory, ec);
}

} // namespace
} // namespace pnanovdb_editor
