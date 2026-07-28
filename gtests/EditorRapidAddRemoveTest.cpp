// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <nanovdb_editor/putil/Compiler.h>
#include <nanovdb_editor/putil/Compute.h>
#include <nanovdb_editor/putil/Editor.h>

#include "editor/Editor.h" // pnanovdb_editor_impl_t / EditorWorker
#include "editor/PipelineTypes.h" // pnanovdb_pipeline_type_* enum
#include "GpuTestSupport.h"

#include <nanovdb/tools/CreatePrimitives.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace
{

using namespace std::chrono_literals;

constexpr auto kWorkerStartupTimeout = std::chrono::milliseconds(60000);
constexpr auto kStateTimeout = std::chrono::milliseconds(30000);

bool wait_until(std::function<bool()> predicate, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (predicate())
            return true;
        std::this_thread::sleep_for(1ms);
    }
    return predicate();
}

enum class SetupStatus
{
    Ok,
    Skip,
    Fatal
};

// RAII bundle that stands up a headless editor over a real device. setup() never calls GTEST_SKIP()
// itself (that macro cannot live in a non-void helper); it reports status so the TEST body decides.
struct EditorFixture
{
    pnanovdb_compiler_t compiler{};
    pnanovdb_compute_t compute{};
    pnanovdb_compute_device_manager_t* device_manager = nullptr;
    pnanovdb_compute_device_t* device = nullptr;
    pnanovdb_editor_t editor{};
    pnanovdb_editor_config_t cfg{};
    bool started = false;
    std::string message;

    SetupStatus setup(int port, const char* feature_label)
    {
        pnanovdb_compiler_load(&compiler);
        if (compiler.module == nullptr)
        {
            message = "Compiler module not available";
            return SetupStatus::Fatal;
        }
        pnanovdb_compute_load(&compute, &compiler);
        if (compute.module == nullptr)
        {
            message = "Failed to load compute module";
            return SetupStatus::Fatal;
        }

        device_manager = compute.device_interface.create_device_manager(PNANOVDB_FALSE);
        if (device_manager == nullptr)
        {
            message = "Failed to create compute device manager";
            return SetupStatus::Fatal;
        }

        pnanovdb_compute_physical_device_desc_t phys_desc{};
        if (!compute.device_interface.enumerate_devices(device_manager, 0u, &phys_desc))
        {
            message = "No Vulkan-compatible device available on this machine";
            return SetupStatus::Skip;
        }
        if (pnanovdb_editor_test::should_skip_on_software_renderer(phys_desc.device_name))
        {
            message = pnanovdb_editor_test::software_renderer_skip_reason(phys_desc.device_name, feature_label);
            return SetupStatus::Skip;
        }

        pnanovdb_compute_device_desc_t device_desc{};
        device = compute.device_interface.create_device(device_manager, &device_desc);
        if (device == nullptr)
        {
            message = "Failed to create compute device";
            return SetupStatus::Fatal;
        }

        pnanovdb_editor_load(&editor, &compute, &compiler);
        if (editor.module == nullptr || editor.impl == nullptr)
        {
            message = "Editor module failed to load";
            return SetupStatus::Fatal;
        }

        cfg.ip_address = "127.0.0.1";
        cfg.port = port;
        cfg.headless = PNANOVDB_TRUE;
        cfg.streaming = PNANOVDB_FALSE;
        return SetupStatus::Ok;
    }

    void start()
    {
        editor.start(&editor, device, &cfg);
        started = true;
    }

    ~EditorFixture()
    {
        if (editor.module)
        {
            if (started)
            {
                editor.stop(&editor);
                std::this_thread::sleep_for(100ms);
            }
            pnanovdb_editor_free(&editor);
        }
        if (device)
            compute.device_interface.destroy_device(device_manager, device);
        if (device_manager)
            compute.device_interface.destroy_device_manager(device_manager);
        if (compute.module)
            pnanovdb_compute_free(&compute);
        if (compiler.module)
            pnanovdb_compiler_free(&compiler);
    }
};

} // namespace

