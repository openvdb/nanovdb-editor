// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

#include "ConsoleTestSupport.h"
#include "editor/Console.h"

#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace
{
std::mutex warning_mutex;
std::vector<std::string> warnings;
}

namespace pnanovdb_editor
{
Console::Console() = default;

void Console::addLog(const char*, ...)
{
}

void Console::addLog(LogLevel level, const char* format, ...)
{
    if (level != LogLevel::Warning)
    {
        return;
    }
    char message[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    std::lock_guard<std::mutex> lock(warning_mutex);
    warnings.emplace_back(message);
}

namespace test
{
void clearConsoleWarnings()
{
    std::lock_guard<std::mutex> lock(warning_mutex);
    warnings.clear();
}

std::vector<std::string> consoleWarnings()
{
    std::lock_guard<std::mutex> lock(warning_mutex);
    return warnings;
}
}
}
