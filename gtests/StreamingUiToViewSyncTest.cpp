// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <nanovdb/tools/CreatePrimitives.h>

#include <nanovdb_editor/putil/Compiler.h>
#include <nanovdb_editor/putil/Compute.h>
#include <nanovdb_editor/putil/Editor.h>

#include "editor/Editor.h"
#include "editor/EditorSceneManager.h"
#include "editor/ShaderParams.h"
#include "EditorTestSupport.h"
#include "GpuTestSupport.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>

namespace
{

constexpr const char* kDefaultEditorShader = "editor/editor.slang";

constexpr auto kWorkerStartupTimeout = std::chrono::milliseconds(60000);
constexpr auto kObjectReadyTimeout = std::chrono::milliseconds(30000);
constexpr auto kPropagationTimeout = std::chrono::milliseconds(30000);

bool wait_until(std::function<bool()> predicate, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (predicate())
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return predicate();
}

} // namespace

TEST(StreamingUiToViewSync, PoolMutationPropagatesToObjectBufferEachFrame)
{
    pnanovdb_compiler_t compiler{};
    pnanovdb_compiler_load(&compiler);
    ASSERT_NE(compiler.module, nullptr) << "Compiler module not available";

    pnanovdb_compute_t compute{};
    pnanovdb_compute_load(&compute, &compiler);
    ASSERT_NE(compute.module, nullptr) << "Failed to load compute module";

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
            phys_desc.device_name, "streaming UI-sync test (headless editor worker races teardown)");
        compute.device_interface.destroy_device_manager(device_manager);
        pnanovdb_compute_free(&compute);
        pnanovdb_compiler_free(&compiler);
        GTEST_SKIP() << skip_reason;
    }

    pnanovdb_compute_device_desc_t device_desc{};
    pnanovdb_compute_device_t* device = compute.device_interface.create_device(device_manager, &device_desc);
    ASSERT_NE(device, nullptr);

    pnanovdb_editor_t editor{};
    pnanovdb_editor_load(&editor, &compute, &compiler);
    ASSERT_NE(editor.module, nullptr);
    ASSERT_NE(editor.impl, nullptr);

    pnanovdb_editor_config_t cfg{};
    cfg.ip_address = "127.0.0.1";
    cfg.port = 8090;
    cfg.headless = PNANOVDB_TRUE;
    cfg.streaming = PNANOVDB_FALSE;

    editor.start(&editor, device, &cfg);

    ASSERT_NE(editor.impl->editor_worker, nullptr) << "Streaming mode must create an EditorWorker";

    ASSERT_TRUE(wait_until([&]() { return !editor.impl->editor_worker->is_starting.load(); }, kWorkerStartupTimeout))
        << "Editor worker did not finish starting";

    pnanovdb_editor_token_t* scene_token = editor.get_token("ui_sync_streaming_scene");
    pnanovdb_editor_token_t* name_token = editor.get_token("ui_sync_streaming_object");
    ASSERT_NE(scene_token, nullptr);
    ASSERT_NE(name_token, nullptr);

    auto sphere_grid = nanovdb::tools::createLevelSetSphere<float>(4.0f);
    pnanovdb_compute_array_t* nanovdb_array = compute.create_array(4u, sphere_grid.bufferSize() / 4u, sphere_grid.data());
    ASSERT_NE(nanovdb_array, nullptr);
    editor.add_nanovdb_2(&editor, scene_token, name_token, nanovdb_array);
    compute.destroy_array(nanovdb_array);

    ASSERT_TRUE(wait_until(
        [&]() { return pnanovdb_editor_test::get_object_shader_params_ptr(&editor, scene_token, name_token) != nullptr; },
        kObjectReadyTimeout))
        << "Per-object shader_params buffer was never allocated for the added object";

    std::array<uint8_t, 64> baseline{};
    ASSERT_EQ(pnanovdb_editor_test::snapshot_object_shader_params(
                  &editor, scene_token, name_token, baseline.data(), baseline.size()),
              baseline.size());
    const float baseline_first = *reinterpret_cast<const float*>(baseline.data());

    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    constexpr float kSentinel = 7.5f;
    auto& shader_params = editor.impl->scene_manager->shader_params;
    pnanovdb_compute_array_t* pool_array = shader_params.get_compute_array_for_shader(kDefaultEditorShader, &compute);
    ASSERT_NE(pool_array, nullptr) << "No compute array for default editor shader; cannot exercise UI sync";
    ASSERT_NE(pool_array->data, nullptr);
    ASSERT_GE(pool_array->element_size * pool_array->element_count, sizeof(float));
    *reinterpret_cast<float*>(pool_array->data) = kSentinel;
    shader_params.set_compute_array_for_shader(kDefaultEditorShader, pool_array);
    compute.destroy_array(pool_array);

    std::array<uint8_t, 64> after{};
    const bool propagated = wait_until(
        [&]()
        {
            std::memset(after.data(), 0, after.size());
            pnanovdb_editor_test::snapshot_object_shader_params(
                &editor, scene_token, name_token, after.data(), after.size());
            return *reinterpret_cast<const float*>(after.data()) == kSentinel;
        },
        kPropagationTimeout);

    editor.stop(&editor);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    pnanovdb_editor_free(&editor);
    compute.device_interface.destroy_device(device_manager, device);
    compute.device_interface.destroy_device_manager(device_manager);
    pnanovdb_compute_free(&compute);
    pnanovdb_compiler_free(&compiler);

    EXPECT_NE(baseline_first, kSentinel) << "Test is broken: baseline already equals sentinel";
    EXPECT_TRUE(propagated)
        << "Regression: in streaming mode, UI pool mutation never propagated to SceneObject::shader_params() "
        << "Per-object buffer first float = " << *reinterpret_cast<const float*>(after.data()) << " (expected "
        << kSentinel << ")";
}
