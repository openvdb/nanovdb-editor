# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: Apache-2.0

if(NOT DEFINED LINK_PATH)
    message(FATAL_ERROR "LINK_PATH is required")
endif()

if(NOT DEFINED SOURCE_PATH)
    message(FATAL_ERROR "SOURCE_PATH is required")
endif()

if(NOT DEFINED LINK_KIND)
    message(FATAL_ERROR "LINK_KIND is required")
endif()

# Remove broken or stale symlinks so the subsequent create_symlink step can
# recreate them without touching valid links. If an older build left a real
# file or directory behind at the target location, remove it so the symlink can
# be recreated.
if(IS_SYMLINK "${LINK_PATH}")
    file(READ_SYMLINK "${LINK_PATH}" CURRENT_TARGET)
    if(NOT CURRENT_TARGET STREQUAL SOURCE_PATH OR NOT EXISTS "${LINK_PATH}")
        file(REMOVE "${LINK_PATH}")
    endif()
elseif(EXISTS "${LINK_PATH}")
    if(LINK_KIND STREQUAL "directory")
        file(REMOVE_RECURSE "${LINK_PATH}")
    elseif(LINK_KIND STREQUAL "file")
        file(REMOVE "${LINK_PATH}")
    else()
        message(FATAL_ERROR "Unsupported LINK_KIND: ${LINK_KIND}")
    endif()
endif()
