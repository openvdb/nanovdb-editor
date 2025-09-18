# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: Apache-2.0

# CMake utilities for NanoVDB Editor

include_guard(GLOBAL)

function(create_distribution_package TARGET_NAME)
    # Create distribution directories
    set(DIST_DIR "${PROJECT_BINARY_DIR}/dist")
    set(DIST_BIN_DIR "${DIST_DIR}")

    if(WIN32)
        # On Windows, DLLs must be next to the executable
        set(DIST_LIB_DIR "${DIST_DIR}")
    else()
        # On Linux/Mac, use lib subdirectory
        set(DIST_LIB_DIR "${DIST_DIR}/lib")
    endif()

    if(WIN32)
        add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
            # Create distribution directories (Windows: DLLs next to exe)
            COMMAND ${CMAKE_COMMAND} -E make_directory "${DIST_BIN_DIR}"

            # Copy executable to bin directory
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:${TARGET_NAME}>" "${DIST_BIN_DIR}/"

            # Copy pnanovdb libraries to same directory as executable
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:pnanovdbcompiler>" "${DIST_LIB_DIR}/"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:pnanovdbcompute>" "${DIST_LIB_DIR}/"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:pnanovdbfileformat>" "${DIST_LIB_DIR}/"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:pnanovdbeditor>" "${DIST_LIB_DIR}/"

            # Copy Slang libraries (needed by pnanovdbcompiler)
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_BINARY_DIR}/$<CONFIG>/slang${CMAKE_SHARED_LIBRARY_SUFFIX}" "${DIST_LIB_DIR}/"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_BINARY_DIR}/$<CONFIG>/slang-glslang${CMAKE_SHARED_LIBRARY_SUFFIX}" "${DIST_LIB_DIR}/"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_BINARY_DIR}/$<CONFIG>/slang-llvm${CMAKE_SHARED_LIBRARY_SUFFIX}" "${DIST_LIB_DIR}/"

            # Copy GLFW library (needed on Windows)
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_BINARY_DIR}/$<CONFIG>/glfw3.dll" "${DIST_LIB_DIR}/"

            # Copy shaders directory
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${PROJECT_BINARY_DIR}/shaders" "${DIST_BIN_DIR}/shaders"

            # Copy data directory
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${PROJECT_BINARY_DIR}/data" "${DIST_BIN_DIR}/data"

            COMMENT "Creating distribution package for ${TARGET_NAME}"
        )
    else()
        add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
            # Create distribution directories (Linux/Mac: DLLs in lib subdirectory)
            COMMAND ${CMAKE_COMMAND} -E make_directory "${DIST_BIN_DIR}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${DIST_LIB_DIR}"

            # Copy executable to bin directory
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:${TARGET_NAME}>" "${DIST_BIN_DIR}/"

            # Copy pnanovdb libraries to lib subdirectory
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:pnanovdbcompiler>" "${DIST_LIB_DIR}/"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:pnanovdbcompute>" "${DIST_LIB_DIR}/"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:pnanovdbfileformat>" "${DIST_LIB_DIR}/"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:pnanovdbeditor>" "${DIST_LIB_DIR}/"

            # Copy Slang libraries (needed by pnanovdbcompiler)
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/libslang${CMAKE_SHARED_LIBRARY_SUFFIX}" "${DIST_LIB_DIR}/"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/libslang-glslang${CMAKE_SHARED_LIBRARY_SUFFIX}" "${DIST_LIB_DIR}/"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/libslang-llvm${CMAKE_SHARED_LIBRARY_SUFFIX}" "${DIST_LIB_DIR}/"

            # Copy shaders directory
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${PROJECT_BINARY_DIR}/shaders" "${DIST_BIN_DIR}/shaders"

            # Copy data directory
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${PROJECT_BINARY_DIR}/data" "${DIST_BIN_DIR}/data"

            COMMENT "Creating distribution package for ${TARGET_NAME}"
        )
    endif()
endfunction()
