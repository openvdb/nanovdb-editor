// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/editor/Console.cpp

    \author Petra Hapalova

    \brief
*/

#include "Console.h"
#include <chrono>
#include <iomanip>
#include <sstream>

namespace pnanovdb_editor
{
Console::Console()
{
}

bool Console::render()
{
    if (ImGui::BeginChild("ScrollingRegion", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()), false,
                          ImGuiWindowFlags_HorizontalScrollbar))
    {
        ImGui::TextUnformatted(buffer_.begin());

        // Auto-scroll to bottom when new content arrives, or if user is already at the bottom
        if (scrollToBottom_)
        {
            ImGui::SetScrollHereY(1.0f);
            scrollToBottom_ = false;
        }
        else if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
        {
            ImGui::SetScrollHereY(1.0f);
        }
    }
    ImGui::EndChild();

    return true;
}

void Console::addLog(const char* fmt, ...)
{
    std::lock_guard<std::mutex> lock(logMutex_);

    // Add timestamp
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

    std::tm tm = {};
#ifdef _WIN32
    localtime_s(&tm, &time);
#else
    localtime_r(&time, &tm);
#endif

    std::ostringstream timestamp;
    timestamp << "[" << std::setfill('0') << std::setw(4) << (tm.tm_year + 1900) << "-" << std::setfill('0')
              << std::setw(2) << (tm.tm_mon + 1) << "-" << std::setfill('0') << std::setw(2) << tm.tm_mday << " "
              << std::setfill('0') << std::setw(2) << tm.tm_hour << ":" << std::setfill('0') << std::setw(2)
              << tm.tm_min << ":" << std::setfill('0') << std::setw(2) << tm.tm_sec << "." << std::setfill('0')
              << std::setw(3) << ms.count() << "] ";

    buffer_.append(timestamp.str().c_str());

    va_list args;
    va_start(args, fmt);
    buffer_.appendfv(fmt, args);
    va_end(args);
    buffer_.append("\n");
    scrollToBottom_ = true;
}

}
