// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <nanovdb_editor/putil/Compiler.h>
#include <nanovdb_editor/putil/Compute.h>
#include <nanovdb_editor/putil/Editor.h>

#include "editor/Editor.h"
#include "editor/EditorScene.h"
#include "editor/EditorSceneManager.h"
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

class EditorStreamingTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        pnanovdb_compiler_load(&compiler);
        ASSERT_NE(compiler.module, nullptr) << "Compiler module not available";

        pnanovdb_compute_load(&compute, &compiler);
        ASSERT_NE(compute.module, nullptr) << "Failed to load compute module";

        device_manager = compute.device_interface.create_device_manager(PNANOVDB_FALSE);
        ASSERT_NE(device_manager, nullptr) << "Failed to create compute device manager";

        pnanovdb_compute_physical_device_desc_t phys_desc = {};
        if (!compute.device_interface.enumerate_devices(device_manager, 0u, &phys_desc))
        {
            GTEST_SKIP() << "No Vulkan-compatible device available on this machine";
        }

        pnanovdb_compute_device_desc_t device_desc = {};
        device = compute.device_interface.create_device(device_manager, &device_desc);
        ASSERT_NE(device, nullptr) << "Failed to create compute device";

        pnanovdb_editor_load(&editor, &compute, &compiler);
        ASSERT_NE(editor.module, nullptr) << "Editor module failed to load";

        auto sphere_grid = nanovdb::tools::createLevelSetSphere<float>(10.0f);
        nanovdb_array = compute.create_array(4u, sphere_grid.bufferSize() / 4u, sphere_grid.data());
        ASSERT_NE(nanovdb_array, nullptr) << "Failed to create nanovdb array";

        scene_token = editor.get_token("main");
        object_token = editor.get_token("test_object");
        cfg.ip_address = "127.0.0.1";
        cfg.port = 8080;
        cfg.headless = PNANOVDB_TRUE;
        cfg.streaming = PNANOVDB_TRUE;
    }

    void TearDown() override
    {
        if (editor.module)
        {
            editor.stop(&editor);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            pnanovdb_editor_free(&editor);
        }
        if (nanovdb_array)
        {
            compute.destroy_array(nanovdb_array);
        }
        if (device)
        {
            compute.device_interface.destroy_device(device_manager, device);
        }
        if (device_manager)
        {
            compute.device_interface.destroy_device_manager(device_manager);
        }
        if (compute.module)
        {
            pnanovdb_compute_free(&compute);
        }
        if (compiler.module)
        {
            pnanovdb_compiler_free(&compiler);
        }
    }

    pnanovdb_compiler_t compiler = {};
    pnanovdb_compute_t compute = {};
    pnanovdb_editor_t editor = {};
    pnanovdb_compute_device_manager_t* device_manager = nullptr;
    pnanovdb_compute_device_t* device = nullptr;
    pnanovdb_compute_array_t* nanovdb_array = nullptr;
    pnanovdb_editor_token_t* scene_token = nullptr;
    pnanovdb_editor_token_t* object_token = nullptr;
    pnanovdb_editor_config_t cfg = {};
};

TEST_F(EditorStreamingTest, EditorStartStopHeadlessStreaming)
{
    editor.start(&editor, device, &cfg);
    editor.add_nanovdb_2(&editor, scene_token, object_token, nanovdb_array);
    compute.destroy_array(nanovdb_array);
    nanovdb_array = nullptr;

    auto* mapped_shader = static_cast<pnanovdb_editor_shader_name_t*>(editor.map_params(
        &editor, scene_token, object_token, PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_editor_shader_name_t)));
    ASSERT_NE(mapped_shader, nullptr);
    mapped_shader->shader_name = editor.get_token("editor/wireframe.slang");
    editor.unmap_params(&editor, scene_token, object_token);

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
}

class EditorClientInterfaceTest : public EditorStreamingTest,
                                  public ::testing::WithParamInterface<std::tuple<bool, const char*>>
{
};

