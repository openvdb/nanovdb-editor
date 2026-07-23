// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include "editor/ShaderParams.h"

#include <atomic>
#include <thread>
#include <vector>

namespace
{

using pnanovdb_editor::ShaderParam;
using pnanovdb_editor::ShaderParams;

// All threads mutate/read the shared pool and param map concurrently on a single instance.
TEST(ShaderParamsConcurrency, ConcurrentPoolAndMapAccessIsRaceFree)
{
    ShaderParams params;

    constexpr int kThreads = 8;
    constexpr int kIterationsPerThread = 4000;

    // Half the threads churn the pool (push_back + clear); the rest also scan the param map, so the
    // recursive_mutex is exercised across shader_params_pool_ and params_map_ simultaneously.
    auto pool_churn = [&params, kIterationsPerThread](bool also_scan_map)
    {
        std::vector<char> seed(64, 0x5A);
        ShaderParam probe;
        probe.name = "concurrency_probe";
        probe.type = ImGuiDataType_Float;
        probe.size = sizeof(float);
        probe.num_elements = 1;

        for (int i = 0; i < kIterationsPerThread; ++i)
        {
            const size_t idx = params.allocatePoolArray(seed.size(), seed.data());
            params.deallocatePoolArray(idx);
            if (also_scan_map)
            {
                (void)params.findEquivalentParamPoolIndex(probe);
                params.clear_pending_array_for_shader("nonexistent_shader");
            }
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i)
    {
        threads.emplace_back(pool_churn, /*also_scan_map*/ (i % 2) == 0);
    }
    for (auto& t : threads)
    {
        t.join();
    }

    // The instance must still be usable and internally consistent after the concurrent burst.
    const size_t idx = params.allocatePoolArray(16, nullptr);
    EXPECT_NE(idx, SIZE_MAX);
    params.deallocatePoolArray(idx);
}

} // namespace
