// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include "editor/PipelineRuntime.h"
#include "editor/PipelineTypes.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

namespace
{

struct TrackedResource
{
    static std::atomic<int> s_live;
    TrackedResource()
    {
        s_live.fetch_add(1, std::memory_order_relaxed);
    }
    ~TrackedResource()
    {
        s_live.fetch_sub(1, std::memory_order_relaxed);
    }
};

std::atomic<int> TrackedResource::s_live{ 0 };

class LifecycleProbeWorker : public pnanovdb_editor::AsyncWorker
{
public:
    LifecycleProbeWorker() = default;
    ~LifecycleProbeWorker() override
    {
        release();
    }

    pnanovdb_pipeline_type_t pipeline_type() const override
    {
        return pnanovdb_pipeline_type_noop;
    }

    void start_slow_task(int sleep_ms)
    {
        m_task_started.store(false, std::memory_order_relaxed);
        m_task_finished.store(false, std::memory_order_relaxed);
        m_pending = nullptr;
        m_enqueued = true;
        m_task_id = m_worker->enqueue(
            [this, sleep_ms]() -> bool
            {
                m_task_started.store(true, std::memory_order_release);
                std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
                m_pending = new TrackedResource();
                m_task_finished.store(true, std::memory_order_release);
                return true;
            });
    }

    bool handle_completion() override
    {
        if (!is_completed())
        {
            return false;
        }
        m_completion_calls.fetch_add(1, std::memory_order_relaxed);
        delete m_pending;
        m_pending = nullptr;
        finish_task();
        return true;
    }

    bool wait_until_started(int timeout_ms)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (!m_task_started.load(std::memory_order_acquire))
        {
            if (std::chrono::steady_clock::now() > deadline)
            {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return true;
    }

    bool task_finished() const
    {
        return m_task_finished.load(std::memory_order_acquire);
    }
    int completion_calls() const
    {
        return m_completion_calls.load(std::memory_order_relaxed);
    }
    int release_resources_calls() const
    {
        return m_release_resources_calls.load(std::memory_order_relaxed);
    }
    const TrackedResource* pending() const
    {
        return m_pending;
    }

protected:
    void release_resources() override
    {
        m_release_resources_calls.fetch_add(1, std::memory_order_relaxed);
        delete m_pending;
        m_pending = nullptr;
    }

private:
    std::atomic<bool> m_task_started{ false };
    std::atomic<bool> m_task_finished{ false };
    std::atomic<int> m_completion_calls{ 0 };
    std::atomic<int> m_release_resources_calls{ 0 };
    TrackedResource* m_pending = nullptr;
};

class PipelineWorkerShutdownTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_EQ(TrackedResource::s_live.load(), 0) << "leaked TrackedResource before test started";
    }
    void TearDown() override
    {
        EXPECT_EQ(TrackedResource::s_live.load(), 0) << "TrackedResource leaked across the test";
    }
};

} // namespace

TEST_F(PipelineWorkerShutdownTest, ReleaseWhileTaskInFlightJoinsWithoutStaleCompletion)
{
    auto worker = std::make_unique<LifecycleProbeWorker>();

    worker->start_slow_task(/*sleep_ms=*/200);
    ASSERT_TRUE(worker->wait_until_started(/*timeout_ms=*/5000)) << "worker task never started";
    EXPECT_TRUE(worker->is_running()) << "task should still be in flight before release()";

    worker->release();

    EXPECT_TRUE(worker->task_finished()) << "release() must join the running task, not abandon it";
    EXPECT_EQ(worker->completion_calls(), 0) << "shutdown must not run the completion path against torn-down state";
    EXPECT_GE(worker->release_resources_calls(), 1) << "release() must reclaim worker-thread resources";
    EXPECT_EQ(worker->pending(), nullptr) << "pending output should be freed on release()";
    EXPECT_FALSE(worker->is_running());

    worker.reset();
}

TEST_F(PipelineWorkerShutdownTest, DestructorWhileTaskInFlightIsClean)
{
    auto worker = std::make_unique<LifecycleProbeWorker>();
    worker->start_slow_task(/*sleep_ms=*/200);
    ASSERT_TRUE(worker->wait_until_started(/*timeout_ms=*/5000)) << "worker task never started";
    EXPECT_TRUE(worker->is_running());

    worker.reset();

    EXPECT_EQ(TrackedResource::s_live.load(), 0) << "destructor must reclaim the in-flight worker output";
}

TEST_F(PipelineWorkerShutdownTest, ReleaseIsIdempotent)
{
    auto worker = std::make_unique<LifecycleProbeWorker>();
    worker->start_slow_task(/*sleep_ms=*/50);
    ASSERT_TRUE(worker->wait_until_started(/*timeout_ms=*/5000));

    worker->release();
    const int first_release_calls = worker->release_resources_calls();
    EXPECT_GE(first_release_calls, 1);

    worker->release();
    EXPECT_EQ(worker->release_resources_calls(), first_release_calls)
        << "second release() must be a no-op (release_resources only runs once)";

    worker.reset();
    EXPECT_EQ(TrackedResource::s_live.load(), 0);
}

TEST_F(PipelineWorkerShutdownTest, NormalCompletionTransfersAndFinishes)
{
    auto worker = std::make_unique<LifecycleProbeWorker>();
    worker->start_slow_task(/*sleep_ms=*/0);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (worker->is_running())
    {
        ASSERT_LT(std::chrono::steady_clock::now(), deadline) << "task never completed";
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    EXPECT_TRUE(worker->handle_completion());
    EXPECT_EQ(worker->completion_calls(), 1);
    EXPECT_EQ(worker->pending(), nullptr);

    worker.reset();
    EXPECT_EQ(TrackedResource::s_live.load(), 0);
}

TEST_F(PipelineWorkerShutdownTest, ContainerOwningInFlightWorkersReleasesAllOnDestruction)
{
    std::vector<std::unique_ptr<pnanovdb_editor::AsyncWorker>> workers;
    for (int i = 0; i < 4; ++i)
    {
        auto w = std::make_unique<LifecycleProbeWorker>();
        w->start_slow_task(/*sleep_ms=*/150);
        ASSERT_TRUE(w->wait_until_started(/*timeout_ms=*/5000)) << "worker " << i << " never started";
        workers.push_back(std::move(w));
    }

    // Destroy the container while all four workers still have tasks in flight.
    workers.clear();

    EXPECT_EQ(TrackedResource::s_live.load(), 0) << "destroying the owning container must reclaim every worker output";
}
