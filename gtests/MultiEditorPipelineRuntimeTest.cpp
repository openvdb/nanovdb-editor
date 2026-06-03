// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <nanovdb_editor/putil/Compiler.h>
#include <nanovdb_editor/putil/Compute.h>
#include <nanovdb_editor/putil/Editor.h>

#include "editor/PipelineTypes.h" // canonical pnanovdb_pipeline_type_* enum

#include <nanovdb/tools/CreatePrimitives.h>

#include <chrono>
#include <thread>

namespace
{
struct EditorHandle
{
    pnanovdb_editor_t editor = {};
};
} // namespace

TEST(NanoVDBEditor, MultiEditorPipelineRuntimeIsolation)
{
    pnanovdb_compiler_t compiler = {};
    pnanovdb_compiler_load(&compiler);
    ASSERT_NE(compiler.module, nullptr) << "Compiler module not available";

    pnanovdb_compute_t compute = {};
    pnanovdb_compute_load(&compute, &compiler);
    ASSERT_NE(compute.module, nullptr) << "Failed to load compute module";

    pnanovdb_compute_device_desc_t device_desc = {};
    pnanovdb_compute_device_manager_t* device_manager = compute.device_interface.create_device_manager(PNANOVDB_FALSE);
    ASSERT_NE(device_manager, nullptr) << "Failed to create compute device manager";

    pnanovdb_compute_physical_device_desc_t phys_desc = {};
    if (!compute.device_interface.enumerate_devices(device_manager, 0u, &phys_desc))
    {
        compute.device_interface.destroy_device_manager(device_manager);
        pnanovdb_compute_free(&compute);
        pnanovdb_compiler_free(&compiler);
        GTEST_SKIP() << "No Vulkan-compatible device available on this machine";
    }

    pnanovdb_compute_device_t* device_a = compute.device_interface.create_device(device_manager, &device_desc);
    pnanovdb_compute_device_t* device_b = compute.device_interface.create_device(device_manager, &device_desc);
    ASSERT_NE(device_a, nullptr) << "Failed to create device A";
    ASSERT_NE(device_b, nullptr) << "Failed to create device B";

    EditorHandle a;
    EditorHandle b;
    pnanovdb_editor_load(&a.editor, &compute, &compiler);
    pnanovdb_editor_load(&b.editor, &compute, &compiler);
    ASSERT_NE(a.editor.module, nullptr) << "Editor A failed to load";
    ASSERT_NE(b.editor.module, nullptr) << "Editor B failed to load";

    auto sphere_a = nanovdb::tools::createLevelSetSphere<float>(8.0f);
    auto sphere_b = nanovdb::tools::createLevelSetSphere<float>(12.0f);

    pnanovdb_compute_array_t* arr_a = compute.create_array(4u, sphere_a.size() / 4u, sphere_a.data());
    pnanovdb_compute_array_t* arr_b = compute.create_array(4u, sphere_b.size() / 4u, sphere_b.data());
    ASSERT_NE(arr_a, nullptr);
    ASSERT_NE(arr_b, nullptr);

    pnanovdb_editor_config_t cfg_a = {};
    cfg_a.ip_address = "127.0.0.1";
    cfg_a.port = 8090;
    cfg_a.headless = PNANOVDB_TRUE;
    cfg_a.streaming = PNANOVDB_FALSE;

    pnanovdb_editor_config_t cfg_b = {};
    cfg_b.ip_address = "127.0.0.1";
    cfg_b.port = 8091;
    cfg_b.headless = PNANOVDB_TRUE;
    cfg_b.streaming = PNANOVDB_FALSE;

    a.editor.start(&a.editor, device_a, &cfg_a);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    b.editor.start(&b.editor, device_b, &cfg_b);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    ASSERT_NE(a.editor.impl, nullptr);
    ASSERT_NE(b.editor.impl, nullptr);
    EXPECT_NE(a.editor.impl, b.editor.impl)
        << "Each editor must own its own impl (and therefore its own PipelineRuntime)";

    pnanovdb_editor_token_t* scene = a.editor.get_token("shared_scene_name");
    pnanovdb_editor_token_t* only_in_a = a.editor.get_token("only_in_a");
    pnanovdb_editor_token_t* only_in_b = b.editor.get_token("only_in_b");
    ASSERT_EQ(scene, b.editor.get_token("shared_scene_name")) << "Tokens are interned process-globally";

    a.editor.add_nanovdb_2(&a.editor, scene, only_in_a, arr_a);
    b.editor.add_nanovdb_2(&b.editor, scene, only_in_b, arr_b);

    compute.destroy_array(arr_a);
    compute.destroy_array(arr_b);

    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    EXPECT_EQ(a.editor.get_pipeline(&a.editor, scene, only_in_a, pnanovdb_pipeline_stage_render),
              pnanovdb_pipeline_type_nanovdb_render);
    EXPECT_EQ(b.editor.get_pipeline(&b.editor, scene, only_in_b, pnanovdb_pipeline_stage_render),
              pnanovdb_pipeline_type_nanovdb_render);
    EXPECT_EQ(
        b.editor.get_pipeline(&b.editor, scene, only_in_a, pnanovdb_pipeline_stage_render), pnanovdb_pipeline_type_noop)
        << "Editor B must not see object only added to Editor A";
    EXPECT_EQ(
        a.editor.get_pipeline(&a.editor, scene, only_in_b, pnanovdb_pipeline_stage_render), pnanovdb_pipeline_type_noop)
        << "Editor A must not see object only added to Editor B";

    a.editor.set_pipeline(&a.editor, scene, only_in_a, pnanovdb_pipeline_stage_render, pnanovdb_pipeline_type_raster2d);
    EXPECT_EQ(a.editor.get_pipeline(&a.editor, scene, only_in_a, pnanovdb_pipeline_stage_render),
              pnanovdb_pipeline_type_raster2d);
    EXPECT_EQ(b.editor.get_pipeline(&b.editor, scene, only_in_b, pnanovdb_pipeline_stage_render),
              pnanovdb_pipeline_type_nanovdb_render)
        << "set_pipeline on Editor A must not leak into Editor B's per-object state";

    b.editor.stop(&b.editor);
    a.editor.stop(&a.editor);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    pnanovdb_editor_free(&b.editor);
    pnanovdb_editor_free(&a.editor);
    compute.device_interface.destroy_device(device_manager, device_b);
    compute.device_interface.destroy_device(device_manager, device_a);
    compute.device_interface.destroy_device_manager(device_manager);
    pnanovdb_compute_free(&compute);
    pnanovdb_compiler_free(&compiler);
}
