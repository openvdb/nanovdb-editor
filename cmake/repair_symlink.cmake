# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: Apache-2.0

if(NOT DEFINED LINK_PATH)
    message(FATAL_ERROR "LINK_PATH is required")
endif()

if(NOT DEFINED SOURCE_PATH)
    message(FATAL_ERROR "SOURCE_PATH is required")
endif()

# Remove broken or stale symlinks so the subsequent create_symlink step can
# recreate them without touching valid links or real directories/files.
if(IS_SYMLINK "${LINK_PATH}")
    file(READ_SYMLINK "${LINK_PATH}" CURRENT_TARGET)
    if(NOT CURRENT_TARGET STREQUAL SOURCE_PATH OR NOT EXISTS "${LINK_PATH}")
        file(REMOVE "${LINK_PATH}")
    endif()
endif()
