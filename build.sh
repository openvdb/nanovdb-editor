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
test_only=false

usage() {
    echo "Usage: $0 [-x] [-r] [-d] [-v] [-s] [-a] [-p] [-t] [-f]"
    echo "  -x    Perform a clean build"
    echo "  -r    Build in release"
    echo "  -d    Build in debug"
    echo "  -v    Enable CMake verbose output"
    echo "  -s    Compile slang into ASM"
    echo "  -a    Build in debug with sanitizers"
    echo "  -p    Build and install python module (requires Python 3.8+, auto-installs scikit-build & wheel)"
    echo "  -t    Run tests using ctest"
    echo "  -f    Disable GLFW (headless build)"
}

while getopts ":xrdvsapthf" opt; do
    case ${opt} in
        x) clean_build=true ;;
        r) release=true ;;
        d) debug=true ;;
        v) verbose=true ;;
        s) SLANG_DEBUG_OUTPUT=ON; CLEAN_SHADERS=ON;;
        a) ENABLE_SANITIZERS=ON; debug=true ;;
        p) python_only=true ;;
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

    CONFIG_SETTINGS=()

    if $debug; then
        CONFIG_SETTINGS+=(-C cmake.build-type=Debug)
    else
        CONFIG_SETTINGS+=(-C cmake.build-type=Release)
    fi
    if $verbose; then
        CONFIG_SETTINGS+=(-C cmake.verbose=true)
    fi

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
    $PYTHON_CMD -m pip install --upgrade scikit-build-core wheel build
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

    VERSION=$(<VERSION.txt)
    export SETUPTOOLS_SCM_PRETEND_VERSION=$VERSION

    echo "-- Cleaning up old python module builds..."
    rm -rf build/ dist/ *.egg-info/ _skbuild/

    echo "-- Building NanoVDB editor wheel..."
    if $PYTHON_CMD -m build --wheel "${CONFIG_SETTINGS[@]}"; then
        echo "-- NanoVDB editor wheel built successfully"
    else
        echo "Error: Failed to build NanoVDB editor wheel" >&2
        exit 1
    fi

    echo "-- Installing NanoVDB editor wheel..."
    if $PYTHON_CMD -m pip install --force-reinstall dist/*.whl; then
        echo "-- NanoVDB editor wheel installed successfully"
    else
        echo "Error: Failed to install NanoVDB editor wheel" >&2
        exit 1
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
