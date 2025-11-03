// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/editor/Console.h

    \author Petra Hapalova

    \brief
*/

#ifndef IMGUI_DEFINE_MATH_OPERATORS
#    define IMGUI_DEFINE_MATH_OPERATORS
#endif // IMGUI_DEFINE_MATH_OPERATORS

#include <imgui.h>

#include <TextEditor.h>

#include <string>
#include <mutex>
#include <vector>

#pragma once

namespace pnanovdb_editor
{
class Console
{
public:
    enum class LogLevel
    {
        Trace, // Very detailed per-frame/per-update logs
        Debug,
        Info,
        Warning,
        Error
    };

    static Console& getInstance()
    {
        static Console instance;
        return instance;
    }

    bool render();
    void addLog(const char* fmt, ...);
    void addLog(LogLevel level, const char* fmt, ...);

private:
    Console();
    ~Console() = default;

    Console(const Console&) = delete;
    Console& operator=(const Console&) = delete;
    Console(Console&&) = delete;
    Console& operator=(Console&&) = delete;

    struct LogEntry
    {
        std::string text; // fully formatted line including timestamp
        LogLevel level;
    };

    // state
    TextEditor editor_;
    std::mutex logMutex_;
    std::vector<LogEntry> logs_;

    // visibility toggles
    bool showTrace_ = false;
    bool showDebug_ = false;
    bool showInfo_ = true;
    bool showWarning_ = true;
    bool showError_ = true;

    // flag to rebuild editor text from logs_
    bool needsRebuild_ = true;

    // when paused, logs still accumulate but display doesn't update
    bool isPaused_ = false;

    // palettes for normal and paused states
    TextEditor::Palette normalPalette_;
    TextEditor::Palette pausedPalette_;

    // helpers
    static std::string makeTimestampPrefix();
    bool isLevelVisible(LogLevel level) const;
    void rebuildVisibleTextLocked();
};
}
