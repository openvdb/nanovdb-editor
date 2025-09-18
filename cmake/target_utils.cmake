# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: Apache-2.0

# Target Configuration Utilities for NanoVDB Editor

function(create_nanovdb_library TARGET_NAME)
    set(options)
    set(oneValueArgs)
    set(multiValueArgs SOURCES HEADERS INCLUDES LIBS DEFINITIONS)

    cmake_parse_arguments(ARGS "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    add_library(${TARGET_NAME} SHARED ${ARGS_SOURCES})

    if(ARGS_INCLUDES)
        target_include_directories(${TARGET_NAME} PRIVATE ${ARGS_INCLUDES})
    endif()

    if(ARGS_HEADERS)
        target_sources(${TARGET_NAME} PRIVATE ${ARGS_HEADERS})
    endif()

    if(ARGS_LIBS)
        target_link_libraries(${TARGET_NAME} PRIVATE ${ARGS_LIBS})
    endif()

    if(ARGS_DEFINITIONS)
        target_compile_definitions(${TARGET_NAME} PRIVATE ${ARGS_DEFINITIONS})
    endif()

    set_target_properties(${TARGET_NAME} PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        POSITION_INDEPENDENT_CODE ON
    )

    if(APPLE)
        set_target_properties(${TARGET_NAME} PROPERTIES
            BUILD_WITH_INSTALL_RPATH TRUE
            INSTALL_RPATH "@loader_path"
            INSTALL_NAME_DIR "@rpath"
        )
    else()
        set_target_properties(${TARGET_NAME} PROPERTIES
            BUILD_WITH_INSTALL_RPATH TRUE
            INSTALL_RPATH "$ORIGIN;$ORIGIN/lib"
        )
    endif()
endfunction()

function(create_nanovdb_executable TARGET_NAME)
    set(options)
    set(oneValueArgs)
    set(multiValueArgs SOURCES INCLUDES LIBS DEFINITIONS)

    cmake_parse_arguments(ARGS "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    add_executable(${TARGET_NAME} ${ARGS_SOURCES})

    if(ARGS_INCLUDES)
        target_include_directories(${TARGET_NAME} PRIVATE ${ARGS_INCLUDES})
    endif()

    if(ARGS_LIBS)
        target_link_libraries(${TARGET_NAME} PRIVATE ${ARGS_LIBS})
    endif()

    # Link libdl on Linux for dynamic loading functionality
    if(UNIX AND NOT APPLE)
        target_link_libraries(${TARGET_NAME} PRIVATE dl)
    endif()

    if(ARGS_DEFINITIONS)
        target_compile_definitions(${TARGET_NAME} PRIVATE ${ARGS_DEFINITIONS})
    endif()

    set_target_properties(${TARGET_NAME} PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )

    if(APPLE)
        set_target_properties(${TARGET_NAME} PROPERTIES
            BUILD_WITH_INSTALL_RPATH TRUE
            INSTALL_RPATH "@loader_path/lib;/usr/local/lib"
            INSTALL_NAME_DIR "@rpath"
        )
    else()
        set_target_properties(${TARGET_NAME} PROPERTIES
            BUILD_WITH_INSTALL_RPATH TRUE
            INSTALL_RPATH "$ORIGIN/lib"
        )
    endif()

    target_compile_definitions(${TARGET_NAME} PRIVATE
        NOMINMAX
    )
endfunction()

function(add_sanitizer_support TARGET_NAME)
    if(NANOVDB_EDITOR_ENABLE_SANITIZERS AND CMAKE_BUILD_TYPE STREQUAL "Debug")
        if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            target_link_options(${TARGET_NAME} PRIVATE
                -fsanitize=address,undefined
            )
        endif()
    endif()
endfunction()
