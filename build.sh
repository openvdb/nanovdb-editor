#!/bin/bash
# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: Apache-2.0

# Project variables
PROJECT_NAME="nanovdb_editor"
PROJECT_DIR="$(dirname "$(realpath $0)")"
BUILD_DIR="$PROJECT_DIR/build"
SLANG_DEBUG_OUTPUT=OFF
CLEAN_SHADERS=OFF
ENABLE_SANITIZERS=OFF
GLFW_OFF=OFF

# Parse command line arguments
clean_build=false
release=false
debug=false
verbose=false
python_only=false
editable_mode=false
test_only=false

usage() {
    echo "Usage: $0 [-x] [-r] [-d] [-v] [-s] [-a] [-p] [-e] [-t] [-f]"
    echo "  -x    Perform a clean build"
    echo "  -r    Build in release"
    echo "  -d    Build in debug"
    echo "  -v    Enable CMake verbose output"
    echo "  -s    Compile slang into ASM"
    echo "  -a    Build in debug with sanitizers"
    echo "  -p    Build and install python module (requires Python 3.8+, auto-installs scikit-build & wheel)"
    echo "  -e    Build and install python module in editable mode"
    echo "  -t    Run tests using ctest and pytest"
    echo "  -f    Disable GLFW (headless build)"
}

while getopts ":xrdvsapetfh" opt; do
    case ${opt} in
        x) clean_build=true ;;
        r) release=true ;;
        d) debug=true ;;
        v) verbose=true ;;
        s) SLANG_DEBUG_OUTPUT=ON; CLEAN_SHADERS=ON;;
        a) ENABLE_SANITIZERS=ON; debug=true ;;
        p) python_only=true ;;
        e) editable_mode=true ;;
        t) test_only=true ;;
        f) GLFW_OFF=ON ;;
        h) usage; exit 1 ;;
        \?) echo "Invalid option: $OPTARG" 1>&2; usage; exit 1 ;;
    esac
done

# Shift processed options
shift $((OPTIND -1))

# Set defaults
if [[ $release == false && $debug == false && $python_only == false && $test_only == false ]]; then
    release=true
fi

if $verbose; then
    CMAKE_VERBOSE="--verbose"
else
    CMAKE_VERBOSE=""
fi

if $clean_build; then
    echo "-- Performing a clean build..."
    rm -rf $BUILD_DIR
    echo "-- Deleted $BUILD_DIR"
    CLEAN_SHADERS=ON
fi

function run_build() {
    CONFIG=$1
    BUILD_DIR_CONFIG=$BUILD_DIR/$CONFIG

    echo "-- Building config $CONFIG..."

    # Create build directory
    if [ ! -d $BUILD_DIR_CONFIG ]; then
        mkdir -p $BUILD_DIR_CONFIG
    fi

    # Configure
    cmake -G "Unix Makefiles" -S $PROJECT_DIR -B $BUILD_DIR_CONFIG \
    -DCMAKE_BUILD_TYPE=$CONFIG \
    -DNANOVDB_EDITOR_CLEAN_SHADERS=$CLEAN_SHADERS \
    -DNANOVDB_EDITOR_SLANG_DEBUG_OUTPUT=$SLANG_DEBUG_OUTPUT \
    -DNANOVDB_EDITOR_ENABLE_SANITIZERS=$ENABLE_SANITIZERS \
    -DNANOVDB_EDITOR_BUILD_TESTS=ON \
    -DNANOVDB_EDITOR_USE_GLFW=$([ "$GLFW_OFF" = "ON" ] && echo OFF || echo ON)

    # Build
    cmake --build $BUILD_DIR_CONFIG --config $CONFIG $CMAKE_VERBOSE

    # Check for errors
    if [ $? -ne 0 ]; then
        echo "Failure while building $CONFIG" >&2
        exit 1
    else
        echo "-- Built config $CONFIG"
    fi
}

