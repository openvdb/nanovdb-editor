# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: Apache-2.0
#
#[=======================================================================[

  CMake Configuration for NanoVDB Editor

#]=======================================================================]

cmake_minimum_required(VERSION 3.25 FATAL_ERROR)

# Set CMake policies to suppress warnings
if(POLICY CMP0175)
    cmake_policy(SET CMP0175 NEW)
endif()

# Set minimum policy version to resolve blosc compatibility issues
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/build_config.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/target_utils.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/link_utils.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/dist_utils.cmake)

set(NANOVDB_EDITOR_SLANG_PROFILE "spirv_1_3" CACHE STRING "Slang shader profile to use")

project(NanoVDBEditor LANGUAGES CXX)

### Directory Setup

set(SHADERS_DIR_NAME shaders)
set(INCLUDES_FILE includes)
set(COMPILED_SHADERS_DIR_NAME _generated)
set(COMPILED_SHADERS_TEMP_DIR_NAME nanovdb_editor/shader_cache)

add_definitions(-DNANOVDB_EDITOR_SHADER_DIR="${SHADERS_DIR_NAME}")
add_definitions(-DNANOVDB_EDITOR_INCLUDES_FILE="${INCLUDES_FILE}")
add_definitions(-DNANOVDB_EDITOR_SHADER_CACHE="${SHADERS_DIR_NAME}/${COMPILED_SHADERS_DIR_NAME}")
add_definitions(-DNANOVDB_EDITOR_TEMP_SHADER_CACHE="${COMPILED_SHADERS_TEMP_DIR_NAME}")
add_definitions(-DNANOVDB_EDITOR_SHADER_PARAMS_SIZE=64)

# set the shaders directory next to the lib directory
set(SHADERS_DIR "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/../${SHADERS_DIR_NAME}")
set(COMPILED_SHADERS_DIR "${SHADERS_DIR}/${COMPILED_SHADERS_DIR_NAME}")

if(NANOVDB_EDITOR_CLEAN_SHADERS)
  if(EXISTS "${COMPILED_SHADERS_DIR}")
      file(REMOVE_RECURSE "${COMPILED_SHADERS_DIR}")
      message(STATUS "Removed existing folder for shader cache: ${COMPILED_SHADERS_DIR}")
  endif()
endif()

if(NOT EXISTS "${COMPILED_SHADERS_DIR}" AND NOT SKBUILD)
  file(MAKE_DIRECTORY "${COMPILED_SHADERS_DIR}")
  message(STATUS "Created folder for shader cache: ${COMPILED_SHADERS_DIR}")
endif()

set(DATA_DIR ${CMAKE_SOURCE_DIR}/data/)
file(MAKE_DIRECTORY "${DATA_DIR}")

set(SHADER_DIRECTORIES compute imgui editor raster test)

### Dependencies

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/get_cpm.cmake)

# Stabilize VulkanLoader sysconfdir during SKBUILD to avoid temp wheel paths in defines
if(SKBUILD)
    set(SYSCONFDIR "/etc" CACHE STRING "System-wide search directory for Vulkan loader" FORCE)
endif()

if(NANOVDB_EDITOR_USE_VCPKG)
  find_package(ZLIB REQUIRED)
  find_package(Blosc REQUIRED)

  if(NANOVDB_EDITOR_E57_FORMAT)
    find_package(e57format REQUIRED)
  endif()
endif()

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/packages.cmake)

### PNanoVDB Compiler

file(GLOB COMPILER_SOURCE_FILES "compiler/*.cpp")
file(GLOB COMPILER_HEADER_FILES "compiler/*.h")

create_nanovdb_library(pnanovdbcompiler
    SOURCES ${COMPILER_SOURCE_FILES}
    HEADERS ${COMPILER_HEADER_FILES}
    INCLUDES
        ./
        ./compiler
        ${nanovdb_SOURCE_DIR}/nanovdb
        ${Slang_SOURCE_DIR}/include
    LIBS
        slang
        nlohmann_json::nlohmann_json
    DEFINITIONS
        SLANG_PROFILE=${NANOVDB_EDITOR_SLANG_PROFILE}
)

