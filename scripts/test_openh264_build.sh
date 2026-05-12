#!/usr/bin/env bash
# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: Apache-2.0

set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${PROJECT_DIR}/build/ci-openh264"

if [[ -z "${CMAKE_BUILD_PARALLEL_LEVEL:-}" ]]; then
    if command -v getconf >/dev/null 2>&1; then
        export CMAKE_BUILD_PARALLEL_LEVEL="$(getconf _NPROCESSORS_ONLN)"
    elif command -v sysctl >/dev/null 2>&1; then
        export CMAKE_BUILD_PARALLEL_LEVEL="$(sysctl -n hw.ncpu)"
    else
        export CMAKE_BUILD_PARALLEL_LEVEL=4
    fi
fi

echo "-- Configuring OpenH264 regression build..."
rm -rf "${BUILD_DIR}"

cmake -G "Unix Makefiles" \
    -S "${PROJECT_DIR}" \
    -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DNANOVDB_EDITOR_USE_GLFW=OFF \
    -DNANOVDB_EDITOR_USE_H264=ON \
    -DNANOVDB_EDITOR_BUILD_TESTS=OFF

echo "-- Building openh264 target..."
cmake --build "${BUILD_DIR}" --target openh264_build --config Release --verbose