TEST_P(EditorClientInterfaceTest, UiProfilesHeadlessStreaming)
{
    if (std::get<0>(GetParam()))
    {
        // Old clients copy only the callbacks known to their headers.
        constexpr size_t legacy_size = offsetof(pnanovdb_editor_t, get_process_step_count);
        std::memset(reinterpret_cast<char*>(&editor) + legacy_size, 0, sizeof(editor) - legacy_size);
    }
    cfg.ui_profile_name = std::get<1>(GetParam());

    // Keep volume rendering outside the UI lifecycle checks.
    editor.add_nanovdb_2(&editor, scene_token, object_token, nanovdb_array);
    editor.set_pipeline(&editor, scene_token, object_token, pnanovdb_pipeline_stage_render, pnanovdb_pipeline_type_noop);
    ASSERT_EQ(editor.get_pipeline(&editor, scene_token, object_token, pnanovdb_pipeline_stage_render),
              pnanovdb_pipeline_type_noop);
    editor.start(&editor, device, &cfg);
    ASSERT_TRUE(pnanovdb_editor::pnanovdb_get_editor()->update_nanovdb_buffer(
        &editor, scene_token, object_token, nanovdb_array));
    ASSERT_TRUE(editor.impl->editor_worker->render_thread_tasks.run_blocking([]() { return PNANOVDB_TRUE; }));
    compute.destroy_array(nanovdb_array);
    nanovdb_array = nullptr;
    ASSERT_EQ(editor.get_pipeline(&editor, scene_token, object_token, pnanovdb_pipeline_stage_render),
              pnanovdb_pipeline_type_noop);

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
    auto run_on_render_thread = [&](std::function<pnanovdb_bool_t()> task)
    {
        return worker->render_thread_tasks.run_blocking([&]()
        {
            // The test and editor DLL keep separate ImGui context pointers.
            ImGui::SetCurrentContext(editor.impl->editor_scene->get_imgui_instance()->context);
            return task();
        });
    };
    auto wait_for_ui = [&](std::function<bool()> predicate)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (run_on_render_thread(
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
    EXPECT_TRUE(run_on_render_thread([&]()
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
        EXPECT_TRUE(run_on_render_thread([&]()
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

    EXPECT_TRUE(run_on_render_thread([&]()
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

namespace
{
using CreateArrayFn = decltype(pnanovdb_compute_t::create_array);
using DestroyArrayFn = decltype(pnanovdb_compute_t::destroy_array);
CreateArrayFn tracked_create_array = nullptr;
DestroyArrayFn tracked_destroy_array = nullptr;
std::atomic<int> live_param_arrays{0};

pnanovdb_compute_array_t* count_param_array_create(pnanovdb_uint64_t size,
                                                   pnanovdb_uint64_t count,
                                                   const void* data)
{
    auto* array = tracked_create_array(size, count, data);
    if (array && size * count == PNANOVDB_COMPUTE_CONSTANT_BUFFER_MAX_SIZE)
        ++live_param_arrays;
    return array;
}

void count_param_array_destroy(pnanovdb_compute_array_t* array)
{
    if (array && array->element_size * array->element_count == PNANOVDB_COMPUTE_CONSTANT_BUFFER_MAX_SIZE)
        --live_param_arrays;
    tracked_destroy_array(array);
}
}

TEST_F(EditorStreamingTest, RepeatedSelectedBufferUpdatesKeepParameterAllocationsBounded)
{
    editor.add_nanovdb_2(&editor, scene_token, object_token, nanovdb_array);
    editor.start(&editor, device, &cfg);
    auto worker = editor.impl->editor_worker;
    ASSERT_NE(worker, nullptr);
    ASSERT_TRUE(worker->render_thread_tasks.run_blocking([&]()
    {
        tracked_create_array = compute.create_array;
        tracked_destroy_array = compute.destroy_array;
        compute.create_array = count_param_array_create;
        compute.destroy_array = count_param_array_destroy;
        live_param_arrays = 0;
        for (int frame = 0; frame < 128; ++frame)
        {
            EXPECT_TRUE(editor.update_nanovdb_buffer(&editor, scene_token, object_token, nanovdb_array));
        }
        const int growth = live_param_arrays.load();
        compute.create_array = tracked_create_array;
        compute.destroy_array = tracked_destroy_array;
        EXPECT_LE(growth, 1) << "Selected stream updates retain old UI parameter arrays";
        return PNANOVDB_TRUE;
    }));
}

namespace
{
using DispatchNanoVDBFn = decltype(pnanovdb_compute_t::dispatch_shader_on_nanovdb_array);
using InitShaderFn = decltype(pnanovdb_compute_t::init_shader);
DispatchNanoVDBFn tracked_dispatch_nanovdb = nullptr;
InitShaderFn tracked_init_shader = nullptr;
pnanovdb_compute_array_t* tracked_array_a = nullptr;
pnanovdb_compute_array_t* tracked_array_b = nullptr;
std::atomic<int> rendered_a{0}, rendered_b{0}, uploaded_a{0}, compiled_materials{0};

pnanovdb_bool_t count_material_dispatch(const pnanovdb_compute_t* compute,
                                       const pnanovdb_compute_device_t* device,
                                       const pnanovdb_shader_context_t* shader,
                                       pnanovdb_compute_array_t* array,
                                       pnanovdb_int32_t width,
                                       pnanovdb_int32_t height,
                                       pnanovdb_compute_texture_t* background,
                                       pnanovdb_compute_buffer_transient_t* editor_params,
                                       pnanovdb_compute_buffer_transient_t* material_params,
                                       pnanovdb_compute_buffer_t** buffer,
                                       pnanovdb_compute_buffer_transient_t** readback)
{
    const bool uploads = !*buffer;
    const auto result = tracked_dispatch_nanovdb(compute, device, shader, array, width, height, background,
                                                 editor_params, material_params, buffer, readback);
    if (result && array == tracked_array_a)
    {
        ++rendered_a;
        if (uploads)
            ++uploaded_a;
    }
    if (result && array == tracked_array_b)
        ++rendered_b;
    return result;
}

pnanovdb_bool_t count_material_compile(const pnanovdb_compute_t* compute,
                                      pnanovdb_compute_queue_t* queue,
                                      pnanovdb_shader_context_t* shader,
                                      pnanovdb_compiler_settings_t* settings)
{
    ++compiled_materials;
    return tracked_init_shader(compute, queue, shader, settings);
}

class EditorMaterialRenderTest : public EditorStreamingTest
{
protected:
    void SetUp() override
    {
        EditorStreamingTest::SetUp();
        if (HasFatalFailure() || IsSkipped())
            return;
        tracked_dispatch_nanovdb = compute.dispatch_shader_on_nanovdb_array;
        tracked_init_shader = compute.init_shader;
        compute.dispatch_shader_on_nanovdb_array = count_material_dispatch;
        compute.init_shader = count_material_compile;
        tracked_array_a = nullptr;
        tracked_array_b = nullptr;
        rendered_a = rendered_b = uploaded_a = compiled_materials = 0;
        tracking = true;
    }

    void TearDown() override
    {
        if (tracking)
        {
            editor.stop(&editor);
            compute.dispatch_shader_on_nanovdb_array = tracked_dispatch_nanovdb;
            compute.init_shader = tracked_init_shader;
        }
        EditorStreamingTest::TearDown();
    }

    bool wait_for_frames(int a, int b = 0)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        auto worker = editor.impl->editor_worker;
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (rendered_a >= a && rendered_b >= b)
                return true;
            if (!worker->render_thread_tasks.run_blocking([]() { return PNANOVDB_TRUE; }))
                return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return false;
    }

    bool tracking = false;
};
}

TEST_F(EditorMaterialRenderTest, MixedShadersKeepObjectValuesAcrossFramesAndReload)
{
    auto* second = editor.get_token("wireframe");
    editor.add_nanovdb_2(&editor, scene_token, object_token, nanovdb_array);
    editor.add_nanovdb_2(&editor, scene_token, second, nanovdb_array);
    char error[1024]{};
    ASSERT_TRUE(editor.set_shader(&editor, scene_token, object_token, "editor/flow_smoke.slang",
                                  R"({"attenuation":7.75})", error, sizeof(error))) << error;
    ASSERT_TRUE(editor.set_shader(&editor, scene_token, second, "editor/wireframe.slang",
                                  R"({"highlight_bbox":1})", error, sizeof(error))) << error;
    auto& manager = *editor.impl->scene_manager;
    manager.with_object(scene_token, object_token,
                         [](pnanovdb_editor::SceneObject* obj) { tracked_array_a = obj->nanovdb_array(); });
    manager.with_object(scene_token, second,
                         [](pnanovdb_editor::SceneObject* obj) { tracked_array_b = obj->nanovdb_array(); });
    editor.start(&editor, device, &cfg);
    ASSERT_TRUE(wait_for_frames(3, 3));
    const int compile_count = compiled_materials.load();
    ASSERT_TRUE(wait_for_frames(rendered_a + 3, rendered_b + 3));
    EXPECT_EQ(compiled_materials.load(), compile_count);
    ASSERT_TRUE(editor.impl->editor_worker->render_thread_tasks.run_blocking([&]()
    {
        editor.impl->editor_scene->get_imgui_instance()->pending.update_shader = true;
        return PNANOVDB_TRUE;
    }));
    ASSERT_TRUE(wait_for_frames(rendered_a + 2, rendered_b + 2));
    EXPECT_GT(compiled_materials.load(), compile_count);
    manager.with_object(scene_token, object_token, [](pnanovdb_editor::SceneObject* obj)
    {
        EXPECT_FLOAT_EQ(*static_cast<float*>(obj->shader_params()), 7.75f);
    });
    manager.with_object(scene_token, second, [](pnanovdb_editor::SceneObject* obj)
    {
        EXPECT_EQ(*static_cast<uint32_t*>(obj->shader_params()), 1u);
    });
}

TEST_F(EditorMaterialRenderTest, SourceRevisionInvalidatesAnUnchangedArrayAddress)
{
    editor.add_nanovdb_2(&editor, scene_token, object_token, nanovdb_array);
    editor.impl->scene_manager->with_object(scene_token, object_token,
        [](pnanovdb_editor::SceneObject* obj) { tracked_array_a = obj->nanovdb_array(); });
    editor.start(&editor, device, &cfg);
    ASSERT_TRUE(wait_for_frames(3));
    const int uploads = uploaded_a.load();
    ASSERT_TRUE(wait_for_frames(rendered_a + 2));
    EXPECT_EQ(uploaded_a.load(), uploads);
    ASSERT_TRUE(editor.impl->editor_worker->render_thread_tasks.run_blocking([&]()
    {
        editor.impl->scene_manager->with_object(scene_token, object_token,
            [](pnanovdb_editor::SceneObject* obj) { ++obj->lifetime_id; });
        return PNANOVDB_TRUE;
    }));
    ASSERT_TRUE(wait_for_frames(rendered_a + 2));
    EXPECT_EQ(uploaded_a.load(), uploads + 1);
}
