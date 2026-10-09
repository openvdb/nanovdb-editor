// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <nanovdb_editor/putil/Compiler.h>
#include <nanovdb_editor/putil/Compute.h>
#include <nanovdb_editor/putil/Editor.h>

#include "editor/Editor.h"
#include "editor/EditorSceneManager.h"

#include <nanovdb/tools/CreatePrimitives.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

namespace
{
decltype(pnanovdb_compute_t::create_array) create_array_untracked = nullptr;
decltype(pnanovdb_compute_t::destroy_array) destroy_array_untracked = nullptr;
std::atomic<int> live_ui_arrays{ 0 };

pnanovdb_compute_array_t* count_ui_array_create(size_t size, pnanovdb_uint64_t count, const void* data)
{
    auto* array = create_array_untracked(size, count, data);
    if (array && size * count == PNANOVDB_COMPUTE_CONSTANT_BUFFER_MAX_SIZE)
        ++live_ui_arrays;
    return array;
}

void count_ui_array_destroy(pnanovdb_compute_array_t* array)
{
    if (array && array->element_size * array->element_count == PNANOVDB_COMPUTE_CONSTANT_BUFFER_MAX_SIZE)
        --live_ui_arrays;
    destroy_array_untracked(array);
}

void check_ui_parameter_sync(pnanovdb_editor_t& editor, pnanovdb_compute_t& compute)
{
    auto worker = editor.impl->editor_worker;
    ASSERT_NE(worker, nullptr);
    const auto run_task = [&](std::function<pnanovdb_bool_t()> callback)
    {
        auto task = worker->render_thread_tasks.enqueue_blocking(std::move(callback));
        if (!task)
            return false;
        std::unique_lock<std::mutex> lock(task->mutex);
        if (!task->cv.wait_for(lock, std::chrono::seconds(120), [&]() { return task->done; }))
        {
            lock.unlock();
            editor.stop(&editor);
            return false;
        }
        return task->result != PNANOVDB_FALSE;
    };

    bool ready = false;
    const auto ready_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    while (!ready && std::chrono::steady_clock::now() < ready_deadline)
    {
        ready = run_task([&]()
        {
            return editor.impl->shader_params && editor.impl->shader_name == "editor/wireframe.slang" &&
                   !worker->params_dirty.load();
        });
        if (!ready)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_TRUE(ready);
    ASSERT_TRUE(run_task([&]()
    {
        create_array_untracked = compute.create_array;
        destroy_array_untracked = compute.destroy_array;
        live_ui_arrays = 0;
        compute.create_array = count_ui_array_create;
        compute.destroy_array = count_ui_array_destroy;
        return PNANOVDB_TRUE;
    }));
    bool synchronized = true;
    for (pnanovdb_uint32_t update = 0; update < 128 && synchronized; ++update)
    {
        synchronized = run_task([&, update]()
        {
            *static_cast<pnanovdb_uint32_t*>(editor.impl->shader_params) = update % 2;
            worker->params_dirty.store(true);
            return PNANOVDB_TRUE;
        });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
        while (worker->params_dirty.load() && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        synchronized = synchronized && !worker->params_dirty.load();
    }
    EXPECT_TRUE(synchronized);
    ASSERT_TRUE(run_task([&]()
    {
        EXPECT_LE(live_ui_arrays.load(), 1) << "UI synchronization retains old parameter buffers";
        auto* snapshot = editor.impl->scene_manager->shader_params.get_compute_array_for_shader(
            editor.impl->shader_name, &compute);
        EXPECT_NE(snapshot, nullptr);
        if (snapshot)
        {
            EXPECT_EQ(*static_cast<const pnanovdb_uint32_t*>(snapshot->data), 1u);
            compute.destroy_array(snapshot);
        }
        return PNANOVDB_TRUE;
    }));
}

void run_headless_editor(bool streaming)
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
    pnanovdb_compute_array_t* nanovdb_array = compute.create_array(4u, sphere_grid.bufferSize() / 4u, sphere_grid.data());
    ASSERT_NE(nanovdb_array, nullptr) << "Failed to create nanovdb array";

    pnanovdb_editor_config_t cfg = {};
    cfg.ip_address = "127.0.0.1";
    cfg.port = 8080;
    cfg.headless = PNANOVDB_TRUE;
    cfg.streaming = streaming ? PNANOVDB_TRUE : PNANOVDB_FALSE;

    // Start, wait briefly, then stop
    editor.start(&editor, device, &cfg);

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

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    if (!streaming)
    {
        check_ui_parameter_sync(editor, compute);
    }
    editor.stop(&editor);
    if (create_array_untracked)
    {
        compute.create_array = create_array_untracked;
        compute.destroy_array = destroy_array_untracked;
        create_array_untracked = nullptr;
        destroy_array_untracked = nullptr;
    }

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

} // namespace

TEST(NanoVDBEditor, EditorStartStopHeadlessStreaming)
{
    run_headless_editor(true);
}

TEST(NanoVDBEditor, RepeatedUiParameterSyncKeepsAllocationsBounded)
{
    run_headless_editor(false);
}

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
