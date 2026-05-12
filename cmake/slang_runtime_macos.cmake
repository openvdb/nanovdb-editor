# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: Apache-2.0

if(NOT DEFINED SLANG_RUNTIME_SOURCE_DIR OR SLANG_RUNTIME_SOURCE_DIR STREQUAL "")
    message(FATAL_ERROR "SLANG_RUNTIME_SOURCE_DIR must be set")
endif()

if(NOT DEFINED SLANG_RUNTIME_DEST_DIR OR SLANG_RUNTIME_DEST_DIR STREQUAL "")
    message(FATAL_ERROR "SLANG_RUNTIME_DEST_DIR must be set")
endif()

if(NOT DEFINED SLANG_RUNTIME_DYLIB_SUFFIX OR SLANG_RUNTIME_DYLIB_SUFFIX STREQUAL "")
    set(SLANG_RUNTIME_DYLIB_SUFFIX ".dylib")
endif()

file(MAKE_DIRECTORY "${SLANG_RUNTIME_DEST_DIR}")

# The official macOS Slang packages ship a mix of:
# - unversioned compatibility symlinks like libslang.dylib
# - versioned payloads like libslang-compiler.0.<ver>.dylib
# - additional runtime deps like libslang-glslang-<ver>.dylib, libslang-rt, libgfx
file(GLOB _slang_runtime_candidates
    "${SLANG_RUNTIME_SOURCE_DIR}/libslang*${SLANG_RUNTIME_DYLIB_SUFFIX}*"
    "${SLANG_RUNTIME_SOURCE_DIR}/libgfx*${SLANG_RUNTIME_DYLIB_SUFFIX}*"
)

list(REMOVE_DUPLICATES _slang_runtime_candidates)

foreach(_runtime_file IN LISTS _slang_runtime_candidates)
    if(IS_DIRECTORY "${_runtime_file}")
        continue()
    endif()

    get_filename_component(_runtime_name "${_runtime_file}" NAME)
    if(_runtime_name MATCHES "\\.dSYM($|/)")
        continue()
    endif()

    set(_runtime_dst "${SLANG_RUNTIME_DEST_DIR}/${_runtime_name}")
    file(COPY_FILE "${_runtime_file}" "${_runtime_dst}" ONLY_IF_DIFFERENT)
    message(STATUS "Installed macOS Slang runtime: ${_runtime_name}")
endforeach()
