# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: Apache-2.0

# CMake utilities for NanoVDB Editor

include_guard(GLOBAL)

function(create_platform_file_link TARGET_NAME SOURCE_FILE TARGET_FILE COMMENT_TEXT)
    get_filename_component(TARGET_DIR "${TARGET_FILE}" DIRECTORY)

    if(WIN32)
        # Windows: Use mklink for files (hard link) and fall back to a copy when
        # link creation is unavailable. Keep the whole decision in a CMake script
        # so MSBuild does not have to parse fragile inline cmd conditionals.
        add_custom_command(
            TARGET ${TARGET_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "${TARGET_DIR}"
            COMMAND ${CMAKE_COMMAND}
                -DTARGET_PATH="${TARGET_FILE}"
                -DSOURCE_PATH="${SOURCE_FILE}"
                -DLINK_KIND=file
                -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ensure_windows_link.cmake"
            COMMENT "${COMMENT_TEXT}"
            VERBATIM
        )
    else()
        # Linux/Mac: Keep a valid symlink, but repair broken or stale ones.
        add_custom_command(
            TARGET ${TARGET_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "${TARGET_DIR}"
            COMMAND ${CMAKE_COMMAND}
                -DLINK_PATH="${TARGET_FILE}"
                -DSOURCE_PATH="${SOURCE_FILE}"
                -DLINK_KIND=file
                -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/repair_symlink.cmake"
            COMMAND test -e "${TARGET_FILE}" || ${CMAKE_COMMAND} -E create_symlink "${SOURCE_FILE}" "${TARGET_FILE}"
            COMMENT "${COMMENT_TEXT}"
            VERBATIM
        )
    endif()
endfunction()

function(create_platform_directory_link TARGET_NAME SOURCE_DIR TARGET_DIR COMMENT_TEXT)
    get_filename_component(TARGET_PARENT_DIR "${TARGET_DIR}" DIRECTORY)

    if(WIN32)
        # Windows: Use mklink /J for directories and fall back to a copied
        # directory when junction creation is unavailable.
        add_custom_command(
            TARGET ${TARGET_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "${TARGET_PARENT_DIR}"
            COMMAND ${CMAKE_COMMAND}
                -DTARGET_PATH="${TARGET_DIR}"
                -DSOURCE_PATH="${SOURCE_DIR}"
                -DLINK_KIND=directory
                -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ensure_windows_link.cmake"
            COMMENT "${COMMENT_TEXT}"
            VERBATIM
        )
    else()
        # Linux/Mac: Keep a valid symlink, but repair broken or stale ones.
        add_custom_command(
            TARGET ${TARGET_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "${TARGET_PARENT_DIR}"
            COMMAND ${CMAKE_COMMAND}
                -DLINK_PATH="${TARGET_DIR}"
                -DSOURCE_PATH="${SOURCE_DIR}"
                -DLINK_KIND=directory
                -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/repair_symlink.cmake"
            COMMAND test -e "${TARGET_DIR}" || ${CMAKE_COMMAND} -E create_symlink "${SOURCE_DIR}" "${TARGET_DIR}"
            COMMENT "${COMMENT_TEXT}"
            VERBATIM
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
