// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/editor/Profiler.cpp

    \author Petra Hapalova

    \brief  Profiler window for the NanoVDB editor
*/

#include "Profiler.h"

#include <imgui.h>

namespace pnanovdb_editor
{

int Profiler::s_id = 0;

Profiler& Profiler::getInstance()
{
    static Profiler instance;
    return instance;
}

bool Profiler::render(bool* update_memory_stats, float delta_time)
{
    ImGuiIO& io = ImGui::GetIO();
    // ImGui::Text("%.1f FPS", io.Framerate);
    ImGui::Text("%.3f CPU ms/frame", delta_time * 1000.0f);

    // update memory stats timer once per second
    {
        std::lock_guard<std::mutex> lock(mutex_);
        memory_stats_timer_ += delta_time;
        if (memory_stats_timer_ > 1.0f)
        {
            *update_memory_stats = true;
            memory_stats_timer_ = 0.0f;
        }
    }

    pnanovdb_compute_device_memory_stats_t stats;
    bool show_avg = false;
    uint32_t history_depth = 0u;
    std::vector<std::string> profiler_names;
    bool has_any_data = false;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        stats = memory_stats_;
        show_avg = show_averages_;
        history_depth = history_depth_;

        for (const auto& device_entry : profiler_entries_)
        {
            profiler_names.push_back(device_entry.first);
            has_any_data = has_any_data || !device_entry.second.empty();
        }
    }

