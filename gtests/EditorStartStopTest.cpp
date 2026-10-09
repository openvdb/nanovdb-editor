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
#include "editor/ShaderCompileUtils.h"
#include "ShaderMappingTestSupport.h"

#include <imgui_internal.h>

#include <nanovdb/tools/CreatePrimitives.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <filesystem>
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

    pnanovdb_bool_t run_on_render_thread(std::function<pnanovdb_bool_t()> task)
    {
        return editor.impl->editor_worker->render_thread_tasks.run_blocking([&]()
        {
            // The test and editor DLL keep separate ImGui context pointers.
            ImGui::SetCurrentContext(editor.impl->editor_scene->get_imgui_instance()->context);
            return task();
        });
    }

    bool wait_for_ui(std::function<bool()> predicate, std::chrono::seconds timeout = std::chrono::seconds(10))
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (run_on_render_thread([&]() { return predicate() ? PNANOVDB_TRUE : PNANOVDB_FALSE; }))
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    }

    bool start_editor()
    {
        editor.start(&editor, device, &cfg);
        // Cold software-renderer startup must finish before functional timeouts start.
        return wait_for_ui([&]() { return editor.impl->editor_scene->get_imgui_instance()->loaded_ini_once; },
                           std::chrono::seconds(120));
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

TEST_F(EditorStreamingTest, ProfileSwitchPreservesLiveCamera)
{
    cfg.ui_profile_name = "viewer";
    editor.add_nanovdb_2(&editor, scene_token, object_token, nanovdb_array);
    editor.set_pipeline(&editor, scene_token, object_token, pnanovdb_pipeline_stage_render, pnanovdb_pipeline_type_noop);
    ASSERT_TRUE(start_editor());
    auto worker = editor.impl->editor_worker;
    auto wait_for_profile = [&](const char* profile)
    {
        return wait_for_ui([&]()
        {
            auto* instance = editor.impl->editor_scene->get_imgui_instance();
            return instance->loaded_ini_once && instance->current_profile_name == profile;
        });
    };
    ASSERT_TRUE(wait_for_profile("viewer"));
    for (const char* profile : { "nvflow", "default", "viewer" })
    {
        ASSERT_TRUE(worker->render_thread_tasks.run_blocking([&]()
        {
            auto* settings = editor.impl->editor_scene->get_imgui_instance()->render_settings;
            settings->camera_state.position = { 13.f, 17.f, 23.f };
            settings->camera_state.eye_distance_from_position = 42.f;
            settings->camera_config.fov_angle_y = 0.6f;
            settings->camera_config.orthographic_y = 19.f;
            settings->camera_config.is_orthographic = PNANOVDB_TRUE;
            settings->is_orthographic = PNANOVDB_TRUE;
            settings->sync_camera = PNANOVDB_TRUE;
            std::strcpy(settings->ui_profile_name, profile);
            return PNANOVDB_TRUE;
        }));
        ASSERT_TRUE(wait_for_profile(profile));
        ASSERT_TRUE(worker->render_thread_tasks.run_blocking([&]()
        {
            auto* settings = editor.impl->editor_scene->get_imgui_instance()->render_settings;
            EXPECT_FLOAT_EQ(settings->camera_state.position.x, 13.f);
            EXPECT_FLOAT_EQ(settings->camera_state.position.y, 17.f);
            EXPECT_FLOAT_EQ(settings->camera_state.position.z, 23.f);
            EXPECT_FLOAT_EQ(settings->camera_state.eye_distance_from_position, 42.f);
            EXPECT_FLOAT_EQ(settings->camera_config.fov_angle_y, 0.6f);
            EXPECT_FLOAT_EQ(settings->camera_config.orthographic_y, 19.f);
            EXPECT_EQ(settings->camera_config.is_orthographic, PNANOVDB_TRUE);
            EXPECT_EQ(settings->is_orthographic, PNANOVDB_TRUE);
            return PNANOVDB_TRUE;
        }));
    }
}

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
    editor.add_nanovdb_2(&editor, scene_token, object_token, nanovdb_array);
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

pnanovdb_compute_array_t* count_param_array_create(size_t size,
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
            editor.add_nanovdb_2(&editor, scene_token, object_token, nanovdb_array);
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
using CreateShaderContextFn = decltype(pnanovdb_compute_t::create_shader_context);
DispatchNanoVDBFn tracked_dispatch_nanovdb = nullptr;
InitShaderFn tracked_init_shader = nullptr;
CreateShaderContextFn tracked_create_shader_context = nullptr;
pnanovdb_shader_context_t* retry_shader_context = nullptr;
pnanovdb_compute_array_t* tracked_array_a = nullptr;
pnanovdb_compute_array_t* tracked_array_b = nullptr;
std::atomic<int> rendered_a{0}, rendered_b{0}, uploaded_a{0}, compiled_materials{0};
std::atomic<int> rejected_material_compiles{0};
std::atomic<int> retry_shader_compiles{0};

pnanovdb_shader_context_t* track_material_context(const char* filename)
{
    auto* context = tracked_create_shader_context(filename);
    retry_shader_context = std::strcmp(filename, "editor/wireframe.slang") == 0 ? context : nullptr;
    return context;
}

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
    if (shader == retry_shader_context)
        ++retry_shader_compiles;
    if (shader == retry_shader_context && rejected_material_compiles.load() > 0)
    {
        --rejected_material_compiles;
        return PNANOVDB_FALSE;
    }
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
        auto* compiler_inst = compiler.create_instance();
        ASSERT_NE(compiler_inst, nullptr);
        pnanovdb_compiler_settings_t settings{};
        pnanovdb_compiler_settings_init(&settings);
        settings.compile_target = PNANOVDB_COMPILE_TARGET_VULKAN;
        std::strcpy(settings.entry_point_name, "main");
        const auto shader_dir = std::filesystem::path(__FILE__).parent_path().parent_path() / "editor" / "shaders";
        bool shaders_compiled = true;
        for (const char* shader : { "flow_smoke.slang", "wireframe.slang" })
        {
            shaders_compiled &= compiler.compile_shader_from_file(
                compiler_inst, (shader_dir / shader).string().c_str(), &settings, nullptr) != PNANOVDB_FALSE;
        }
        compiler.destroy_instance(compiler_inst);
        ASSERT_TRUE(shaders_compiled);
        tracked_dispatch_nanovdb = compute.dispatch_shader_on_nanovdb_array;
        tracked_init_shader = compute.init_shader;
        tracked_create_shader_context = compute.create_shader_context;
        compute.dispatch_shader_on_nanovdb_array = count_material_dispatch;
        compute.init_shader = count_material_compile;
        compute.create_shader_context = track_material_context;
        retry_shader_context = nullptr;
        tracked_array_a = nullptr;
        tracked_array_b = nullptr;
        rendered_a = rendered_b = uploaded_a = compiled_materials = 0;
        rejected_material_compiles = 0;
        retry_shader_compiles = 0;
        tracking = true;
    }

    void TearDown() override
    {
        if (tracking)
        {
            editor.stop(&editor);
            compute.dispatch_shader_on_nanovdb_array = tracked_dispatch_nanovdb;
            compute.init_shader = tracked_init_shader;
            compute.create_shader_context = tracked_create_shader_context;
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

    bool add_tracked_grid(pnanovdb_editor_token_t* name,
                          const char* shader,
                          pnanovdb_compute_array_t*& source)
    {
        editor.add_nanovdb_2(&editor, scene_token, name, nanovdb_array);
        if (!pnanovdb_editor_test::map_shader_defaults(editor, compute, scene_token, name, shader))
            return false;
        editor.impl->scene_manager->with_object(scene_token, name,
            [&](pnanovdb_editor::SceneObject* obj) { source = obj->nanovdb_array(); });
        return true;
    }

    bool tracking = false;
};
}

TEST_F(EditorMaterialRenderTest, MixedShadersKeepObjectValuesAcrossFramesAndReload)
{
    auto* second = editor.get_token("wireframe");
    editor.add_nanovdb_2(&editor, scene_token, object_token, nanovdb_array);
    editor.add_nanovdb_2(&editor, scene_token, second, nanovdb_array);
    ASSERT_TRUE(pnanovdb_editor_test::map_shader_defaults(
        editor, compute, scene_token, object_token, "editor/flow_smoke.slang", [](pnanovdb_uint8_t* params)
        {
            const std::array<float, 2> values{ 7.75f, 4.f };
            std::memcpy(params, values.data(), sizeof(values));
            const auto layout = pnanovdb_editor::EditorSceneManager::load_shader_params_layout("editor/flow_smoke.slang");
            const auto shadow = std::find_if(layout.begin(), layout.end(),
                [](const pnanovdb_editor::ShaderParamLayout& field) { return field.name == "shadow_num_steps"; });
            ASSERT_NE(shadow, layout.end());
            const uint32_t steps = 1u;
            std::memcpy(params + shadow->offset, &steps, sizeof(steps));
        }));
    ASSERT_TRUE(pnanovdb_editor_test::map_shader_defaults(
        editor, compute, scene_token, second, "editor/wireframe.slang", [](pnanovdb_uint8_t* params)
        {
            const uint32_t highlight = 1u;
            std::memcpy(params, &highlight, sizeof(highlight));
        }));
    auto& manager = *editor.impl->scene_manager;
    const auto expect_material_values = [&]()
    {
        manager.with_object(scene_token, object_token, [](pnanovdb_editor::SceneObject* obj)
        {
            EXPECT_FLOAT_EQ(*static_cast<float*>(obj->shader_params()), 7.75f);
        });
        manager.with_object(scene_token, second, [](pnanovdb_editor::SceneObject* obj)
        {
            EXPECT_EQ(*static_cast<uint32_t*>(obj->shader_params()), 1u);
        });
    };
    manager.with_object(scene_token, object_token,
                         [](pnanovdb_editor::SceneObject* obj) { tracked_array_a = obj->nanovdb_array(); });
    manager.with_object(scene_token, second,
                         [](pnanovdb_editor::SceneObject* obj) { tracked_array_b = obj->nanovdb_array(); });
    ASSERT_TRUE(start_editor());
    ASSERT_TRUE(wait_for_frames(3, 3));
    expect_material_values();
    const int compile_count = compiled_materials.load();
    ASSERT_TRUE(wait_for_frames(rendered_a + 3, rendered_b + 3));
    EXPECT_EQ(compiled_materials.load(), compile_count);
    for (auto* selected : { object_token, second, object_token })
    {
        ASSERT_TRUE(editor.impl->editor_worker->render_thread_tasks.run_blocking([&]()
        {
            editor.impl->editor_scene->select_render_view(scene_token, selected);
            return PNANOVDB_TRUE;
        }));
        ASSERT_TRUE(wait_for_frames(rendered_a + 2, rendered_b + 2));
        EXPECT_EQ(compiled_materials.load(), compile_count) << "Selecting a cached material must not recompile it";
        expect_material_values();
    }

    const int before_reload = compiled_materials.load();
    ASSERT_TRUE(editor.impl->editor_worker->render_thread_tasks.run_blocking([&]()
    {
        auto* instance = editor.impl->editor_scene->get_imgui_instance();
        std::lock_guard<std::mutex> lock(instance->shader_reload_mutex);
        instance->shader_reload_requests.insert("wireframe.slang");
        return PNANOVDB_TRUE;
    }));
    ASSERT_TRUE(wait_for_frames(rendered_a + 2, rendered_b + 2));
    EXPECT_EQ(compiled_materials.load(), before_reload + 1) << "A shader reload must preserve unrelated contexts";
    expect_material_values();
    const int before_force_reload = compiled_materials.load();
    ASSERT_TRUE(editor.impl->editor_worker->render_thread_tasks.run_blocking([&]()
    {
        editor.impl->editor_scene->get_imgui_instance()->pending.update_shader = true;
        return PNANOVDB_TRUE;
    }));
    ASSERT_TRUE(wait_for_frames(rendered_a + 2, rendered_b + 2));
    EXPECT_EQ(compiled_materials.load(), before_force_reload + 2);
    expect_material_values();
}

TEST_F(EditorMaterialRenderTest, FileWatcherReloadsRelativeAndAbsoluteShaderAliases)
{
    auto linked_shader = std::filesystem::absolute(pnanovdb_shader::getCurrentDirectory()) /
                         "shaders" / "editor" / "wireframe.slang";
    if (!std::filesystem::exists(linked_shader))
    {
        linked_shader = std::filesystem::absolute(pnanovdb_shader::getShaderDir()) / "editor" / "wireframe.slang";
    }
    ASSERT_TRUE(std::filesystem::exists(linked_shader));
#if !defined(_WIN32)
    ASSERT_TRUE(std::filesystem::is_symlink(linked_shader.parent_path()));
#endif
    const auto source_shader = std::filesystem::canonical(linked_shader);
    auto* absolute_alias = editor.get_token("absolute_shader_alias");
    auto* unrelated = editor.get_token("unrelated_shader");
    ASSERT_TRUE(add_tracked_grid(object_token, "editor/wireframe.slang", tracked_array_a));
    ASSERT_TRUE(add_tracked_grid(absolute_alias, source_shader.string().c_str(), tracked_array_b));
    editor.add_nanovdb_2(&editor, scene_token, unrelated, nanovdb_array);
    ASSERT_TRUE(start_editor());
    ASSERT_TRUE(wait_for_frames(3, 3));

    auto notifying_compiler = compiler;
    notifying_compiler.compile_shader_from_file = [](pnanovdb_compiler_instance_t*, const char* filename,
                                                     pnanovdb_compiler_settings_t*, pnanovdb_bool_t* updated)
    {
        EXPECT_TRUE(std::filesystem::exists(filename));
        if (updated)
            *updated = PNANOVDB_TRUE;
        return PNANOVDB_TRUE;
    };
    auto callback = pnanovdb_editor::get_shader_recompile_callback(
        editor.impl->editor_scene->get_imgui_instance(), &notifying_compiler);
    for (const auto& path : { linked_shader, source_shader })
    {
        const int before_reload = compiled_materials.load();
        callback(path.string());
        ASSERT_TRUE(wait_for_frames(rendered_a + 3, rendered_b + 3));
        EXPECT_EQ(compiled_materials.load(), before_reload + 2)
            << "Reload must invalidate both path aliases and preserve the unrelated shader";
    }
}

TEST_F(EditorMaterialRenderTest, CachedFramesContinueWhileCompilerSettingsAreLocked)
{
    ASSERT_TRUE(add_tracked_grid(object_token, "editor/wireframe.slang", tracked_array_a));
    ASSERT_TRUE(start_editor());
    ASSERT_TRUE(wait_for_frames(3));
    auto* instance = editor.impl->editor_scene->get_imgui_instance();
    auto worker = editor.impl->editor_worker;
    int expected_frames = 0;
    int frames_before_unlock = 0;
    {
        std::lock_guard<std::mutex> lock(instance->compiler_settings_mutex);
        expected_frames = rendered_a.load() + 3;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (rendered_a < expected_frames && std::chrono::steady_clock::now() < deadline)
        {
            worker->render_thread_tasks.run_async([]() {});
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        frames_before_unlock = rendered_a.load();
    }
    EXPECT_GE(frames_before_unlock, expected_frames);
}

TEST_F(EditorMaterialRenderTest, FailedShaderInitializationRetriesOnLaterFrame)
{
    ASSERT_TRUE(add_tracked_grid(object_token, "editor/wireframe.slang", tracked_array_a));
    rejected_material_compiles = 1;
    ASSERT_TRUE(start_editor());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (retry_shader_compiles < 2 && std::chrono::steady_clock::now() < deadline)
    {
        ASSERT_TRUE(editor.impl->editor_worker->render_thread_tasks.run_blocking([]() { return PNANOVDB_TRUE; }));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ASSERT_GE(retry_shader_compiles.load(), 2) << "A failed shader must not remain cached";
    EXPECT_TRUE(wait_for_frames(3));
}

TEST_F(EditorMaterialRenderTest, FailedShaderDoesNotHideOtherObjects)
{
    auto* second = editor.get_token("valid_material");
    ASSERT_TRUE(add_tracked_grid(object_token, "editor/wireframe.slang", tracked_array_a));
    editor.add_nanovdb_2(&editor, scene_token, second, nanovdb_array);
    editor.impl->scene_manager->with_object(scene_token, second,
        [](pnanovdb_editor::SceneObject* obj) { tracked_array_b = obj->nanovdb_array(); });
    rejected_material_compiles = 1000000;
    ASSERT_TRUE(start_editor());
    EXPECT_TRUE(wait_for_frames(0, 3));
    EXPECT_GT(rejected_material_compiles.load(), 0);
    EXPECT_EQ(rendered_a.load(), 0);
}

TEST_F(EditorMaterialRenderTest, SourceRevisionInvalidatesAnUnchangedArrayAddress)
{
    ASSERT_TRUE(add_tracked_grid(object_token, "editor/wireframe.slang", tracked_array_a));
    ASSERT_TRUE(start_editor());
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
