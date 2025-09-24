# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: Apache-2.0

# CPM cache directory setup
if(NOT CPM_SOURCE_CACHE OR NOT IS_DIRECTORY "${CPM_SOURCE_CACHE}")
    if(DEFINED ENV{CPM_SOURCE_CACHE})
        message(STATUS "CPM_SOURCE_CACHE defined in environment, using $ENV{CPM_SOURCE_CACHE}")
        set(CPM_SOURCE_CACHE $ENV{CPM_SOURCE_CACHE} CACHE PATH "CPM cache directory" FORCE)
    else()
        message(STATUS "CPM_SOURCE_CACHE not defined, setting to ${CMAKE_SOURCE_DIR}/.cpm_cache")
        set(CPM_SOURCE_CACHE "${CMAKE_SOURCE_DIR}/.cpm_cache" CACHE PATH "Default CPM cache directory" FORCE)
    endif()
    if(NOT IS_DIRECTORY "${CPM_SOURCE_CACHE}")
        file(MAKE_DIRECTORY "${CPM_SOURCE_CACHE}")
    endif()
endif()

message(STATUS "CPM cache directory: ${CPM_SOURCE_CACHE}")

# Download CPM.cmake to the cache directory if it doesn't exist
if(NOT EXISTS ${CPM_SOURCE_CACHE}/CPM.cmake)
  file(
    DOWNLOAD
    https://github.com/cpm-cmake/CPM.cmake/releases/download/v0.40.2/CPM.cmake
    ${CPM_SOURCE_CACHE}/CPM.cmake
    EXPECTED_HASH SHA256=c8cdc32c03816538ce22781ed72964dc864b2a34a310d3b7104812a5ca2d835d
    TIMEOUT 600
    INACTIVITY_TIMEOUT 60
  )
endif()

# Include CPM.cmake from the cache
include("${CPM_SOURCE_CACHE}/CPM.cmake")
