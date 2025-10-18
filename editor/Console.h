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

#include <imgui/ImguiTLS.h>
#include <imgui.h>

#include <TextEditor.h>

#include <string>
#include <mutex>

#pragma once

namespace pnanovdb_editor
{
class Console
{
public:
    static Console& getInstance()
    {
        static Console instance;
        return instance;
    }

    bool render();
    void addLog(const char* fmt, ...);

private:
    Console();
    ~Console() = default;

    Console(const Console&) = delete;
    Console& operator=(const Console&) = delete;
    Console(Console&&) = delete;
    Console& operator=(Console&&) = delete;
    ImGuiTextBuffer buffer_;
    bool scrollToBottom_ = false;
    std::mutex logMutex_;
};
}
