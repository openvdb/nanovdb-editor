// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/editor/Console.cpp

    \author Petra Hapalova

    \brief
*/

#include "Console.h"
#include "imgui/ImguiWindow.h"

#include <chrono>
#include <iomanip>
#include <sstream>
#include <cstring>

namespace pnanovdb_editor
{
static const char* LABEL_TRACE = "T";
static const char* LABEL_DEBUG = "D";
static const char* LABEL_INFO = "I";
static const char* LABEL_WARNING = "W";
static const char* LABEL_ERROR = "E";
static const char* LABEL_ALL = "All";
static const char* LABEL_NONE = "None";
static const char* LABEL_PAUSE = "Pause";
static const char* LABEL_COPY = "Copy";
static const char* LABEL_CLEAR = "Clear";

static const char* TIP_TRACE = "Show Trace";
static const char* TIP_DEBUG = "Show Debug";
static const char* TIP_INFO = "Show Info";
static const char* TIP_WARNING = "Show Warnings";
static const char* TIP_ERROR = "Show Errors";
static const char* TIP_PAUSE = "Pause updates";
static const char* TIP_COPY = "Copy all visible logs to clipboard";
static const char* TIP_CLEAR = "Clear Log";

Console::Console()
{
    editor_.SetShowLineNumbersEnabled(false);
    editor_.SetReadOnlyEnabled(true);
    editor_.SetShowWhitespacesEnabled(false);
    editor_.SetShowScrollbarMiniMapEnabled(false);
    editor_.SetShowPanScrollIndicatorEnabled(false);

    normalPalette_ = editor_.GetPalette();
    // Hide cursor by making it the same color as the background (but allow selection)
    normalPalette_[static_cast<size_t>(TextEditor::Color::cursor)] =
        normalPalette_[static_cast<size_t>(TextEditor::Color::background)];

    pausedPalette_ = normalPalette_;
    // Make all text grey when paused (apply to entire palette)
    for (size_t i = 0; i < pausedPalette_.size(); ++i)
    {
        if (i == static_cast<size_t>(TextEditor::Color::background) ||
            i == static_cast<size_t>(TextEditor::Color::cursor))
        {
            continue;
        }
        pausedPalette_[i] = 0xff808080;
    }

    editor_.SetPalette(normalPalette_);
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
    drawToggle(LABEL_TRACE, showTrace_, ImVec4(0.40f, 0.40f, 0.40f, 0.70f), TIP_TRACE);
    ImGui::SameLine();
    if (ImGui::SmallButton(LABEL_ALL))
    {
        showTrace_ = showDebug_ = showInfo_ = showWarning_ = showError_ = true;
        toggled = true;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(LABEL_NONE))
    {
        showTrace_ = showDebug_ = showInfo_ = showWarning_ = showError_ = false;
        toggled = true;
    }

    // Right-align Pause, Copy and Clear buttons
    const ImGuiStyle& style = ImGui::GetStyle();
    auto buttonWidth = [&](const char* label) { return ImGui::CalcTextSize(label).x + style.FramePadding.x * 2.0f; };
    float groupWidth = buttonWidth(LABEL_PAUSE) + style.ItemSpacing.x + buttonWidth(LABEL_COPY) + style.ItemSpacing.x +
                       buttonWidth(LABEL_CLEAR);

    float startX = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - groupWidth;
    float minX = ImGui::GetCursorPosX() + style.ItemSpacing.x;
    if (startX < minX)
        startX = minX;
    ImGui::SameLine(startX);

    // Pause button with color to indicate state
    ImVec4 pauseColor = isPaused_ ? ImVec4(0.85f, 0.70f, 0.20f, 0.85f) : ImGui::GetStyleColorVec4(ImGuiCol_Button);
    ImGui::PushStyleColor(ImGuiCol_Button, pauseColor);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, pauseColor);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, pauseColor);
    if (ImGui::SmallButton(LABEL_PAUSE))
    {
        isPaused_ = !isPaused_;
        if (!isPaused_)
        {
            // Resume - force rebuild to show accumulated logs
            needsRebuild_ = true;
            editor_.SetPalette(normalPalette_);
        }
        else
        {
            // Paused - use grey palette
            editor_.SetPalette(pausedPalette_);
        }
    }
    ImGui::PopStyleColor(3);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("%s", TIP_PAUSE);
    }

    ImGui::SameLine();
    if (ImGui::SmallButton(LABEL_COPY))
    {
        std::string text = editor_.GetText();
        if (!text.empty())
        {
            pnanovdb_imgui_set_system_clipboard(text.c_str());
        }
        else
        {
            addLog(LogLevel::Warning, "Console is empty, nothing to copy");
        }
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("%s", TIP_COPY);
    }

    ImGui::SameLine();
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

    // Only rebuild display if not paused (logs still accumulate in background)
    if (needsRebuild_ && !isPaused_)
    {
        std::lock_guard<std::mutex> lock(logMutex_);
        rebuildVisibleTextLocked();
        needsRebuild_ = false;
    }

    editor_.Render("##console_log", ImVec2(-1, -toolbarHeight), true);

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
    case LogLevel::Trace:
        return showTrace_;
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
        editor_.ScrollToLine(lineCount, TextEditor::Scroll::alignBottom);
    }
}

}
