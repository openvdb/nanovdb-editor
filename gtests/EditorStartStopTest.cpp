// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <nanovdb_editor/putil/Compiler.h>
#include <nanovdb_editor/putil/Compute.h>
#include <nanovdb_editor/putil/Editor.h>

#include <chrono>
#include <thread>

TEST(NanoVDBEditor, EditorStartStopHeadless)
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

    // Configure editor (headless, no streaming)
    pnanovdb_editor_config_t cfg = {};
    cfg.ip_address = "127.0.0.1";
    cfg.port = 8080;
    cfg.headless = PNANOVDB_TRUE;
    cfg.streaming = PNANOVDB_FALSE;

    // Start, wait briefly, then stop
    editor.start(&editor, device, &cfg);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    editor.stop(&editor);

    // Cleanup
    pnanovdb_editor_free(&editor);
    compute.device_interface.destroy_device(device_manager, device);
    compute.device_interface.destroy_device_manager(device_manager);
    pnanovdb_compute_free(&compute);
    pnanovdb_compiler_free(&compiler);

    SUCCEED();
}


