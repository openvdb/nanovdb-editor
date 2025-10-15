// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/editor/Profiler.h

    \author Petra Hapalova

    \brief  Profiler window for the NanoVDB editor
*/

#pragma once

#include "ImguiInstance.h"

#include <string>
#include <unordered_map>
#include <vector>
#include <mutex>

namespace pnanovdb_editor
{

struct ProfilerEntryValue
{
    pnanovdb_compute_profiler_entry_t entry;
    pnanovdb_uint64_t capture_id;
    ProfilerEntryValue()
    {
    }
    ProfilerEntryValue(const pnanovdb_compute_profiler_entry_t& entry, pnanovdb_uint64_t capture_id)
        : entry(entry), capture_id(capture_id)
    {
    }
};

struct ProfilerEntry
{
    std::vector<ProfilerEntryValue> entries;
    ProfilerEntry()
    {
    }
};

class Profiler
{
public:
    static Profiler& getInstance()
    {
        static Profiler instance;
        return instance;
    }

    pnanovdb_compute_device_memory_stats_t* getMemoryStats()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return &memory_stats_;
    }

    bool render(bool* update_memory_stats, float delta_time);

    static void report_callback(void* userdata,
                                pnanovdb_uint64_t captureID,
                                pnanovdb_uint32_t numEntries,
                                pnanovdb_compute_profiler_entry_t* entries);

private:
    Profiler() = default;
    ~Profiler() = default;

    Profiler(const Profiler&) = delete;
    Profiler& operator=(const Profiler&) = delete;
    Profiler(Profiler&&) = delete;
    Profiler& operator=(Profiler&&) = delete;

    void render_profiler_table(pnanovdb_uint64_t capture_id,
                               const std::map<std::string, ProfilerEntry>& entries,
                               bool show_avg,
                               uint32_t history_depth);

    static int s_id;

    mutable std::mutex mutex_;

    pnanovdb_compute_device_memory_stats_t memory_stats_ = {};
    float memory_stats_timer_ = 0.f;

    std::atomic<bool> profiler_paused_ = true;
    bool show_averages_ = false;
    uint32_t history_depth_ = 10u;

    std::unordered_map<std::string, pnanovdb_uint64_t> profiler_capture_ids_;
    std::unordered_map<std::string, std::map<std::string, ProfilerEntry>> profiler_entries_;
};
} // namespace pnanovdb_editor