function build_python_module() {
    echo "-- Building python module..."

    if ! command -v python &> /dev/null && ! command -v python3 &> /dev/null; then
        echo "Error: Python not found. Please install Python 3.8 or later." >&2
        exit 1
    fi

    PYTHON_CMD="python"
    if command -v python3 &> /dev/null; then
        PYTHON_CMD="python3"
    fi

    echo "   -- Checking Python dependencies..."

    echo "   -- Installing scikit-build-core and wheel..."
    $PYTHON_CMD -m pip install --upgrade pip
    $PYTHON_CMD -m pip install --upgrade scikit-build-core wheel
    if ! $PYTHON_CMD -c "import scikit_build_core" 2>/dev/null; then
        echo "Error: Failed to install scikit-build-core. Please install manually:" >&2
        echo "  $PYTHON_CMD -m pip install scikit-build-core" >&2
        exit 1
    fi

    if ! $PYTHON_CMD -c "import wheel" 2>/dev/null; then
        echo "Error: Failed to install wheel. Please install manually:" >&2
        echo "  $PYTHON_CMD -m pip install wheel" >&2
        exit 1
    fi

    cd ./pymodule || exit 1

    if $clean_build; then
        echo "-- Cleaning up old python module builds..."
        rm -rf build/ _skbuild/ dist/ *.egg-info/
        echo "   -- Removed build directories"
    fi

    if $debug; then
        PIP_ARGS+=(--config-settings=cmake.build-type=Debug)
    else
        PIP_ARGS+=(--config-settings=cmake.build-type=Release)
    fi

    if [ "$GLFW_OFF" = "ON" ]; then
        PIP_ARGS+=(--config-settings=cmake.define.NANOVDB_EDITOR_USE_GLFW=OFF)
    fi

    if $verbose; then
        PIP_ARGS+=(--config-settings=cmake.verbose=true)
        PIP_ARGS+=(-v)
    fi

    # This is needed for the incremental builds to work without reinstalling the dependencies
    PIP_ARGS+=(--no-build-isolation)

    if $editable_mode; then
        echo "-- Installing NanoVDB editor in editable mode..."
        if $PYTHON_CMD -m pip install -e . "${PIP_ARGS[@]}"; then
            echo "-- NanoVDB editor installed in editable mode successfully"
        else
            echo "Error: Failed to install NanoVDB editor in editable mode" >&2
            exit 1
        fi
    else
        echo "-- Building and installing NanoVDB editor..."

        if $PYTHON_CMD -m pip wheel . --wheel-dir dist "${PIP_ARGS[@]}"; then
            echo "-- NanoVDB editor built successfully"
        else
            echo "Error: Failed to build NanoVDB editor" >&2
            exit 1
        fi

        if $PYTHON_CMD -m pip install dist/*.whl --force-reinstall; then
            echo "-- NanoVDB editor installed successfully from wheel"
        else
            echo "Error: Failed to install NanoVDB editor" >&2
            exit 1
        fi
    fi
    cd ..
}

function run_tests() {
    CONFIG=$1
    BUILD_DIR_CONFIG=$BUILD_DIR/$CONFIG

    if $verbose; then
        VERBOSE="--verbose"
    else
        VERBOSE=""
    fi

    echo "-- Running tests..."
    ctest --test-dir $BUILD_DIR_CONFIG/gtests -C $CONFIG --output-on-failure $VERBOSE
    pytest -vvv
}

if $python_only; then
    build_python_module
elif $test_only; then
    if [[ $release == false && $debug == false ]]; then
        release=true
    fi
    if $release; then
        run_tests "Release"
    fi
    if $debug; then
        run_tests "Debug"
    fi
else
    echo "-- Building $PROJECT_NAME..."
    if $release; then
        run_build "Release"
    fi
    if $debug; then
        run_build "Debug"
    fi
    echo "-- Build of $PROJECT_NAME completed"
fi

exit 0
