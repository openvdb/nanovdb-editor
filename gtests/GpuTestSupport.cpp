// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

#include "GpuTestSupport.h"

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace pnanovdb_editor_test
{

bool is_software_renderer_name(const char* name)
{
    if (!name || name[0] == '\0')
    {
        return false;
    }
    std::string lowered(name);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lowered.find("lavapipe") != std::string::npos || lowered.find("llvmpipe") != std::string::npos ||
           lowered.find("swiftshader") != std::string::npos;
}

bool software_gpu_tests_force_enabled()
{
    const char* env = std::getenv("PNANOVDB_ENABLE_SOFTWARE_GPU_TESTS");
    return env && env[0] != '\0' && std::strcmp(env, "0") != 0;
}

bool should_skip_on_software_renderer(const char* device_name)
{
    return is_software_renderer_name(device_name) && !software_gpu_tests_force_enabled();
}

std::string software_renderer_skip_reason(const char* device_name, const char* feature_label)
{
    std::string reason = "Software Vulkan driver detected ('";
    reason += (device_name ? device_name : "");
    reason += "'); skipping ";
    reason += (feature_label && feature_label[0] != '\0') ? feature_label : "GPU-heavy test";
    reason +=
        " because it is prohibitively slow on software rasterizers. "
        "Set PNANOVDB_ENABLE_SOFTWARE_GPU_TESTS=1 to override.";
    return reason;
}

namespace
{
bool gpu_test_debug_logging_enabled()
{
    const char* env = std::getenv("PNANOVDB_GPU_TEST_DEBUG_LOG");
    return env && env[0] != '\0' && std::strcmp(env, "0") != 0;
}
} // namespace

void stderr_log_print(pnanovdb_compute_log_level_t level, const char* fmt, ...)
{
    if (level == PNANOVDB_COMPUTE_LOG_LEVEL_DEBUG && !gpu_test_debug_logging_enabled())
    {
        return;
    }

    const char* level_str = "INFO";
    switch (level)
    {
    case PNANOVDB_COMPUTE_LOG_LEVEL_ERROR:
        level_str = "ERROR";
        break;
    case PNANOVDB_COMPUTE_LOG_LEVEL_WARNING:
        level_str = "WARN";
        break;
    case PNANOVDB_COMPUTE_LOG_LEVEL_INFO:
        level_str = "INFO";
        break;
    case PNANOVDB_COMPUTE_LOG_LEVEL_DEBUG:
        level_str = "DEBUG";
        break;
    default:
        break;
    }

    va_list args;
    va_start(args, fmt);
    std::fprintf(stderr, "[pnanovdb-%s] ", level_str);
    if (fmt)
    {
        std::vfprintf(stderr, fmt, args);
    }
    std::fprintf(stderr, "\n");
    std::fflush(stderr);
    va_end(args);
}

} // namespace pnanovdb_editor_test
