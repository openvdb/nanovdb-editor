# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: Apache-2.0

# Build Configuration for NanoVDB Editor

cmake_minimum_required(VERSION 3.25 FATAL_ERROR)

# Build options
option(NANOVDB_EDITOR_USE_VCPKG "Use Vcpkg for dependencies" OFF)
option(NANOVDB_EDITOR_ENABLE_SANITIZERS "Enable sanitizers in debug builds" OFF)
option(NANOVDB_EDITOR_CLEAN_SHADERS "Clean shader cache (_generated dir)" OFF)
option(NANOVDB_EDITOR_SLANG_DEBUG_OUTPUT "Enable Slang debug output (compiles shader into Spirv Assembly)" OFF)
option(NANOVDB_EDITOR_E57_FORMAT "Use E57Format" OFF)
option(NANOVDB_EDITOR_ENABLE_LTO "Enable Link Time Optimization" ON)
option(NANOVDB_EDITOR_DIST_PACKAGE "Create distribution package" OFF)
option(NANOVDB_EDITOR_FORCE_REBUILD_DEPS "Force rebuild all dependencies (clears dependencies cache)" OFF)
option(NANOVDB_EDITOR_USE_GLFW "Use GLFW, not needed for streaming only, when OFF, Vulkan loadeer is built to ensure streaming compatibility" ON)
option(NANOVDB_EDITOR_BUILD_TESTS "Configure CMake to build gtests for NanoVDB Editor" ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

cmake_policy(SET CMP0069 NEW)  # INTERPROCEDURAL_OPTIMIZATION
cmake_policy(SET CMP0077 NEW)  # option() honors normal variables
cmake_policy(SET CMP0135 NEW)  # file(GENERATE) supports relative paths

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Compiler-specific optimizations
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-unused-parameter -Wno-unused-variable")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-format-security")
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -Wno-format-security")
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -Wno-format-security")

    # Optimization flags
    if(CMAKE_BUILD_TYPE STREQUAL "Release")

        set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -ffast-math")
    elseif(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(CMAKE_CXX_FLAGS_DEBUG "-O0 -g -DDEBUG")
        set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -fsanitize=address -fno-omit-frame-pointer")
    endif()

    # Add system library support for Linux compatibility
    if(UNIX AND NOT APPLE)
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -lstdc++fs -ldl -lm")
        set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -lstdc++fs -ldl -lm")
    endif()

    # Link-time optimization
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_JOBS auto)

elseif(MSVC)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /W4 /utf-8")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /permissive- /Zc:__cplusplus")

    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        set(CMAKE_CXX_FLAGS_RELEASE "/O2 /DNDEBUG")
    elseif(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(CMAKE_CXX_FLAGS_DEBUG "/Od /Zi /DDEBUG")
    endif()

    # Enable LTO for MSVC
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

# Platform-specific settings
if(UNIX AND NOT APPLE)
    # Linux-specific settings
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fPIC")
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)

elseif(APPLE)
    # macOS-specific settings
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fPIC")
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)

elseif(WIN32)
    # Windows-specific settings
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /bigobj")
    add_definitions(-DNOMINMAX -DWIN32_LEAN_AND_MEAN)
endif()

# Conditional settings
if(NANOVDB_EDITOR_ENABLE_SANITIZERS AND CMAKE_BUILD_TYPE STREQUAL "Debug")
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -fsanitize=address,undefined")
        set(CMAKE_EXE_LINKER_FLAGS_DEBUG "${CMAKE_EXE_LINKER_FLAGS_DEBUG} -fsanitize=address,undefined")
        set(CMAKE_SHARED_LINKER_FLAGS_DEBUG "${CMAKE_SHARED_LINKER_FLAGS_DEBUG} -fsanitize=address,undefined")
    endif()
endif()

if(NOT NANOVDB_EDITOR_ENABLE_LTO)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION FALSE)
endif()

# Output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# If switching from headless (GLFW OFF) to GLFW ON, remove the built Vulkan loader to use system one
if(NANOVDB_EDITOR_USE_GLFW)
  file(GLOB VULKAN_LOADER_FILES
       "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/libvulkan*"
       "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/vulkan*"
  )
  if(VULKAN_LOADER_FILES)
    message(STATUS "Removing previously built Vulkan loader: ${VULKAN_LOADER_FILES}")
    file(REMOVE ${VULKAN_LOADER_FILES})
  endif()
endif()

# Print configuration summary
message(STATUS "Build Configuration:")
message(STATUS "  Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "  E57 Format: ${NANOVDB_EDITOR_E57_FORMAT}")
message(STATUS "  Vcpkg: ${NANOVDB_EDITOR_USE_VCPKG}")
message(STATUS "  Sanitizers: ${NANOVDB_EDITOR_ENABLE_SANITIZERS}")
message(STATUS "  Clean Shaders: ${NANOVDB_EDITOR_CLEAN_SHADERS}")
message(STATUS "  Slang Debug Output: ${NANOVDB_EDITOR_SLANG_DEBUG_OUTPUT}")
message(STATUS "  Use GLFW: ${NANOVDB_EDITOR_USE_GLFW}")
message(STATUS "  Build Tests: ${NANOVDB_EDITOR_BUILD_TESTS}")
