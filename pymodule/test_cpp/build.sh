#!/bin/bash
# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: Apache-2.0

mkdir -p build
cd build

# Determine build type (default Release; use Debug if 'debug', '--debug' or '-d')
BUILD_TYPE="Release"
if [[ "$1" == "debug" || "$1" == "--debug" || "$1" == "-d" ]]; then
    BUILD_TYPE="Debug"
fi

echo "Building test in ${BUILD_TYPE}..."

cmake -DCMAKE_BUILD_TYPE=${BUILD_TYPE} ..
cmake --build .

# Check for errors
if [ $? -ne 0 ]; then
    echo "Failure while building test" >&2
    exit 1
fi

# Return to original directory
cd ..

echo "Build completed successfully"
exit 0