if(NANOVDB_EDITOR_SLANG_DEBUG_OUTPUT)
  target_compile_definitions(pnanovdbcompiler PRIVATE USE_SLANG_DEBUG_OUTPUT)
endif()

if(APPLE)
  target_link_libraries(pnanovdbcompiler PRIVATE
      "-framework CoreFoundation"
      "-framework CoreServices"
  )
endif()

if(NOT SKBUILD)
    if(TARGET copy_slang_libs)
        add_dependencies(pnanovdbcompiler copy_slang_libs)
    endif()
endif()

### PNanoVDB Compute

file(GLOB COMPUTE_SOURCE_FILES "compute/*.cpp")
file(GLOB VULKAN_SOURCE_FILES "vulkan/*.cpp")
file(GLOB RASTER_SOURCE_FILES "raster/*.cpp")
file(GLOB COMPUTE_HEADER_FILES "compute/*.h")
file(GLOB VULKAN_HEADER_FILES "vulkan/*.h")

create_nanovdb_library(pnanovdbcompute
    SOURCES
        ${COMPUTE_SOURCE_FILES}
        ${VULKAN_SOURCE_FILES}
        ${RASTER_SOURCE_FILES}
    HEADERS
        ${COMPUTE_HEADER_FILES}
        ${VULKAN_HEADER_FILES}
    INCLUDES
        ./
        ./compute
        ./vulkan
        ./raster
        ${nanovdb_SOURCE_DIR}/nanovdb
    LIBS
        nlohmann_json::nlohmann_json
        blosc_static
        VulkanHeaders
    DEFINITIONS
        NOMINMAX
        NANOVDB_USE_BLOSC
)

if(NANOVDB_EDITOR_USE_GLFW)
    target_compile_definitions(pnanovdbcompute PRIVATE PNANOVDB_USE_GLFW)
endif()

# if (UNIX AND NOT APPLE)
#     # Make sure RPATH takes precedence over the system library directories
#     set_target_properties(pnanovdbcompute PROPERTIES LINK_FLAGS "-Wl,--disable-new-dtags")
# endif()

# Ensure the Vulkan loader is built and copied for non-scikit-build builds only
if(NOT SKBUILD)
    if(TARGET copy_vulkan_loader)
        add_dependencies(pnanovdbcompute copy_vulkan_loader)
    endif()
endif()

### PNanoVDB FileFormat

file(GLOB FILEFORMAT_SOURCE_FILES "fileformat/*.cpp")
file(GLOB FILEFORMAT_HEADER_FILES "fileformat/*.h")

create_nanovdb_library(pnanovdbfileformat
    SOURCES ${FILEFORMAT_SOURCE_FILES}
    HEADERS ${FILEFORMAT_HEADER_FILES}
    INCLUDES
        ./
        ${nanovdb_SOURCE_DIR}/nanovdb
        ${cnpy_SOURCE_DIR}
        ${zstr_SOURCE_DIR}/src
    LIBS
        nlohmann_json::nlohmann_json
        cnpy-static
        zlibstatic
    DEFINITIONS
        $<$<BOOL:${NANOVDB_EDITOR_E57_FORMAT}>:NANOVDB_EDITOR_E57_FORMAT>
)

if(NANOVDB_EDITOR_E57_FORMAT)
    target_link_libraries(pnanovdbfileformat PRIVATE E57Format)
endif()

### PNanoVDB Editor

# Read version from VERSION.txt
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/pymodule/VERSION.txt" NANOVDB_EDITOR_VERSION)
string(STRIP "${NANOVDB_EDITOR_VERSION}" NANOVDB_EDITOR_VERSION)

# Check if NANOVDB_EDITOR_COMMIT_HASH is already defined (e.g., passed via cmake -D)
if(NOT DEFINED NANOVDB_EDITOR_COMMIT_HASH OR NANOVDB_EDITOR_COMMIT_HASH STREQUAL "")
    # Get git commit hash from the nanovdb-editor repository
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git")
        execute_process(
            COMMAND git rev-parse --short HEAD
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            OUTPUT_VARIABLE NANOVDB_EDITOR_COMMIT_HASH
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
            RESULT_VARIABLE GIT_RESULT
        )
        if(NOT GIT_RESULT EQUAL 0)
            set(NANOVDB_EDITOR_COMMIT_HASH "Unknown")
        endif()
    else()
        set(NANOVDB_EDITOR_COMMIT_HASH "Unknown")
    endif()
