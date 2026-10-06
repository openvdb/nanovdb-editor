// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <nanovdb_editor/putil/Compiler.h>
#include <nanovdb_editor/putil/Compute.h>
#include <nanovdb_editor/putil/Editor.h>

#include "editor/Editor.h"
#include "editor/EditorScene.h"
#include "editor/ImguiInstance.h"

#include <imgui_internal.h>

#include <nanovdb/tools/CreatePrimitives.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <memory>
#include <thread>
#include <tuple>

class EditorClientInterfaceTest : public ::testing::TestWithParam<std::tuple<bool, const char*>>
{
};

TEST_P(EditorClientInterfaceTest, EditorStartStopHeadlessStreaming)
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

    if (std::get<0>(GetParam()))
    {
        // Old clients copy only the callbacks known to their headers.
        constexpr size_t legacy_size = offsetof(pnanovdb_editor_t, get_process_step_count);
        std::memset(reinterpret_cast<char*>(&editor) + legacy_size, 0, sizeof(editor) - legacy_size);
    }

    // Create a minimal NanoVDB sphere grid programmatically
    auto sphere_grid = nanovdb::tools::createLevelSetSphere<float>(10.0f);

    // Create compute array from the grid data
    pnanovdb_compute_array_t* nanovdb_array = compute.create_array(4u, sphere_grid.bufferSize() / 4u, sphere_grid.data());
    ASSERT_NE(nanovdb_array, nullptr) << "Failed to create nanovdb array";

    // Configure editor (headless, streaming mode)
    pnanovdb_editor_config_t cfg = {};
    cfg.ip_address = "127.0.0.1";
    cfg.port = 8080;
    cfg.headless = PNANOVDB_TRUE;
    cfg.streaming = PNANOVDB_TRUE;
    cfg.ui_profile_name = std::get<1>(GetParam());

    // Start, wait briefly, then stop
    editor.start(&editor, device, &cfg);

    // Add nanovdb to a scene with a token
    pnanovdb_editor_token_t* scene_token = editor.get_token("main");
    pnanovdb_editor_token_t* object_token = editor.get_token("test_object");
    editor.add_nanovdb_2(&editor, scene_token, object_token, nanovdb_array);
    editor.add_nanovdb_2(&editor, scene_token, object_token, nanovdb_array);
    compute.destroy_array(nanovdb_array);

    ASSERT_NE(editor.impl->editor_scene, nullptr);
    auto* ui_editor = editor.impl->editor_scene->get_editor();
    EXPECT_NE(ui_editor, &editor);
    EXPECT_EQ(ui_editor->impl, editor.impl);
    EXPECT_EQ(ui_editor->module, editor.module);
    EXPECT_NE(ui_editor->get_process_step_count, nullptr);
    EXPECT_NE(ui_editor->map_process_step_params, nullptr);
    EXPECT_NE(ui_editor->unmap_process_step_params, nullptr);
    EXPECT_NE(ui_editor->set_process_step, nullptr);
    if (std::get<0>(GetParam()))
    {
        EXPECT_EQ(editor.get_process_step_count, nullptr);
        EXPECT_EQ(editor.set_process_step, nullptr);
    }

    auto worker = editor.impl->editor_worker;
    auto wait_for_ui = [&](std::function<bool()> predicate)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (worker->render_thread_tasks.run_blocking(
                    [&]() { return predicate() ? PNANOVDB_TRUE : PNANOVDB_FALSE; }))
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    };
    auto params_are_docked = [&]()
    {
        const auto* properties = ImGui::FindWindowByName("Properties");
        const auto* params = ImGui::FindWindowByName("Params");
        return properties && params && properties->DockId && properties->DockId == params->DockId &&
               ImGui::FindWindowByName("Simulation") == nullptr;
    };
    EXPECT_TRUE(wait_for_ui(params_are_docked));

    auto* schema = editor.get_token(R"json({"SceneParams": {
        "Play": {"type": "bool", "value": false, "widget": "toggleButton", "group": "Simulation"},
        "Restart": {"type": "bool", "value": false, "widget": "button"},
        "Other": {"type": "bool", "value": false, "widget": "button", "group": "Other"}
    }})json");
    EXPECT_TRUE(editor.set_custom_scene_params(&editor, scene_token, schema, nullptr, 0));
    EXPECT_TRUE(worker->render_thread_tasks.run_blocking([&]()
    {
        auto* params = ImGui::FindWindowByName("Params");
        if (!params)
        {
            return PNANOVDB_FALSE;
        }
        if (params->DockNode && params->DockNode->TabBar)
        {
            params->DockNode->TabBar->NextSelectedTabId = params->TabId;
        }
        ImGui::FocusWindow(params);
        return PNANOVDB_TRUE;
    }));
    EXPECT_TRUE(wait_for_ui([&]()
    {
        const auto* params = ImGui::FindWindowByName("Params");
        return params_are_docked() && params->Active && !params->Hidden;
    }));

    for (const char* field_name : { "Play", "Restart", "Other" })
    {
        EXPECT_TRUE(worker->render_thread_tasks.run_blocking([&]()
        {
            auto* params = ImGui::FindWindowByName("Params");
            if (!params)
            {
                return PNANOVDB_FALSE;
            }
            ImGui::ActivateItemByID(ImHashStr("###custom", 0, params->GetID(field_name)));
            return PNANOVDB_TRUE;
        }));
        EXPECT_TRUE(wait_for_ui([&]()
        {
            const auto* type = editor.get_custom_scene_params_data_type(&editor, scene_token);
            auto* data = static_cast<const char*>(editor.map_params(&editor, scene_token, nullptr, type));
            if (!data)
            {
                return false;
            }
            bool activated = false;
            for (pnanovdb_uint64_t i = 0; i < type->child_reflect_data_count; ++i)
            {
                const auto& field = type->child_reflect_datas[i];
                if (std::strcmp(field.name, field_name) == 0)
                {
                    activated = *reinterpret_cast<const pnanovdb_bool_t*>(data + field.data_offset) == PNANOVDB_TRUE;
                }
            }
            editor.unmap_params(&editor, scene_token, nullptr);
            return activated && params_are_docked();
        })) << field_name;
    }

    EXPECT_TRUE(worker->render_thread_tasks.run_blocking([&]()
    {
        auto* handler = ImGui::FindSettingsHandler("RenderSettings");
        if (!handler)
        {
            return PNANOVDB_FALSE;
        }
        auto* instance = static_cast<imgui_instance_user::Instance*>(handler->UserData);
        EXPECT_STREQ(instance->render_settings->ui_profile_name, cfg.ui_profile_name);
        instance->loaded_ini_once = false;
        return PNANOVDB_TRUE;
    }));
    EXPECT_TRUE(wait_for_ui([&]()
    {
        auto* handler = ImGui::FindSettingsHandler("RenderSettings");
        if (!handler)
        {
            return false;
        }
        const auto* instance = static_cast<imgui_instance_user::Instance*>(handler->UserData);
        return instance->loaded_ini_once &&
               std::strcmp(instance->render_settings->ui_profile_name, cfg.ui_profile_name) == 0 && params_are_docked();
    }));

    schema = editor.get_token(R"json({"SceneParams": {
        "Counter": {"type": "uint", "group": "Simulation", "hidden": true}
    }})json");
    EXPECT_TRUE(editor.set_custom_scene_params(&editor, scene_token, schema, nullptr, 0));
    EXPECT_TRUE(wait_for_ui(params_are_docked));

    // Use map_params to set the shader to wireframe.slang
    pnanovdb_editor_shader_name_t* mapped_shader = (pnanovdb_editor_shader_name_t*)editor.map_params(
        &editor, scene_token, object_token, PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_editor_shader_name_t));
    if (mapped_shader)
    {
        mapped_shader->shader_name = editor.get_token("editor/wireframe.slang");
        editor.unmap_params(&editor, scene_token, object_token);
    }

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