TEST(EditorRapidAddRemove, ManyRapidAddRemoveOperationsStayConsistent)
{
    EditorFixture fx;
    const SetupStatus status = fx.setup(8110, "rapid add/remove object stress (headless editor worker races teardown)");
    if (status == SetupStatus::Skip)
        GTEST_SKIP() << fx.message;
    ASSERT_EQ(status, SetupStatus::Ok) << fx.message;

    fx.start();

    std::shared_ptr<pnanovdb_editor::EditorWorker> worker = fx.editor.impl->editor_worker;
    ASSERT_NE(worker, nullptr) << "headless start() must create an EditorWorker";
    ASSERT_TRUE(wait_until([&]() { return !worker->is_starting.load(); }, kWorkerStartupTimeout))
        << "editor worker did not finish starting";

    auto sphere = nanovdb::tools::createLevelSetSphere<float>(4.0f);
    pnanovdb_compute_array_t* arr = fx.compute.create_array(4u, sphere.bufferSize() / 4u, sphere.data());
    ASSERT_NE(arr, nullptr);

    pnanovdb_editor_token_t* scene = fx.editor.get_token("rapid_scene");
    ASSERT_NE(scene, nullptr);

    // Phase 1: tight add-then-remove churn on a single object name. Adds are blocking (state is
    // observable immediately); removes are async and share the FIFO queue, so the next blocking add
    // cannot return until the prior remove has been drained.
    pnanovdb_editor_token_t* churn = fx.editor.get_token("rapid_churn_object");
    constexpr int kChurnIterations = 40;
    for (int i = 0; i < kChurnIterations; ++i)
    {
        fx.editor.add_nanovdb_2(&fx.editor, scene, churn, arr);
        EXPECT_EQ(fx.editor.get_pipeline(&fx.editor, scene, churn, pnanovdb_pipeline_stage_render),
                  pnanovdb_pipeline_type_nanovdb_render)
            << "object must be present right after the blocking add on iteration " << i;
        fx.editor.remove(&fx.editor, scene, churn);
    }
    // The final remove is async; wait for the queue to drain it.
    EXPECT_TRUE(wait_until(
        [&]()
        {
            return fx.editor.get_pipeline(&fx.editor, scene, churn, pnanovdb_pipeline_stage_render) ==
                   pnanovdb_pipeline_type_noop;
        },
        kStateTimeout))
        << "object still present after its final removal was drained";

    // Phase 2: add a batch of distinct objects, then remove them all rapidly. Verify every one is
    // eventually gone and nothing crashed / hung.
    constexpr int kBatch = 24;
    std::vector<pnanovdb_editor_token_t*> names;
    names.reserve(kBatch);
    for (int i = 0; i < kBatch; ++i)
    {
        pnanovdb_editor_token_t* name = fx.editor.get_token(("rapid_batch_" + std::to_string(i)).c_str());
        names.push_back(name);
        fx.editor.add_nanovdb_2(&fx.editor, scene, name, arr);
    }
    for (pnanovdb_editor_token_t* name : names)
        fx.editor.remove(&fx.editor, scene, name);

    EXPECT_TRUE(wait_until(
        [&]()
        {
            for (pnanovdb_editor_token_t* name : names)
            {
                if (fx.editor.get_pipeline(&fx.editor, scene, name, pnanovdb_pipeline_stage_render) !=
                    pnanovdb_pipeline_type_noop)
                    return false;
            }
            return true;
        },
        kStateTimeout))
        << "not all batch objects were removed after the rapid remove burst";

    fx.compute.destroy_array(arr);
}

