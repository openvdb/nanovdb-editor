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
    editor_.SetShowLineNumbersEnabled(false);
    editor_.SetReadOnlyEnabled(true);

    // Hide cursor by making it the same color as the background
    TextEditor::Palette palette = editor_.GetPalette();
    palette[static_cast<size_t>(TextEditor::Color::cursor)] = palette[static_cast<size_t>(TextEditor::Color::background)];
    editor_.SetPalette(palette);

    editor_.ClearCursors();
}

bool Console::render()
{
    editor_.Render("##console_log", ImVec2(-1, -ImGui::GetFrameHeightWithSpacing()), false);

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

    std::ostringstream log;

    std::string currentText = editor_.GetText();
    if (!currentText.empty())
    {
        log << "\n";
    }

    log << "[" << std::setfill('0') << std::setw(4) << (tm.tm_year + 1900) << "-" << std::setfill('0')
              << std::setw(2) << (tm.tm_mon + 1) << "-" << std::setfill('0') << std::setw(2) << tm.tm_mday << " "
              << std::setfill('0') << std::setw(2) << tm.tm_hour << ":" << std::setfill('0') << std::setw(2)
              << tm.tm_min << ":" << std::setfill('0') << std::setw(2) << tm.tm_sec << "." << std::setfill('0')
              << std::setw(3) << ms.count() << "] ";

    va_list args;
    va_start(args, fmt);

    char buffer[1024];
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    log << buffer;

    // Append to existing text
    editor_.SetText(currentText + log.str());

    // Scroll to bottom
    int lineCount = editor_.GetLineCount();
    if (lineCount > 0)
    {
        editor_.ScrollToLine(lineCount - 1, TextEditor::Scroll::alignBottom);
    }
}

}