INSTANTIATE_TEST_SUITE_P(CurrentAndLegacyClients,
                         EditorClientInterfaceTest,
                         ::testing::Combine(::testing::Bool(), ::testing::Values("viewer", "nvflow")));

TEST(NanoVDBEditor, ShutdownFromRenderThreadDefersTeardown)
{
    pnanovdb_compiler_t compiler = {};
    pnanovdb_compiler_load(&compiler);
    ASSERT_NE(compiler.module, nullptr);

    pnanovdb_compute_t compute = {};
    pnanovdb_compute_load(&compute, &compiler);
    ASSERT_NE(compute.module, nullptr);

    pnanovdb_editor_t editor = {};
    pnanovdb_editor_load(&editor, &compute, &compiler);
    ASSERT_NE(editor.module, nullptr);
    ASSERT_NE(editor.impl, nullptr);

    // Model shutdown/free being requested by a callback running inside the
    // active render loop.  Such a thread cannot join itself.
    auto worker = std::make_shared<pnanovdb_editor::EditorWorker>();
    worker->render_thread_id.store(std::this_thread::get_id());
    editor.impl->editor_worker = worker;

    pnanovdb_editor_free(&editor);

    EXPECT_NE(editor.impl, nullptr) << "self-thread shutdown must leave live render-loop state intact";
    EXPECT_TRUE(worker->should_stop.load()) << "self-thread shutdown must still request loop termination";

    // Simulate run_show_loop() releasing its worker, then finish teardown from
    // an external thread as required by the API contract.
    {
        std::lock_guard<std::mutex> lock(editor.impl->editor_worker_lifecycle_mutex);
        editor.impl->editor_worker = nullptr;
    }
    worker.reset();
    pnanovdb_editor_free(&editor);
    EXPECT_EQ(editor.impl, nullptr);

    pnanovdb_compute_free(&compute);
    pnanovdb_compiler_free(&compiler);
}

