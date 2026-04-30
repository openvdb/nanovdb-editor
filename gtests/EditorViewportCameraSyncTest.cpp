// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include "editor/EditorToken.h"
#include "editor/SceneView.h"

namespace pnanovdb_editor
{
namespace
{

TEST(NanoVDBEditor, SyncCameraOwnerPreservesViewportCameraContext)
{
    EditorToken::getInstance().clear();

    SceneView scene_view;
    pnanovdb_editor_token_t* scene_token = EditorToken::getInstance().getToken("scene");
    ASSERT_NE(scene_token, nullptr);

    SceneViewData* scene = scene_view.get_or_create_scene(scene_token);
    ASSERT_NE(scene, nullptr);

    pnanovdb_editor_token_t* viewport_token = scene_view.get_viewport_camera_token(scene_token);
    ASSERT_NE(viewport_token, nullptr);

    auto initial_it = scene->cameras.find(viewport_token->id);
    ASSERT_NE(initial_it, scene->cameras.end());

    const CameraViewContext original_context = initial_it->second;
    ASSERT_TRUE(original_context.camera_view);
    ASSERT_TRUE(original_context.camera_config);
    ASSERT_TRUE(original_context.camera_state);
    ASSERT_EQ(original_context.camera_view->configs, original_context.camera_config.get());
    ASSERT_EQ(original_context.camera_view->states, original_context.camera_state.get());

    scene_view.sync_camera_owner(scene_token, viewport_token, original_context.camera_view);

    scene = scene_view.get_or_create_scene(scene_token);
    ASSERT_NE(scene, nullptr);

    auto synced_it = scene->cameras.find(viewport_token->id);
    ASSERT_NE(synced_it, scene->cameras.end());

    const CameraViewContext& synced_context = synced_it->second;
    EXPECT_EQ(synced_context.camera_view, original_context.camera_view);
    ASSERT_TRUE(synced_context.camera_config);
    ASSERT_TRUE(synced_context.camera_state);
    EXPECT_EQ(synced_context.camera_config.get(), original_context.camera_config.get());
    EXPECT_EQ(synced_context.camera_state.get(), original_context.camera_state.get());
    EXPECT_EQ(synced_context.camera_view->configs, synced_context.camera_config.get());
    EXPECT_EQ(synced_context.camera_view->states, synced_context.camera_state.get());
}

} // namespace
} // namespace pnanovdb_editor