    // TODO: can have a table per device
    // if (ImGui::CollapsingHeader("Device", ImGuiTreeNodeFlags_DefaultOpen))
    {
        if (ImGui::CollapsingHeader("Memory Usage", ImGuiTreeNodeFlags_DefaultOpen))
        {
            if (ImGui::BeginTable("MemoryStatsTable", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
            {
                ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Size (MB)", ImGuiTableColumnFlags_WidthFixed, 100.0f);
                ImGui::TableHeadersRow();

                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted("Device");
                ImGui::TableNextColumn();
                ImGui::Text("%.2f", stats.device_memory_bytes / (1024.0f * 1024.0f));

                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted("Upload");
                ImGui::TableNextColumn();
                ImGui::Text("%.2f", stats.upload_memory_bytes / (1024.0f * 1024.0f));

                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted("Readback");
                ImGui::TableNextColumn();
                ImGui::Text("%.2f", stats.readback_memory_bytes / (1024.0f * 1024.0f));

                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted("Other");
                ImGui::TableNextColumn();
                ImGui::Text("%.2f", stats.other_memory_bytes / (1024.0f * 1024.0f));

                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted("Total");
                ImGui::TableNextColumn();
                ImGui::Text("%.2f", (stats.device_memory_bytes + stats.upload_memory_bytes +
                                     stats.readback_memory_bytes + stats.other_memory_bytes) /
                                        (1024.0f * 1024.0f));

                ImGui::EndTable();
            }
        }

        ImGui::Separator();

        if (has_any_data)
        {
            if (ImGui::Button(profiler_paused_ ? "Resume" : "Pause"))
            {
                std::lock_guard<std::mutex> lock(mutex_);
                profiler_paused_ = !profiler_paused_;
            }

            ImGui::SameLine();
            if (ImGui::Button("Clear"))
            {
                std::lock_guard<std::mutex> lock(mutex_);
                profiler_capture_ids_.clear();
                profiler_entries_.clear();
            }

            ImGui::SameLine();
            ImGui::SetNextItemWidth(50.f);
            int temp_history_depth = (int)history_depth;
            if (ImGui::DragInt("History Depth", &temp_history_depth, 1.f, 0, 1000))
            {
                std::lock_guard<std::mutex> lock(mutex_);
                history_depth_ = (uint32_t)temp_history_depth;
            }

            const char* label_averages = "Show Averages";

            float window_width = ImGui::GetWindowWidth();
            float checkbox_width = ImGui::CalcTextSize(label_averages).x + ImGui::GetStyle().FramePadding.x * 2 + 20.0f;
            ImGui::SameLine(window_width - checkbox_width);

            bool temp_show_averages = show_avg;
            if (ImGui::Checkbox(label_averages, &temp_show_averages))
            {
                std::lock_guard<std::mutex> lock(mutex_);
                show_averages_ = temp_show_averages;
            }

            for (std::string profile_name : profiler_names)
            {
                if (ImGui::CollapsingHeader(profile_name.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
                {
                    std::map<std::string, ProfilerEntry> entries_copy;

                    pnanovdb_uint64_t capture_id = 0u;
                    {
                        std::lock_guard<std::mutex> lock(mutex_);

                        if (profiler_capture_ids_.find(profile_name) != profiler_capture_ids_.end())
                        {
                            capture_id = profiler_capture_ids_[profile_name];
                        }

                        if (profiler_entries_.find(profile_name) != profiler_entries_.end())
                        {
                            entries_copy = profiler_entries_[profile_name];
                        }
                    }

                    render_profiler_table(capture_id, entries_copy, show_avg, history_depth);
                }
            }
        }
        else
        {
            ImGui::Text("No profiler data available");
            if (ImGui::Button("Start Profiling"))
            {
                profiler_paused_ = false;
            }
        }
    }
    return true;
}

void Profiler::render_profiler_table(
    pnanovdb_uint64_t capture_id,
    const std::map<std::string, ProfilerEntry>& entries,
    bool show_avg,
    uint32_t history_depth)
{
    if (show_avg)
    {
        if (ImGui::BeginTable("ProfilerTable", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
        {
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Count", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableSetupColumn("CPU (ms)", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableSetupColumn("GPU (ms)", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableHeadersRow();

            for (const auto& pair : entries)
            {
                const auto& label = pair.first;
                const auto& entry = pair.second;

                float cpu_sum = 0.0f;
                float gpu_sum = 0.0f;
                size_t count = 0u;
                for (size_t idx = entry.entries.size() - 1u; idx < entry.entries.size(); idx--)
                {
                    cpu_sum += entry.entries[idx].entry.cpu_delta_time * 1000.0f;
                    gpu_sum += entry.entries[idx].entry.gpu_delta_time * 1000.0f;
                    count++;
                }
                cpu_sum = count == 0u ? 0.f : cpu_sum / (float)count;
                gpu_sum = count == 0u ? 0.f : gpu_sum / (float)count;

                if (cpu_sum == 0.f && gpu_sum == 0.f)
                {
                    continue;
                }

                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%s", label.c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%zu", count);
                ImGui::TableNextColumn();
                ImGui::Text("%.3f", cpu_sum);
                ImGui::TableNextColumn();
                ImGui::Text("%.3f", gpu_sum);
            }
            ImGui::EndTable();
        }

        return;
    }

    std::vector<pnanovdb_uint64_t> capture_hash(history_depth);
    for (auto& hash : capture_hash)
    {
        hash = 0llu;
    }
    float global_total_cpu_time = 0.0f;
    float global_total_gpu_time = 0.0f;
    float total_cpu_time = 0.0f;
    float total_gpu_time = 0.0f;

    for (uint64_t capture_id_offset = 0llu; capture_id_offset < history_depth; capture_id_offset++)
    {
        if (capture_id < capture_id_offset)
        {
            break;
        }
        uint64_t cmp_capture_id = capture_id - capture_id_offset;

        // compute hash, check for redundancy
        uint32_t hash_idx = 0u;
        uint64_t match_count = 0llu;
        for (const auto& pair : entries)
        {
            const auto& label = pair.first;
            const auto& entry = pair.second;

            for (size_t idx = entry.entries.size() - 1u; idx < entry.entries.size(); idx--)
            {
                if (entry.entries[idx].capture_id == cmp_capture_id)
                {
                    capture_hash[capture_id_offset] ^= (1llu << (hash_idx & 63u));
                    match_count++;
                }
            }
            hash_idx++;
        }
        bool found_redundant = false;
        for (uint64_t compare_idx = 0u; compare_idx < capture_id_offset; compare_idx++)
        {
            if (capture_hash[compare_idx] == capture_hash[capture_id_offset])
            {
                found_redundant = true;
                break;
            }
        }
        if (found_redundant || match_count == 0llu)
        {
            continue;
        }

        if (ImGui::BeginTable("ProfilerTable", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
        {
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("CPU (ms)", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableSetupColumn("GPU (ms)", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableHeadersRow();

            total_cpu_time = 0.0f;
            total_gpu_time = 0.0f;

            for (const auto& pair : entries)
            {
                const auto& label = pair.first;
                const auto& entry = pair.second;

                float cpu_ms = 0.f;
                float gpu_ms = 0.f;
                for (size_t idx = entry.entries.size() - 1u; idx < entry.entries.size(); idx--)
                {
                    if (entry.entries[idx].capture_id == cmp_capture_id)
                    {
                        cpu_ms += entry.entries[idx].entry.cpu_delta_time * 1000.0f;
                        gpu_ms += entry.entries[idx].entry.gpu_delta_time * 1000.0f;
                    }
                }
                if (cpu_ms == 0.f && gpu_ms == 0.f)
                {
                    continue;
                }

                total_cpu_time += cpu_ms;
                total_gpu_time += gpu_ms;

                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%s", label.c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%.3f", cpu_ms);
                ImGui::TableNextColumn();
                ImGui::Text("%.3f", gpu_ms);
            }

            if (!show_avg) // summing up average is misleading
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted("Total");
                ImGui::TableNextColumn();
                ImGui::Text("%.3f", total_cpu_time);
                ImGui::TableNextColumn();
                ImGui::Text("%.3f", total_gpu_time);
            }

            global_total_cpu_time += total_cpu_time;
            global_total_gpu_time += total_gpu_time;

            ImGui::EndTable();
        }
    }
    // if blobal time is unique, show it
    if (global_total_cpu_time != total_cpu_time || global_total_gpu_time != total_gpu_time)
    {
        if (ImGui::BeginTable("ProfilerTable", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
        {
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("CPU (ms)", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableSetupColumn("GPU (ms)", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableHeadersRow();

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted("Global Total");
            ImGui::TableNextColumn();
            ImGui::Text("%.3f", global_total_cpu_time);
            ImGui::TableNextColumn();
            ImGui::Text("%.3f", global_total_gpu_time);

            ImGui::EndTable();
        }
    }
}

void Profiler::report_callback(void* userdata,
                               pnanovdb_uint64_t captureID,
                               pnanovdb_uint32_t numEntries,
                               pnanovdb_compute_profiler_entry_t* entries)
{
    auto profiler = &Profiler::getInstance();
    if (profiler->profiler_paused_)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(profiler->mutex_);

    // Extract profiler label from userdata
    std::string name = reinterpret_cast<const char*>(userdata);
    if (name.empty())
    {
        name = "Profiler " + std::to_string(Profiler::s_id++);
    }

    profiler->profiler_capture_ids_[name] = captureID;

    // cleanup
    auto& profiler_entries = profiler->profiler_entries_[name];
    for (auto& pair : profiler_entries)
    {
        const auto& label = pair.first;
        auto& entry = pair.second;
        if (!entry.entries.empty() && captureID >= profiler->history_depth_)
        {
            size_t write_idx = 0u;
            for (size_t read_idx = 0u; read_idx < entry.entries.size(); read_idx++)
            {
                if (entry.entries[read_idx].capture_id > captureID - profiler->history_depth_)
                {
                    entry.entries[write_idx] = entry.entries[read_idx];
                    write_idx++;
                }
            }
            entry.entries.resize(write_idx);
        }
    }

    // add new
    for (pnanovdb_uint32_t i = 0; i < numEntries; ++i)
    {
        if (entries[i].label && entries[i].label[0] != '\0')
        {
            std::string label = entries[i].label;
            auto& profiler_entry = profiler->profiler_entries_[name][label];

            profiler_entry.entries.push_back({entries[i], captureID});
        }
    }
}

} // namespace pnanovdb_editor
