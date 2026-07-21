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
#include <string>
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

TEST(RenderThreadTaskQueue, ThrowingTaskLogsTelemetryToStderr)
{
    RenderThreadTaskQueue queue;
    testing::internal::CaptureStderr();

    auto caller = std::async(
        std::launch::async, [&queue]()
        { return queue.run_blocking([]() -> pnanovdb_bool_t { throw std::runtime_error("boom-telemetry-marker"); }); });
    ASSERT_TRUE(wait_until_queued(queue, 1));
    queue.drain();

    const std::future_status status = caller.wait_for(2s);
    ASSERT_EQ(status, std::future_status::ready);
    EXPECT_EQ(caller.get(), PNANOVDB_FALSE);

    const std::string err = testing::internal::GetCapturedStderr();
    EXPECT_NE(err.find("render thread task threw"), std::string::npos) << "stderr was: " << err;
    EXPECT_NE(err.find("boom-telemetry-marker"), std::string::npos) << "stderr was: " << err;
}

TEST(RenderThreadTaskQueue, ThrowingNonStdExceptionLogsTelemetryToStderr)
{
    RenderThreadTaskQueue queue;
    testing::internal::CaptureStderr();

    auto caller =
        std::async(std::launch::async, [&queue]() { return queue.run_blocking([]() -> pnanovdb_bool_t { throw 42; }); });
    ASSERT_TRUE(wait_until_queued(queue, 1));
    queue.drain();

    const std::future_status status = caller.wait_for(2s);
    ASSERT_EQ(status, std::future_status::ready);
    EXPECT_EQ(caller.get(), PNANOVDB_FALSE);

    const std::string err = testing::internal::GetCapturedStderr();
    EXPECT_NE(err.find("unknown exception"), std::string::npos) << "stderr was: " << err;
}

TEST(RenderThreadTaskQueue, ThrowingAsyncTaskLogsTelemetryToStderr)
{
    RenderThreadTaskQueue queue;
    testing::internal::CaptureStderr();

    queue.run_async([]() { throw std::runtime_error("async-telemetry-marker"); });
    ASSERT_TRUE(wait_until_queued(queue, 1));
    queue.drain();

    const std::string err = testing::internal::GetCapturedStderr();
    EXPECT_NE(err.find("render thread task threw"), std::string::npos) << "stderr was: " << err;
    EXPECT_NE(err.find("async-telemetry-marker"), std::string::npos) << "stderr was: " << err;
}

TEST(RenderThreadTaskQueue, EnqueueAfterShutdownIsRejectedAndCallerWoken)
{
    RenderThreadTaskQueue queue;
    std::atomic<int> run_count{ 0 };

    queue.close(); // render loop has gone away

    // enqueue_blocking must not queue anything and must report rejection via a null handle.
    auto handle = queue.enqueue_blocking(
        [&run_count]()
        {
            run_count.fetch_add(1, std::memory_order_relaxed);
            return PNANOVDB_TRUE;
        });
    EXPECT_EQ(handle, nullptr);
    EXPECT_EQ(RenderThreadTaskQueue::wait_blocking(handle), PNANOVDB_FALSE);

    // A blocking caller entering after shutdown returns FALSE promptly (no hang).
    auto caller = std::async(std::launch::async,
                             [&queue, &run_count]()
                             {
                                 return queue.run_blocking(
                                     [&run_count]()
                                     {
                                         run_count.fetch_add(1, std::memory_order_relaxed);
                                         return PNANOVDB_TRUE;
                                     });
                             });
    ASSERT_EQ(caller.wait_for(2s), std::future_status::ready) << "run_blocking after close must not hang";
    EXPECT_EQ(caller.get(), PNANOVDB_FALSE);

    // Async work is silently dropped after shutdown.
    queue.run_async([&run_count]() { run_count.fetch_add(1, std::memory_order_relaxed); });
    queue.drain();

    EXPECT_EQ(run_count.load(std::memory_order_relaxed), 0);
}

