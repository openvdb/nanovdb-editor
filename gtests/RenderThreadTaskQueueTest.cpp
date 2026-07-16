// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include "editor/Editor.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace
{

using namespace std::chrono_literals;
using pnanovdb_editor::RenderThreadTaskQueue;

bool wait_until_queued(RenderThreadTaskQueue& queue, size_t expected)
{
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline)
    {
        {
            std::lock_guard<std::mutex> lock(queue.mutex);
            if (queue.tasks.size() == expected)
                return true;
        }
        std::this_thread::yield();
    }
    return false;
}

TEST(RenderThreadTaskQueue, CloseWakesAllBlockingCallersWithoutRunningTheirTasks)
{
    RenderThreadTaskQueue queue;
    constexpr size_t caller_count = 8;
    std::atomic<size_t> run_count{ 0 };
    std::vector<std::future<pnanovdb_bool_t>> callers;
    callers.reserve(caller_count);

    for (size_t i = 0; i < caller_count; ++i)
    {
        callers.push_back(std::async(std::launch::async,
                                     [&queue, &run_count]()
                                     {
                                         return queue.run_blocking(
                                             [&run_count]()
                                             {
                                                 run_count.fetch_add(1, std::memory_order_relaxed);
                                                 return PNANOVDB_TRUE;
                                             });
                                     }));
    }

    const bool all_queued = wait_until_queued(queue, caller_count);
    queue.close();

    EXPECT_TRUE(all_queued);
    for (auto& caller : callers)
    {
        const std::future_status status = caller.wait_for(2s);
        EXPECT_EQ(status, std::future_status::ready);
        if (status == std::future_status::ready)
            EXPECT_EQ(caller.get(), PNANOVDB_FALSE);
    }
    EXPECT_EQ(run_count.load(std::memory_order_relaxed), 0u);
}

TEST(RenderThreadTaskQueue, ClosedQueueRejectsNewBlockingAndAsyncTasks)
{
    RenderThreadTaskQueue queue;
    std::atomic<int> run_count{ 0 };
    queue.close();

    EXPECT_EQ(queue.run_blocking(
                  [&run_count]()
                  {
                      run_count.fetch_add(1, std::memory_order_relaxed);
                      return PNANOVDB_TRUE;
                  }),
              PNANOVDB_FALSE);
    queue.run_async([&run_count]() { run_count.fetch_add(1, std::memory_order_relaxed); });
    queue.drain();

    EXPECT_EQ(run_count.load(std::memory_order_relaxed), 0);
}

TEST(RenderThreadTaskQueue, CloseDropsQueuedAsyncTasks)
{
    RenderThreadTaskQueue queue;
    std::atomic<int> run_count{ 0 };

    for (int i = 0; i < 8; ++i)
        queue.run_async([&run_count]() { run_count.fetch_add(1, std::memory_order_relaxed); });

    queue.close();
    queue.drain();

    EXPECT_EQ(run_count.load(std::memory_order_relaxed), 0);
}

TEST(RenderThreadTaskQueue, CloseDuringDrainLetsInFlightTaskCompleteExactlyOnce)
{
    RenderThreadTaskQueue queue;
    std::mutex gate_mutex;
    std::condition_variable gate_cv;
    bool task_started = false;
    bool release_task = false;
    std::atomic<int> run_count{ 0 };

    auto caller = std::async(std::launch::async,
                             [&queue, &gate_mutex, &gate_cv, &task_started, &release_task, &run_count]()
                             {
                                 return queue.run_blocking(
                                     [&gate_mutex, &gate_cv, &task_started, &release_task, &run_count]()
                                     {
                                         run_count.fetch_add(1, std::memory_order_relaxed);
                                         std::unique_lock<std::mutex> lock(gate_mutex);
                                         task_started = true;
                                         gate_cv.notify_one();
                                         gate_cv.wait(lock, [&release_task]() { return release_task; });
                                         return PNANOVDB_TRUE;
                                     });
                             });

    const bool queued = wait_until_queued(queue, 1);
    std::thread render_thread([&queue]() { queue.drain(); });
    bool started;
    {
        std::unique_lock<std::mutex> lock(gate_mutex);
        started = gate_cv.wait_for(lock, 2s, [&task_started]() { return task_started; });
    }

    queue.close();
    const std::future_status before_release = caller.wait_for(20ms);
    {
        std::lock_guard<std::mutex> lock(gate_mutex);
        release_task = true;
    }
    gate_cv.notify_one();
    render_thread.join();

    const std::future_status after_release = caller.wait_for(2s);
    EXPECT_TRUE(queued);
    EXPECT_TRUE(started);
    EXPECT_EQ(before_release, std::future_status::timeout);
    EXPECT_EQ(after_release, std::future_status::ready);
    if (after_release == std::future_status::ready)
        EXPECT_EQ(caller.get(), PNANOVDB_TRUE);
    EXPECT_EQ(run_count.load(std::memory_order_relaxed), 1);
}

TEST(RenderThreadTaskQueue, ThrowingTaskReportsFailureAndDoesNotStopDrain)
{
    RenderThreadTaskQueue queue;
    std::atomic<int> later_run_count{ 0 };
    auto throwing = std::async(
        std::launch::async, [&queue]()
        { return queue.run_blocking([]() -> pnanovdb_bool_t { throw std::runtime_error("task failure"); }); });
    const bool throwing_queued = wait_until_queued(queue, 1);
    auto later = std::async(std::launch::async,
                            [&queue, &later_run_count]()
                            {
                                return queue.run_blocking(
                                    [&later_run_count]()
                                    {
                                        later_run_count.fetch_add(1, std::memory_order_relaxed);
                                        return PNANOVDB_TRUE;
                                    });
                            });

    const bool both_queued = wait_until_queued(queue, 2);
    if (throwing_queued && both_queued)
        queue.drain();
    else
        queue.close();

    const std::future_status throwing_status = throwing.wait_for(2s);
    const std::future_status later_status = later.wait_for(2s);
    EXPECT_TRUE(throwing_queued);
    EXPECT_TRUE(both_queued);
    EXPECT_EQ(throwing_status, std::future_status::ready);
    EXPECT_EQ(later_status, std::future_status::ready);
    if (throwing_status == std::future_status::ready)
        EXPECT_EQ(throwing.get(), PNANOVDB_FALSE);
    if (later_status == std::future_status::ready)
        EXPECT_EQ(later.get(), PNANOVDB_TRUE);
    EXPECT_EQ(later_run_count.load(std::memory_order_relaxed), 1);
}

} // namespace