endif()

message(STATUS "NanoVDB Editor Version: ${NANOVDB_EDITOR_VERSION}")
message(STATUS "NanoVDB Editor Commit Hash: ${NANOVDB_EDITOR_COMMIT_HASH}")

file(GLOB EDITOR_SOURCE_FILES "editor/*.cpp")
file(GLOB SERVER_SOURCE_FILES "server/*.cpp")
file(GLOB VULKAN_IMGUI_SOURCE_FILES "imgui/*.cpp")
file(GLOB EDITOR_HEADER_FILES "editor/*.h")

create_nanovdb_library(pnanovdbeditor
    SOURCES
        ${EDITOR_SOURCE_FILES}
        ${SERVER_SOURCE_FILES}
        ${VULKAN_IMGUI_SOURCE_FILES}
        ${IMGUI_SOURCE_FILES}
        ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp    # for std::string text input
    HEADERS ${EDITOR_HEADER_FILES}
    INCLUDES
        ./
        ./imgui
        ${nanovdb_SOURCE_DIR}/nanovdb
        ${imgui_SOURCE_DIR}
        ${glfw_SOURCE_DIR}/include
        ${FileWatch_SOURCE_DIR}
        ${ImGuiFileDialog_SOURCE_DIR}
        ${ImGuiColorTextEdit_SOURCE_DIR}
        ${llhttp_SOURCE_DIR}/include
        ${asio_SOURCE_DIR}/asio/include
        ${expected_SOURCE_DIR}/include
        ${restinio_SOURCE_DIR}/dev
    LIBS
        nlohmann_json::nlohmann_json
        llhttp_static
        ImGuiColorTextEdit
        ImGuiFileDialog
        fmt::fmt
        VulkanHeaders
    DEFINITIONS
        NOMINMAX
        COMPILED_SHADERS_DIR=${COMPILED_SHADERS_DIR}
        NANOVDB_EDITOR_VERSION="${NANOVDB_EDITOR_VERSION}"
        NANOVDB_EDITOR_COMMIT_HASH="${NANOVDB_EDITOR_COMMIT_HASH}"
)

if(NANOVDB_EDITOR_USE_GLFW)
    target_compile_definitions(pnanovdbeditor PRIVATE PNANOVDB_USE_GLFW)
endif()

if(APPLE)
    target_link_libraries(pnanovdbeditor PRIVATE
        "-framework CoreFoundation"
        "-framework CoreServices"
    )
endif()

if(MSVC)
    target_compile_options(pnanovdbeditor PRIVATE /utf-8)
endif()

add_sanitizer_support(pnanovdbeditor)

if(WIN32 AND TARGET glfw)
    add_custom_command(
        TARGET pnanovdbeditor POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_PROPERTY:glfw,IMPORTED_LOCATION>" "${CMAKE_BINARY_DIR}/$<CONFIG>/glfw3.dll"
        COMMENT "Copying GLFW DLL"
    )
endif()

if(NANOVDB_EDITOR_BUILD_TESTS)
    add_subdirectory(gtests)
endif()

set(NANOVDB_EDITOR_PYTHON_LIB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/pymodule/nanovdb_editor/lib)
set(NANOVDB_EDITOR_PYTHON_SHADERS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/pymodule/nanovdb_editor/${SHADERS_DIR_NAME})