// Rapidly enqueue from many threads while shutdown is triggered concurrently, mirroring
// external add/remove calls racing editor stop(). No caller may hang, the blocking result
// must always match whether the task actually ran, and every enqueue attempt made strictly
// after close() must be rejected (task never runs).
TEST(RenderThreadTaskQueue, EnqueueDuringShutdownNeverHangsAndPostCloseEnqueuesAreRejected)
{
    RenderThreadTaskQueue queue;
    constexpr int producer_count = 8;
    std::atomic<bool> stop_producers{ false };
    std::atomic<int> mismatch{ 0 };

    std::thread render_thread(
        [&queue, &stop_producers]()
        {
            while (!stop_producers.load(std::memory_order_acquire))
            {
                queue.drain();
                std::this_thread::yield();
            }
            queue.drain();
        });

    std::vector<std::thread> producers;
    producers.reserve(producer_count);
    for (int p = 0; p < producer_count; ++p)
    {
        producers.emplace_back(
            [&queue, &stop_producers, &mismatch]()
            {
                while (!stop_producers.load(std::memory_order_acquire))
                {
                    bool ran = false;
                    const pnanovdb_bool_t r = queue.run_blocking(
                        [&ran]()
                        {
                            ran = true;
                            return PNANOVDB_TRUE;
                        });
                    // Core contract: the blocking result reflects whether the task ran.
                    // A rejected/failed task must never have executed its body.
                    if ((r == PNANOVDB_TRUE) != ran)
                        mismatch.fetch_add(1, std::memory_order_relaxed);
                }
            });
    }

    std::this_thread::sleep_for(20ms);
    queue.close();

    stop_producers.store(true, std::memory_order_release);
    for (auto& producer : producers)
        producer.join();
    render_thread.join();

    EXPECT_EQ(mismatch.load(std::memory_order_relaxed), 0) << "blocking result did not match run status";

    // With the queue fully closed and drained, any further enqueue is rejected outright.
    std::atomic<int> post_close_runs{ 0 };
    EXPECT_EQ(queue.enqueue_blocking(
                  [&post_close_runs]()
                  {
                      post_close_runs.fetch_add(1, std::memory_order_relaxed);
                      return PNANOVDB_TRUE;
                  }),
              nullptr);
    queue.run_async([&post_close_runs]() { post_close_runs.fetch_add(1, std::memory_order_relaxed); });
    queue.drain();
    EXPECT_EQ(post_close_runs.load(std::memory_order_relaxed), 0) << "task enqueued after close must never run";
}

TEST(RenderThreadTaskQueue, ConcurrentBlockingCallersMatchRunResultContractAcrossClose)
{
    RenderThreadTaskQueue queue;
    constexpr int producer_count = 8;
    constexpr int per_producer = 250;
    constexpr int total = producer_count * per_producer;

    std::vector<std::atomic<int>> run_counts(total);
    std::vector<std::atomic<int>> results(total); // -1 = not returned, 0 = FALSE, 1 = TRUE
    for (int i = 0; i < total; ++i)
    {
        run_counts[i].store(0, std::memory_order_relaxed);
        results[i].store(-1, std::memory_order_relaxed);
    }

    std::atomic<bool> keep_draining{ true };
    std::thread render_thread(
        [&queue, &keep_draining]()
        {
            while (keep_draining.load(std::memory_order_relaxed))
            {
                queue.drain();
                std::this_thread::yield();
            }
            queue.drain(); // final sweep for anything enqueued right before shutdown
        });

    std::vector<std::thread> producers;
    producers.reserve(producer_count);
    for (int p = 0; p < producer_count; ++p)
    {
        producers.emplace_back(
            [&queue, &run_counts, &results, p, per_producer]()
            {
                for (int i = 0; i < per_producer; ++i)
                {
                    const int id = p * per_producer + i;
                    const pnanovdb_bool_t r = queue.run_blocking(
                        [&run_counts, id]()
                        {
                            run_counts[id].fetch_add(1, std::memory_order_relaxed);
                            return PNANOVDB_TRUE;
                        });
                    results[id].store(r == PNANOVDB_TRUE ? 1 : 0, std::memory_order_relaxed);
                }
            });
    }

    // Close mid-flight so some tasks are accepted/run and some are rejected/dropped.
    std::this_thread::sleep_for(15ms);
    queue.close();

    for (auto& producer : producers)
        producer.join();
    keep_draining.store(false, std::memory_order_relaxed);
    render_thread.join();

    for (int id = 0; id < total; ++id)
    {
        const int runs = run_counts[id].load(std::memory_order_relaxed);
        const int result = results[id].load(std::memory_order_relaxed);
        EXPECT_LE(runs, 1) << "task " << id << " ran more than once";
        EXPECT_NE(result, -1) << "producer for task " << id << " never returned (possible hang)";
        if (result == 1)
            EXPECT_EQ(runs, 1) << "task " << id << " reported success but did not run exactly once";
        if (result == 0)
            EXPECT_EQ(runs, 0) << "task " << id << " reported failure but ran";
    }
}

