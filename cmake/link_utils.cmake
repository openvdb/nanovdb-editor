# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: Apache-2.0

# CMake utilities for NanoVDB Editor

include_guard(GLOBAL)

function(create_platform_file_link TARGET_NAME SOURCE_FILE TARGET_FILE COMMENT_TEXT)
    get_filename_component(TARGET_DIR "${TARGET_FILE}" DIRECTORY)

    if(WIN32)
        # Windows: Use mklink for files (hard link) with proper path conversion
        file(TO_NATIVE_PATH "${TARGET_FILE}" TARGET_FILE_NATIVE)
        file(TO_NATIVE_PATH "${SOURCE_FILE}" SOURCE_FILE_NATIVE)
        add_custom_command(
            TARGET ${TARGET_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "${TARGET_DIR}"
            COMMAND ${CMAKE_COMMAND} -E remove -f "${TARGET_FILE}"
            COMMAND cmd /c "mklink /H \"${TARGET_FILE_NATIVE}\" \"${SOURCE_FILE_NATIVE}\" || exit 0"
            COMMAND if not exist "${TARGET_FILE}" ${CMAKE_COMMAND} -E copy_if_different "${SOURCE_FILE}" "${TARGET_FILE}"
            COMMENT "${COMMENT_TEXT}"
        )
    else()
        # Linux/Mac: Use symbolic links
        add_custom_command(
            TARGET ${TARGET_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "${TARGET_DIR}"
            COMMAND ${CMAKE_COMMAND} -E remove -f "${TARGET_FILE}"
            COMMAND ${CMAKE_COMMAND} -E create_symlink "${SOURCE_FILE}" "${TARGET_FILE}"
            COMMENT "${COMMENT_TEXT}"
        )
    endif()
endfunction()

function(create_platform_directory_link TARGET_NAME SOURCE_DIR TARGET_DIR COMMENT_TEXT)
    get_filename_component(TARGET_PARENT_DIR "${TARGET_DIR}" DIRECTORY)

    if(WIN32)
        # Windows: Use mklink /J for directories (junction) with proper path conversion
        file(TO_NATIVE_PATH "${TARGET_DIR}" TARGET_DIR_NATIVE)
        file(TO_NATIVE_PATH "${SOURCE_DIR}" SOURCE_DIR_NATIVE)
        add_custom_command(
            TARGET ${TARGET_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "${TARGET_PARENT_DIR}"
            COMMAND ${CMAKE_COMMAND} -E remove_directory "${TARGET_DIR}"
            COMMAND cmd /c "mklink /J \"${TARGET_DIR_NATIVE}\" \"${SOURCE_DIR_NATIVE}\" || exit 0"
            COMMAND if not exist "${TARGET_DIR}" ${CMAKE_COMMAND} -E copy_directory "${SOURCE_DIR}" "${TARGET_DIR}"
            COMMENT "${COMMENT_TEXT}"
        )
    else()
        # Linux/Mac: Use symbolic links
        add_custom_command(
            TARGET ${TARGET_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "${TARGET_PARENT_DIR}"
            COMMAND ${CMAKE_COMMAND} -E remove_directory "${TARGET_DIR}"
            COMMAND ${CMAKE_COMMAND} -E create_symlink "${SOURCE_DIR}" "${TARGET_DIR}"
            COMMENT "${COMMENT_TEXT}"
        )
    endif()
endfunction()

function(build_time_link TARGET_NAME SOURCE_PATH TARGET_PATH)
    create_platform_directory_link(${TARGET_NAME} "${SOURCE_PATH}" "${TARGET_PATH}" "Creating directory link")
endfunction()

function(build_time_link_nanovdb TARGET_NAME NANOVDB_FILE)
    file(MAKE_DIRECTORY "${PROJECT_BINARY_DIR}/${SHADERS_DIR_NAME}/nanovdb")

    set(SOURCE_FILE "${nanovdb_SOURCE_DIR}/nanovdb/nanovdb/${NANOVDB_FILE}")
    set(TARGET_FILE "${PROJECT_BINARY_DIR}/${SHADERS_DIR_NAME}/nanovdb/${NANOVDB_FILE}")

    create_platform_file_link(${TARGET_NAME} "${SOURCE_FILE}" "${TARGET_FILE}" "Creating link for ${NANOVDB_FILE}")
endfunction()

function(build_time_link_nanovdb_editor TARGET_NAME NANOVDB_FILE)
    file(MAKE_DIRECTORY "${PROJECT_BINARY_DIR}/${SHADERS_DIR_NAME}/nanovdb_editor")

    set(SOURCE_FILE "${PROJECT_SOURCE_DIR}/nanovdb_editor/${NANOVDB_FILE}")
    set(TARGET_FILE "${PROJECT_BINARY_DIR}/${SHADERS_DIR_NAME}/nanovdb_editor/${NANOVDB_FILE}")

    create_platform_file_link(${TARGET_NAME} "${SOURCE_FILE}" "${TARGET_FILE}" "Creating link for ${NANOVDB_FILE}")
endfunction()

function(build_time_link_shader TARGET_NAME SHADER_DIR)
    set(SOURCE_DIR "${PROJECT_SOURCE_DIR}/${SHADER_DIR}/shaders")
    set(TARGET_DIR "${PROJECT_BINARY_DIR}/${SHADERS_DIR_NAME}/${SHADER_DIR}")

    create_platform_directory_link(${TARGET_NAME} "${SOURCE_DIR}" "${TARGET_DIR}" "Creating shader link for ${SHADER_DIR}")
endfunction()