TEST(EditorRapidAddRemove, ObjectAddedDuringStartupIsAppliedAndSynced)
{
    EditorFixture fx;
    const SetupStatus status = fx.setup(8111, "startup add deferral parity (headless editor worker races teardown)");
    if (status == SetupStatus::Skip)
        GTEST_SKIP() << fx.message;
    ASSERT_EQ(status, SetupStatus::Ok) << fx.message;

    auto sphere = nanovdb::tools::createLevelSetSphere<float>(6.0f);
    pnanovdb_compute_array_t* arr = fx.compute.create_array(4u, sphere.bufferSize() / 4u, sphere.data());
    ASSERT_NE(arr, nullptr);

    pnanovdb_editor_token_t* scene = fx.editor.get_token("startup_scene");
    pnanovdb_editor_token_t* name = fx.editor.get_token("startup_object");
    ASSERT_NE(scene, nullptr);
    ASSERT_NE(name, nullptr);

    // Start the render loop and immediately add an object while the worker starts. The add operation
    // must copy the input without waiting for the render thread. The render view must synchronize later.
    fx.start();
    fx.editor.add_nanovdb_3(
        &fx.editor, scene, name, arr, pnanovdb_pipeline_type_noop, pnanovdb_pipeline_type_nanovdb_render);

    EXPECT_TRUE(wait_until(
        [&]()
        {
            return fx.editor.get_pipeline(&fx.editor, scene, name, pnanovdb_pipeline_stage_render) ==
                   pnanovdb_pipeline_type_nanovdb_render;
        },
        kStateTimeout))
        << "object added during startup was never applied to the scene manager";

    // Parity check: the added object must be synced into the render view (this is what the old
    // startup_view deferral guaranteed), so the active render array becomes non-null.
    EXPECT_TRUE(wait_until([&]() { return fx.editor.impl->nanovdb_array != nullptr; }, kStateTimeout))
        << "startup-added object was applied but never synced into the render view";

    fx.compute.destroy_array(arr);
}

TEST(EditorRapidAddRemove, MutationsRacingShutdownAreSafe)
{
    EditorFixture fx;
    const SetupStatus status = fx.setup(8112, "mutations racing worker shutdown (headless editor worker teardown)");
    if (status == SetupStatus::Skip)
        GTEST_SKIP() << fx.message;
    ASSERT_EQ(status, SetupStatus::Ok) << fx.message;

    fx.start();

    std::shared_ptr<pnanovdb_editor::EditorWorker> worker = fx.editor.impl->editor_worker;
    ASSERT_NE(worker, nullptr) << "headless start() must create an EditorWorker";
    ASSERT_TRUE(wait_until([&]() { return !worker->is_starting.load(); }, kWorkerStartupTimeout))
        << "editor worker did not finish starting";

    auto sphere = nanovdb::tools::createLevelSetSphere<float>(5.0f);
    pnanovdb_compute_array_t* arr = fx.compute.create_array(4u, sphere.bufferSize() / 4u, sphere.data());
    ASSERT_NE(arr, nullptr);

    pnanovdb_editor_token_t* scene = fx.editor.get_token("shutdown_race_scene");
    ASSERT_NE(scene, nullptr);

    std::atomic<bool> stop_mutating{ false };
    std::thread mutator(
        [&]()
        {
            int i = 0;
            while (!stop_mutating.load(std::memory_order_acquire))
            {
                pnanovdb_editor_token_t* name = fx.editor.get_token(("sd_object_" + std::to_string(i++ % 8)).c_str());
                // add is blocking (rejected with FALSE once the queue closes, never hangs);
                // remove is fire-and-forget (dropped after close).
                fx.editor.add_nanovdb_2(&fx.editor, scene, name, arr);
                fx.editor.remove(&fx.editor, scene, name);
            }
        });

    // Let a few mutations land, then tear the worker down underneath the mutator.
    std::this_thread::sleep_for(50ms);
    fx.editor.stop(&fx.editor);

    stop_mutating.store(true, std::memory_order_release);
    mutator.join();

    fx.compute.destroy_array(arr);

    EXPECT_EQ(fx.editor.impl->editor_worker, nullptr) << "worker must be released after stop() joins the render thread";
}
