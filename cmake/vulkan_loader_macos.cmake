# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: Apache-2.0

if(NOT DEFINED VULKAN_LOADER_DEST_DIR)
    message(FATAL_ERROR "VULKAN_LOADER_DEST_DIR must be defined")
endif()

if(NOT DEFINED VULKAN_LOADER_DYLIB_SUFFIX)
    set(VULKAN_LOADER_DYLIB_SUFFIX ".dylib")
endif()

set(_vulkan_loader_source "")

if(DEFINED VULKAN_LOADER_SOURCE_FILE AND EXISTS "${VULKAN_LOADER_SOURCE_FILE}")
    set(_vulkan_loader_source "${VULKAN_LOADER_SOURCE_FILE}")
elseif(DEFINED VULKAN_LOADER_SOURCE_DIR AND IS_DIRECTORY "${VULKAN_LOADER_SOURCE_DIR}")
    file(GLOB _vulkan_loader_candidates
        "${VULKAN_LOADER_SOURCE_DIR}/libvulkan*${VULKAN_LOADER_DYLIB_SUFFIX}"
    )
    list(SORT _vulkan_loader_candidates)

    foreach(_candidate IN LISTS _vulkan_loader_candidates)
        get_filename_component(_candidate_name "${_candidate}" NAME)
        if(NOT _candidate_name STREQUAL "libvulkan${VULKAN_LOADER_DYLIB_SUFFIX}"
           AND NOT _candidate_name STREQUAL "libvulkan.1${VULKAN_LOADER_DYLIB_SUFFIX}")
            set(_vulkan_loader_source "${_candidate}")
            break()
        endif()
    endforeach()

    if(NOT _vulkan_loader_source AND _vulkan_loader_candidates)
        list(GET _vulkan_loader_candidates 0 _vulkan_loader_source)
    endif()
endif()

if(NOT _vulkan_loader_source)
    message(FATAL_ERROR "Could not locate a macOS Vulkan loader to copy")
endif()

file(MAKE_DIRECTORY "${VULKAN_LOADER_DEST_DIR}")

get_filename_component(_vulkan_loader_name "${_vulkan_loader_source}" NAME)
set(_vulkan_loader_targets
    "${_vulkan_loader_name}"
    "libvulkan.1${VULKAN_LOADER_DYLIB_SUFFIX}"
    "libvulkan${VULKAN_LOADER_DYLIB_SUFFIX}"
)

foreach(_target_name IN LISTS _vulkan_loader_targets)
    set(_target_path "${VULKAN_LOADER_DEST_DIR}/${_target_name}")
    if(_target_path STREQUAL _vulkan_loader_source)
        message(STATUS "macOS Vulkan loader already present: ${_target_name}")
    else()
        file(COPY_FILE "${_vulkan_loader_source}" "${_target_path}" ONLY_IF_DIFFERENT)
        if(_target_name STREQUAL _vulkan_loader_name)
            message(STATUS "Installed macOS Vulkan loader: ${_target_name}")
        else()
            message(STATUS "Installed macOS Vulkan loader alias: ${_target_name}")
        endif()
    endif()
endforeach()
