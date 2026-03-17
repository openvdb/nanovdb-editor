# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: Apache-2.0

if(NOT DEFINED TARGET_PATH)
    message(FATAL_ERROR "TARGET_PATH is required")
endif()

if(NOT DEFINED SOURCE_PATH)
    message(FATAL_ERROR "SOURCE_PATH is required")
endif()

if(NOT DEFINED LINK_KIND)
    message(FATAL_ERROR "LINK_KIND is required")
endif()

# MSBuild passes -DVAR=\"...\" through the generated post-build script, so
# strip one layer of surrounding quotes before testing paths or invoking tools.
set(TARGET_PATH_NORMALIZED "${TARGET_PATH}")
set(SOURCE_PATH_NORMALIZED "${SOURCE_PATH}")
string(REGEX REPLACE "^\"(.*)\"$" "\\1" TARGET_PATH_NORMALIZED "${TARGET_PATH_NORMALIZED}")
string(REGEX REPLACE "^\"(.*)\"$" "\\1" SOURCE_PATH_NORMALIZED "${SOURCE_PATH_NORMALIZED}")

if(LINK_KIND STREQUAL "file" AND EXISTS "${TARGET_PATH_NORMALIZED}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${SOURCE_PATH_NORMALIZED}" "${TARGET_PATH_NORMALIZED}"
        RESULT_VARIABLE COPY_RESULT
    )
    if(NOT COPY_RESULT EQUAL 0)
        message(FATAL_ERROR "Failed to refresh file link target: ${TARGET_PATH_NORMALIZED}")
    endif()
elseif(NOT EXISTS "${TARGET_PATH_NORMALIZED}")
    file(TO_NATIVE_PATH "${TARGET_PATH_NORMALIZED}" TARGET_PATH_NATIVE)
    file(TO_NATIVE_PATH "${SOURCE_PATH_NORMALIZED}" SOURCE_PATH_NATIVE)

    # Avoid a trailing backslash before the closing quote in mklink commands.
    string(REGEX REPLACE "[/\\\\]+$" "" TARGET_PATH_NATIVE "${TARGET_PATH_NATIVE}")
    string(REGEX REPLACE "[/\\\\]+$" "" SOURCE_PATH_NATIVE "${SOURCE_PATH_NATIVE}")

    if(LINK_KIND STREQUAL "file")
        execute_process(
            COMMAND cmd /c mklink /H "${TARGET_PATH_NATIVE}" "${SOURCE_PATH_NATIVE}"
            RESULT_VARIABLE LINK_RESULT
        )

        if(NOT LINK_RESULT EQUAL 0 AND NOT EXISTS "${TARGET_PATH_NORMALIZED}")
            execute_process(
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${SOURCE_PATH_NORMALIZED}" "${TARGET_PATH_NORMALIZED}"
                RESULT_VARIABLE COPY_RESULT
            )
            if(NOT COPY_RESULT EQUAL 0)
                message(FATAL_ERROR "Failed to create or copy file link target: ${TARGET_PATH_NORMALIZED}")
            endif()
        endif()
    elseif(LINK_KIND STREQUAL "directory")
        execute_process(
            COMMAND cmd /c mklink /J "${TARGET_PATH_NATIVE}" "${SOURCE_PATH_NATIVE}"
            RESULT_VARIABLE LINK_RESULT
        )

        if(NOT LINK_RESULT EQUAL 0 AND NOT EXISTS "${TARGET_PATH_NORMALIZED}")
            execute_process(
                COMMAND "${CMAKE_COMMAND}" -E copy_directory "${SOURCE_PATH_NORMALIZED}" "${TARGET_PATH_NORMALIZED}"
                RESULT_VARIABLE COPY_RESULT
            )
            if(NOT COPY_RESULT EQUAL 0)
                message(FATAL_ERROR "Failed to create or copy directory link target: ${TARGET_PATH_NORMALIZED}")
            endif()
        endif()
    else()
        message(FATAL_ERROR "Unsupported LINK_KIND: ${LINK_KIND}")
    endif()
endif()