if(SKBUILD)
    message(STATUS "Building with scikit-build")

    # Links should be removed for scikit-build, shaders and files are installed instead
    file(REMOVE_RECURSE "${NANOVDB_EDITOR_PYTHON_LIB_DIR}")
    file(REMOVE_RECURSE "${NANOVDB_EDITOR_PYTHON_SHADERS_DIR}")

    # Find Python components for scikit-build
    find_package(Python3 COMPONENTS Interpreter Development.Module REQUIRED)

    # Set up proper installation directories for scikit-build
    # scikit-build will automatically set CMAKE_INSTALL_PREFIX

    # Install headers
    install(DIRECTORY "nanovdb_editor/putil/"
        DESTINATION nanovdb_editor/include/nanovdb_editor/putil
        COMPONENT python_headers
        FILES_MATCHING PATTERN "*"
    )

    # Install NanoVDB headers
    install(DIRECTORY "${nanovdb_SOURCE_DIR}/nanovdb/nanovdb/putil/"
        DESTINATION nanovdb_editor/include/nanovdb/putil
        COMPONENT python_headers
        FILES_MATCHING PATTERN "*"
    )
    install(FILES
        "${nanovdb_SOURCE_DIR}/nanovdb/nanovdb/PNanoVDB.h"
        DESTINATION nanovdb_editor/include/nanovdb
        COMPONENT python_headers
    )
    install(FILES
        "nanovdb_editor/PNanoVDBExt.h"
        DESTINATION nanovdb_editor/include/nanovdb_editor
        COMPONENT python_headers
    )

    # Install all required libraries for the Python module
    install(TARGETS
        pnanovdbcompiler
        pnanovdbcompute
        pnanovdbfileformat
        pnanovdbeditor
        DESTINATION nanovdb_editor/lib
        COMPONENT python_libs
    )

    # Install Slang libraries
    if(Slang_ADDED)
        if(WIN32)
            install(FILES
                "${Slang_SOURCE_DIR}/bin/slang${CMAKE_SHARED_LIBRARY_SUFFIX}"
                "${Slang_SOURCE_DIR}/bin/slang-glslang${CMAKE_SHARED_LIBRARY_SUFFIX}"
                DESTINATION nanovdb_editor/lib
                COMPONENT python_libs
            )
            if(SLANG_LLVM_EXISTS)
                install(FILES
                    "${Slang_SOURCE_DIR}/bin/slang-llvm${CMAKE_SHARED_LIBRARY_SUFFIX}"
                    DESTINATION nanovdb_editor/lib
                    COMPONENT python_libs
                )
            endif()
        else()
            install(FILES
                "${Slang_SOURCE_DIR}/lib/libslang${CMAKE_SHARED_LIBRARY_SUFFIX}"
                "${Slang_SOURCE_DIR}/lib/libslang-glslang${CMAKE_SHARED_LIBRARY_SUFFIX}"
                DESTINATION nanovdb_editor/lib
                COMPONENT python_libs
            )
            if(SLANG_LLVM_EXISTS)
                install(FILES
                    "${Slang_SOURCE_DIR}/lib/libslang-llvm${CMAKE_SHARED_LIBRARY_SUFFIX}"
                    DESTINATION nanovdb_editor/lib
                    COMPONENT python_libs
                )
            endif()
        endif()
    endif()

    # Install shader files
    foreach(SHADER_DIR ${SHADER_DIRECTORIES})
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${SHADER_DIR}/shaders")
            install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/${SHADER_DIR}/shaders/"
                DESTINATION nanovdb_editor/shaders/${SHADER_DIR}
                COMPONENT python_shaders
                FILES_MATCHING PATTERN "*"
            )
        endif()
    endforeach()

    # Install includes
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/${INCLUDES_FILE}"
        DESTINATION nanovdb_editor/shaders
        COMPONENT python_shaders
    )

    # Install nanovdb headers to shaders directory
    install(FILES
        "${nanovdb_SOURCE_DIR}/nanovdb/nanovdb/PNanoVDB.h"
        DESTINATION nanovdb_editor/shaders/nanovdb
        COMPONENT python_shaders
    )
    install(FILES
        "nanovdb_editor/PNanoVDBExt.h"
        DESTINATION nanovdb_editor/shaders/nanovdb_editor
        COMPONENT python_shaders
    )

    # Install GLFW DLL files that were previously copied
    if(WIN32 AND TARGET glfw)
        install(FILES "${CMAKE_BINARY_DIR}/$<CONFIG>/glfw3.dll"
            DESTINATION nanovdb_editor/lib
            COMPONENT python_libs
        )
    endif()

    if(TARGET vulkan)
        install(FILES
            $<TARGET_FILE:vulkan>
            DESTINATION nanovdb_editor/lib
            COMPONENT python_libs
        )
        if(UNIX AND NOT APPLE)
            # Also install Linux-friendly names alongside the versioned loader
            install(FILES $<TARGET_FILE:vulkan>
                DESTINATION nanovdb_editor/lib
                COMPONENT python_libs
                RENAME libvulkan.so.1)
            install(FILES $<TARGET_FILE:vulkan>
                DESTINATION nanovdb_editor/lib
                COMPONENT python_libs
                RENAME libvulkan.so)
        endif()
    endif()
