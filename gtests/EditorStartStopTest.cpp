// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <nanovdb_editor/putil/Compiler.h>
#include <nanovdb_editor/putil/Compute.h>
#include <nanovdb_editor/putil/Editor.h>

#include <nanovdb/tools/CreatePrimitives.h>

#include <chrono>
#include <thread>

TEST(NanoVDBEditor, EditorStartStopHeadlessStreaming)
{
    // Load compiler
    pnanovdb_compiler_t compiler = {};
    pnanovdb_compiler_load(&compiler);
    ASSERT_NE(compiler.module, nullptr) << "Compiler module not available";

    // Load compute
    pnanovdb_compute_t compute = {};
    pnanovdb_compute_load(&compute, &compiler);
    ASSERT_NE(compute.module, nullptr) << "Failed to load compute module";

    // Create device manager and device
    pnanovdb_compute_device_desc_t device_desc = {};
    pnanovdb_compute_device_manager_t* device_manager = compute.device_interface.create_device_manager(PNANOVDB_FALSE);
    ASSERT_NE(device_manager, nullptr) << "Failed to create compute device manager";

    // Skip if no device available
    pnanovdb_compute_physical_device_desc_t phys_desc = {};
    if (!compute.device_interface.enumerate_devices(device_manager, 0u, &phys_desc))
    {
        compute.device_interface.destroy_device_manager(device_manager);
        pnanovdb_compute_free(&compute);
        pnanovdb_compiler_free(&compiler);
        GTEST_SKIP() << "No Vulkan-compatible device available on this machine";
    }

    pnanovdb_compute_device_t* device = compute.device_interface.create_device(device_manager, &device_desc);
    ASSERT_NE(device, nullptr) << "Failed to create compute device";

    // Load editor
    pnanovdb_editor_t editor = {};
    pnanovdb_editor_load(&editor, &compute, &compiler);
    ASSERT_NE(editor.module, nullptr) << "Editor module failed to load";

    // Create a minimal NanoVDB sphere grid programmatically
    auto sphere_grid = nanovdb::tools::createLevelSetSphere<float>(10.0f);

    // Create compute array from the grid data
    pnanovdb_compute_array_t* nanovdb_array = compute.create_array(4u, sphere_grid.size() / 4u, sphere_grid.data());
    ASSERT_NE(nanovdb_array, nullptr) << "Failed to create nanovdb array";

    // Add nanovdb to a scene with a token
    pnanovdb_editor_token_t* scene_token = editor.get_token("main");
    pnanovdb_editor_token_t* object_token = editor.get_token("test_object");
    editor.add_nanovdb_2(&editor, scene_token, object_token, nanovdb_array);
    compute.destroy_array(nanovdb_array);

    // Use map_params to set the shader to wireframe.slang
    pnanovdb_editor_shader_name_t* mapped_shader = (pnanovdb_editor_shader_name_t*)editor.map_params(
        &editor, scene_token, object_token, PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_editor_shader_name_t));
    if (mapped_shader)
    {
        mapped_shader->shader_name = editor.get_token("editor/wireframe.slang");
        editor.unmap_params(&editor, scene_token, object_token);
    }

    // Configure editor (headless, streaming mode)
    pnanovdb_editor_config_t cfg = {};
    cfg.ip_address = "127.0.0.1";
    cfg.port = 8080;
    cfg.headless = PNANOVDB_TRUE;
    cfg.streaming = PNANOVDB_TRUE;

    // Start, wait briefly, then stop
    editor.start(&editor, device, &cfg);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    editor.stop(&editor);

    // Give extra time for background thread cleanup
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Cleanup - sphere_grid stays alive until here
    pnanovdb_editor_free(&editor);
    compute.device_interface.destroy_device(device_manager, device);
    compute.device_interface.destroy_device_manager(device_manager);
    pnanovdb_compute_free(&compute);
    pnanovdb_compiler_free(&compiler);

    SUCCEED();
}
