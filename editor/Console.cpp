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
#include <cstring>

namespace pnanovdb_editor
{
// Deduplicated labels and tooltips (file-scope for reuse)
static const char* LABEL_DEBUG = "D";
static const char* LABEL_INFO = "I";
static const char* LABEL_WARNING = "W";
static const char* LABEL_ERROR = "E";
static const char* LABEL_ALL = "All";
static const char* LABEL_NONE = "None";
static const char* LABEL_CLEAR = "Clear";

static const char* TIP_DEBUG = "Show Debug";
static const char* TIP_INFO = "Show Info";
static const char* TIP_WARNING = "Show Warnings";
static const char* TIP_ERROR = "Show Errors";
static const char* TIP_CLEAR = "Clear Log";

Console::Console()
{
    editor_.SetShowLineNumbersEnabled(false);
    editor_.SetReadOnlyEnabled(true);
    editor_.SetShowWhitespacesEnabled(false);
    editor_.SetShowScrollbarMiniMapEnabled(false);
    editor_.SetShowPanScrollIndicatorEnabled(false);

    // Hide cursor by making it the same color as the background
    TextEditor::Palette palette = editor_.GetPalette();
    palette[static_cast<size_t>(TextEditor::Color::cursor)] = palette[static_cast<size_t>(TextEditor::Color::background)];
    editor_.SetPalette(palette);

    editor_.ClearCursors();
}

bool Console::render()
{
    bool toggled = false;

    // Compact toolbar for log filters
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.0f, 2.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 2.0f));

    auto drawToggle = [&](const char* label, bool& value, const ImVec4& onColor, const char* tooltip)
    {
        ImVec4 baseBtn = ImGui::GetStyleColorVec4(ImGuiCol_Button);
        ImVec4 baseHov = ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered);
        ImVec4 baseAct = ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
        ImGui::PushStyleColor(ImGuiCol_Button, value ? onColor : baseBtn);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, value ? onColor : baseHov);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, value ? onColor : baseAct);
        if (ImGui::SmallButton(label))
        {
            value = !value;
            toggled = true;
        }
        if (ImGui::IsItemHovered() && tooltip)
        {
            ImGui::SetTooltip("%s", tooltip);
        }
        ImGui::PopStyleColor(3);
    };

    ImGui::AlignTextToFramePadding();

    // Left-aligned verbosity buttons group
    drawToggle(LABEL_ERROR, showError_, ImVec4(0.90f, 0.25f, 0.25f, 0.90f), TIP_ERROR);
    ImGui::SameLine();
    drawToggle(LABEL_WARNING, showWarning_, ImVec4(0.85f, 0.70f, 0.20f, 0.85f), TIP_WARNING);
    ImGui::SameLine();
    drawToggle(LABEL_INFO, showInfo_, ImVec4(0.30f, 0.70f, 0.30f, 0.80f), TIP_INFO);
    ImGui::SameLine();
    drawToggle(LABEL_DEBUG, showDebug_, ImVec4(0.20f, 0.50f, 0.90f, 0.80f), TIP_DEBUG);
    ImGui::SameLine();
    if (ImGui::SmallButton(LABEL_ALL))
    {
        showDebug_ = showInfo_ = showWarning_ = showError_ = true;
        toggled = true;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(LABEL_NONE))
    {
        showDebug_ = showInfo_ = showWarning_ = showError_ = false;
        toggled = true;
    }

    // Right-align Clear button
    const ImGuiStyle& style = ImGui::GetStyle();
    auto buttonWidth = [&](const char* label) { return ImGui::CalcTextSize(label).x + style.FramePadding.x * 2.0f; };
    float groupWidth = buttonWidth(LABEL_CLEAR);

    float startX = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - groupWidth;
    float minX = ImGui::GetCursorPosX() + style.ItemSpacing.x;
    if (startX < minX)
        startX = minX;
    ImGui::SameLine(startX);

    if (ImGui::SmallButton(LABEL_CLEAR))
    {
        std::lock_guard<std::mutex> lock(logMutex_);
        logs_.clear();
        editor_.SetText("");
        needsRebuild_ = false;
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("%s", TIP_CLEAR);
    }

    float toolbarHeight = ImGui::GetFrameHeightWithSpacing();
    ImGui::PopStyleVar(2);

    if (toggled)
    {
        std::lock_guard<std::mutex> lock(logMutex_);
        needsRebuild_ = true;
    }

    if (needsRebuild_)
    {
        std::lock_guard<std::mutex> lock(logMutex_);
        rebuildVisibleTextLocked();
        needsRebuild_ = false;
    }

    editor_.Render("##console_log", ImVec2(-1, -toolbarHeight), false);

    return true;
}

void Console::addLog(const char* fmt, ...)
{
    char buffer[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    addLog(LogLevel::Info, "%s", buffer);
}

void Console::addLog(LogLevel level, const char* fmt, ...)
{
    std::lock_guard<std::mutex> lock(logMutex_);

    va_list args;
    va_start(args, fmt);

    char msgbuf[1024];
    vsnprintf(msgbuf, sizeof(msgbuf), fmt, args);
    va_end(args);

    std::ostringstream line;

    const char* lvl = LABEL_INFO;
    switch (level)
    {
    case LogLevel::Debug:
        lvl = LABEL_DEBUG;
        break;
    case LogLevel::Info:
        lvl = LABEL_INFO;
        break;
    case LogLevel::Warning:
        lvl = LABEL_WARNING;
        break;
    case LogLevel::Error:
        lvl = LABEL_ERROR;
        break;
    }

    line << makeTimestampPrefix() << "[" << lvl << "] " << msgbuf;

    logs_.push_back({ line.str(), level });
    needsRebuild_ = true;
}

std::string Console::makeTimestampPrefix()
{
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

    std::tm tm = {};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif

    std::ostringstream date;
    date << std::setfill('0') << std::setw(4) << (tm.tm_year + 1900) << "-" << std::setfill('0') << std::setw(2)
         << (tm.tm_mon + 1) << "-" << std::setfill('0') << std::setw(2) << tm.tm_mday;

    std::ostringstream time;
    time << std::setfill('0') << std::setw(2) << tm.tm_hour << ":" << std::setfill('0') << std::setw(2) << tm.tm_min
         << ":" << std::setfill('0') << std::setw(2) << tm.tm_sec << "." << std::setfill('0') << std::setw(3)
         << ms.count();

    std::ostringstream ts;
    // ts << "[" << date.str() << " " << time.str() << "] ";  // Full timestamp with date
    ts << "[" << time.str() << "] ";
    return ts.str();
}

bool Console::isLevelVisible(LogLevel level) const
{
    switch (level)
    {
    case LogLevel::Debug:
        return showDebug_;
    case LogLevel::Info:
        return showInfo_;
    case LogLevel::Warning:
        return showWarning_;
    case LogLevel::Error:
        return showError_;
    }
    return true;
}

void Console::rebuildVisibleTextLocked()
{
    std::ostringstream text;
    bool first = true;
    for (const auto& entry : logs_)
    {
        if (!isLevelVisible(entry.level))
        {
            continue;
        }
        if (!first)
        {
            text << "\n";
        }
        text << entry.text;
        first = false;
    }
    editor_.SetText(text.str());

    int lineCount = editor_.GetLineCount();
    if (lineCount > 0)
    {
        editor_.ScrollToLine(lineCount - 1, TextEditor::Scroll::alignBottom);
    }
}

}