endif()

if (NOT SKBUILD)
    message(STATUS "Building NanoVDB Editor Applications...")

    add_custom_target(pnanovdbeditor_prebuild ALL
        COMMAND ${CMAKE_COMMAND} -E echo "Preparing directories..."
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/${INCLUDES_FILE}" "${PROJECT_BINARY_DIR}/${SHADERS_DIR_NAME}/${INCLUDES_FILE}"
    )

    if(TARGET copy_vulkan_loader)
        add_dependencies(pnanovdbeditor_prebuild copy_vulkan_loader)
    endif()

    if(TARGET copy_slang_libs)
        add_dependencies(pnanovdbeditor_prebuild copy_slang_libs)
    endif()

    foreach(SHADER_DIR ${SHADER_DIRECTORIES})
        build_time_link_shader(pnanovdbeditor_prebuild "${SHADER_DIR}")
    endforeach()

    build_time_link_nanovdb(pnanovdbeditor_prebuild "PNanoVDB.h")
    build_time_link_nanovdb_editor(pnanovdbeditor_prebuild "PNanoVDBExt.h")
    build_time_link(pnanovdbeditor_prebuild "${DATA_DIR}" "${PROJECT_BINARY_DIR}/data")

    # Create links for python debug purposes (need to be deleted when building with scikit-build)
    build_time_link(pnanovdbeditor_prebuild "${CMAKE_BINARY_DIR}/$<CONFIG>" "${NANOVDB_EDITOR_PYTHON_LIB_DIR}" )
    build_time_link(pnanovdbeditor_prebuild "${SHADERS_DIR}" "${NANOVDB_EDITOR_PYTHON_SHADERS_DIR}" )

    ### PNanoVDB Editor App

    file(GLOB EDITOR_APP_SOURCE_FILES "app/*.cpp")

    create_nanovdb_executable(pnanovdbeditorapp
        SOURCES ${EDITOR_APP_SOURCE_FILES}
        INCLUDES
            ./
            ./app
            ./editor
            ${nanovdb_SOURCE_DIR}/nanovdb
            ${imgui_SOURCE_DIR}
            ${argparse_SOURCE_DIR}/include
        LIBS
            pnanovdbcompiler
            pnanovdbcompute
            pnanovdbfileformat
            pnanovdbeditor
        DEFINITIONS
            TEST_NODE2
    )

    add_dependencies(pnanovdbeditorapp pnanovdbeditor_prebuild)

    if (PNANOVDB_EDITOR_DIST_PACKAGE)
        create_distribution_package(pnanovdbeditorapp)
    endif()

    set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT pnanovdbeditorapp)

    ### PNanoVDB Editor Test App

    create_nanovdb_executable(pnanovdbeditortestapp
        SOURCES test/main.cpp
        INCLUDES
            ./
            ./test
            ${nanovdb_SOURCE_DIR}/nanovdb
            ${Slang_SOURCE_DIR}/include
            ${argparse_SOURCE_DIR}/include
        LIBS
            pnanovdbcompiler
            pnanovdbcompute
            pnanovdbfileformat
            pnanovdbeditor
            $<$<BOOL:${NANOVDB_EDITOR_USE_H264}>:openh264>
    )

    add_dependencies(pnanovdbeditortestapp pnanovdbeditor_prebuild)
    add_dependencies(pnanovdbeditortestapp openh264_build)
endif()

if (NOT WIN32)
    # Symlink the latest compile_commands.json from the build dir to the source dir for clangd
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E create_symlink
            ${CMAKE_BINARY_DIR}/compile_commands.json
            ${CMAKE_SOURCE_DIR}/compile_commands.json
    )
endif()