TEST(NanoVDBEditor, WorkerRestartUsesFreshTaskQueueState)
{
    using pnanovdb_editor::EditorWorker;

    // First lifecycle: queue accepts and runs work when the (modelled) render loop drains.
    auto worker_a = std::make_shared<EditorWorker>();
    std::atomic<int> a_runs{ 0 };
    worker_a->render_thread_tasks.run_async([&a_runs]() { a_runs.fetch_add(1, std::memory_order_relaxed); });
    worker_a->render_thread_tasks.drain();
    EXPECT_EQ(a_runs.load(std::memory_order_relaxed), 1);

    // Teardown mirrors stop(): signal stop, then close the queue.
    worker_a->should_stop.store(true);
    worker_a->render_thread_tasks.close();
    worker_a->render_thread_tasks.run_async([&a_runs]() { a_runs.fetch_add(1, std::memory_order_relaxed); });
    worker_a->render_thread_tasks.drain();
    EXPECT_EQ(a_runs.load(std::memory_order_relaxed), 1) << "a closed worker queue must not run new work";
    worker_a.reset();

    // Restart: a brand-new worker starts clean, with no residue from the previous lifecycle.
    auto worker_b = std::make_shared<EditorWorker>();
    EXPECT_FALSE(worker_b->should_stop.load()) << "restarted worker must not inherit stop state";
    EXPECT_TRUE(worker_b->is_starting.load()) << "restarted worker must begin in the starting state";
    std::atomic<int> b_runs{ 0 };
    worker_b->render_thread_tasks.run_async([&b_runs]() { b_runs.fetch_add(1, std::memory_order_relaxed); });
    worker_b->render_thread_tasks.drain();
    EXPECT_EQ(b_runs.load(std::memory_order_relaxed), 1)
        << "restarted worker's queue must accept and run work independent of the closed one";
}
