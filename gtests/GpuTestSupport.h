// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

#ifndef NANOVDB_EDITOR_GTESTS_GPU_TEST_SUPPORT_H_HAS_BEEN_INCLUDED
#define NANOVDB_EDITOR_GTESTS_GPU_TEST_SUPPORT_H_HAS_BEEN_INCLUDED

#include <nanovdb_editor/putil/Compute.h>

#include <string>

namespace pnanovdb_editor_test
{

bool is_software_renderer_name(const char* name);

bool software_gpu_tests_force_enabled();

bool should_skip_on_software_renderer(const char* device_name);

std::string software_renderer_skip_reason(const char* device_name, const char* feature_label);

void stderr_log_print(pnanovdb_compute_log_level_t level, const char* fmt, ...);

} // namespace pnanovdb_editor_test

#endif