TEST(RenderThreadTaskQueue, ConcurrentAsyncEnqueueAcrossCloseNeverRunsAfterSweep)
{
    RenderThreadTaskQueue queue;
    constexpr int producer_count = 8;
    constexpr int per_producer = 400;

    std::atomic<int> run_count{ 0 };
    std::atomic<bool> closed{ false };
    std::atomic<int> ran_after_close{ 0 };

    std::atomic<bool> keep_draining{ true };
    std::thread render_thread(
        [&queue, &keep_draining]()
        {
            while (keep_draining.load(std::memory_order_relaxed))
            {
                queue.drain();
                std::this_thread::yield();
            }
            queue.drain();
        });

    std::vector<std::thread> producers;
    producers.reserve(producer_count);
    for (int p = 0; p < producer_count; ++p)
    {
        producers.emplace_back(
            [&queue, &run_count, &closed, &ran_after_close, per_producer]()
            {
                for (int i = 0; i < per_producer; ++i)
                {
                    queue.run_async(
                        [&run_count, &closed, &ran_after_close]()
                        {
                            run_count.fetch_add(1, std::memory_order_relaxed);
                            if (closed.load(std::memory_order_acquire))
                                ran_after_close.fetch_add(1, std::memory_order_relaxed);
                        });
                }
            });
    }

    std::this_thread::sleep_for(10ms);

    for (auto& producer : producers)
        producer.join();

    // Close, then stop draining and do a final sweep. Any task enqueued after close() is rejected,
    // and the render thread's final drain() has no accepted-but-unrun work left.
    queue.close();
    keep_draining.store(false, std::memory_order_relaxed);
    render_thread.join();
    closed.store(true, std::memory_order_release);

    // A second drain after close must be a no-op: nothing can run once the queue is closed and swept.
    queue.drain();

    EXPECT_EQ(ran_after_close.load(std::memory_order_relaxed), 0)
        << "no async task may run after close() swept the queue";
    EXPECT_LE(run_count.load(std::memory_order_relaxed), producer_count * per_producer);
}

TEST(RenderThreadTaskQueue, FreshQueueAfterCloseModelsWorkerRestartWithoutStaleState)
{
    // First lifecycle: a queued task runs when the render loop drains, then close() on teardown.
    RenderThreadTaskQueue first;
    std::atomic<int> first_runs{ 0 };
    first.run_async([&first_runs]() { first_runs.fetch_add(1, std::memory_order_relaxed); });
    first.drain(); // models the render loop processing queued work
    EXPECT_EQ(first_runs.load(std::memory_order_relaxed), 1);

    first.close(); // models stop(): reject further work

    // A closed queue stays closed: newly enqueued work is dropped and never runs, even across a drain,
    // and a blocking caller returns immediately with failure instead of parking on a dead loop.
    first.run_async([&first_runs]() { first_runs.fetch_add(1, std::memory_order_relaxed); });
    EXPECT_EQ(first.run_blocking([]() { return PNANOVDB_TRUE; }), PNANOVDB_FALSE);
    first.drain();
    EXPECT_EQ(first_runs.load(std::memory_order_relaxed), 1) << "a closed queue must not run new work (no reopen)";

    // Second lifecycle: a *new* queue (as a restarted worker would own) starts fresh and accepting,
    // completely unaffected by the previously-closed queue -> no stale state persists across the
    // start -> stop -> start transition.
    RenderThreadTaskQueue second;
    std::atomic<int> second_runs{ 0 };
    second.run_async([&second_runs]() { second_runs.fetch_add(1, std::memory_order_relaxed); });
    second.drain();
    EXPECT_EQ(second_runs.load(std::memory_order_relaxed), 1)
        << "a freshly constructed queue must accept work after a prior queue was closed";
}

} // namespace
