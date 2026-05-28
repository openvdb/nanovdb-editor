#!/bin/bash
# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: Apache-2.0

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"

# Determine build type (default Release; use Debug if 'debug', '--debug' or '-d')
BUILD_TYPE="Release"
for arg in "$@"; do
    case "$arg" in
        debug|--debug|-d) BUILD_TYPE="Debug" ;;
    esac
done

# Always link against the currently installed nanovdb_editor wheel
USE_LOCAL_BUILD=OFF

if [ -f "$BUILD_DIR/CMakeCache.txt" ]; then
    if ! grep -q "^NANOVDB_EDITOR_TEST_USE_LOCAL_BUILD:BOOL=${USE_LOCAL_BUILD}$" "$BUILD_DIR/CMakeCache.txt"; then
        echo "-- Existing build was configured against local libs; reconfiguring against installed wheel..."
        rm -rf "$BUILD_DIR"
    fi
fi

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

echo "Building test in ${BUILD_TYPE} against installed nanovdb_editor wheel..."

cmake \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
    -DNANOVDB_EDITOR_TEST_USE_LOCAL_BUILD="${USE_LOCAL_BUILD}" \
    ..

cmake --build . -j

cd "$SCRIPT_DIR"

echo "Build completed successfully"
echo "Run: $BUILD_DIR/nanovdb_editor_test"
exit 0
