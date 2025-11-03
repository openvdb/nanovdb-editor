cmake_minimum_required(VERSION 3.15)
project(NanovdbEditorTest LANGUAGES CXX)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the type of build." FORCE)
endif()

set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g -O0")

if(WIN32)
    # Use local build instead of Python package
    set(NANOVDB_EDITOR_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
    set(NANOVDB_EDITOR_BUILD_DIR "${NANOVDB_EDITOR_ROOT}/build")
    set(NANOVDB_EDITOR_INCLUDE_DIR "${NANOVDB_EDITOR_ROOT}/nanovdb_editor")
    set(NANOVDB_EDITOR_LIB_DIR "${NANOVDB_EDITOR_BUILD_DIR}/$<CONFIG>")

    message(STATUS "Using local build (Windows):")
    message(STATUS "  NANOVDB_EDITOR_ROOT: ${NANOVDB_EDITOR_ROOT}")
    message(STATUS "  NANOVDB_EDITOR_BUILD_DIR: ${NANOVDB_EDITOR_BUILD_DIR}")
    message(STATUS "  NANOVDB_EDITOR_LIB_DIR: ${NANOVDB_EDITOR_LIB_DIR}")
else()
    # Use Python package on non-Windows platforms
    include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/get_nanovdb_editor.cmake)
endif()

add_executable(nanovdb_editor_test
    main.cpp
)

# Add NOMINMAX definition to prevent Windows min/max macro conflicts
target_compile_definitions(nanovdb_editor_test PRIVATE NOMINMAX)

if(WIN32)
    # Include directories from local build
    target_include_directories(nanovdb_editor_test PRIVATE
        ${NANOVDB_EDITOR_INCLUDE_DIR}
        ${NANOVDB_EDITOR_ROOT}
        ${NANOVDB_EDITOR_BUILD_DIR}/shaders
    )

    # Link against local build libraries using generator expressions for multi-config
    if(CMAKE_CONFIGURATION_TYPES)
        # Multi-config generator - use generator expressions
        target_link_libraries(nanovdb_editor_test PRIVATE
            ${NANOVDB_EDITOR_BUILD_DIR}/lib/$<CONFIG>/pnanovdbcompiler.lib
            ${NANOVDB_EDITOR_BUILD_DIR}/lib/$<CONFIG>/pnanovdbcompute.lib
            ${NANOVDB_EDITOR_BUILD_DIR}/lib/$<CONFIG>/pnanovdbfileformat.lib
            ${NANOVDB_EDITOR_BUILD_DIR}/lib/$<CONFIG>/pnanovdbeditor.lib
        )
    else()
        # Single-config generator
        target_link_libraries(nanovdb_editor_test PRIVATE
            ${NANOVDB_EDITOR_BUILD_DIR}/lib/${CMAKE_BUILD_TYPE}/pnanovdbcompiler.lib
            ${NANOVDB_EDITOR_BUILD_DIR}/lib/${CMAKE_BUILD_TYPE}/pnanovdbcompute.lib
            ${NANOVDB_EDITOR_BUILD_DIR}/lib/${CMAKE_BUILD_TYPE}/pnanovdbfileformat.lib
            ${NANOVDB_EDITOR_BUILD_DIR}/lib/${CMAKE_BUILD_TYPE}/pnanovdbeditor.lib
        )
    endif()
else()
    # Use Python package includes and libraries on non-Windows
    target_include_directories(nanovdb_editor_test PRIVATE
        ${nanovdb_SOURCE_DIR}/nanovdb
    )

    target_link_libraries(nanovdb_editor_test PRIVATE
        nanovdb_editor::nanovdb_editor
    )
endif()

if(UNIX AND NOT APPLE)
    target_link_libraries(nanovdb_editor_test PRIVATE dl)
endif()

if(WIN32)
    # On Windows, create hard links for DLLs to the executable directory for runtime
    set(DLL_NAMES
        pnanovdbcompiler.dll
        pnanovdbcompute.dll
        pnanovdbfileformat.dll
        pnanovdbeditor.dll
        glfw3.dll
        slang.dll
        slang-glslang.dll
        slang-llvm.dll
        vulkan-1.dll
    )

    # Create hard links for DLLs using PowerShell (handles errors gracefully)
    foreach(DLL_NAME ${DLL_NAMES})
        add_custom_command(TARGET nanovdb_editor_test POST_BUILD
            COMMAND cmd /c "powershell -ExecutionPolicy Bypass -Command \"try { if (Test-Path '$<TARGET_FILE_DIR:nanovdb_editor_test>/${DLL_NAME}') { Remove-Item -Force '$<TARGET_FILE_DIR:nanovdb_editor_test>/${DLL_NAME}' }; New-Item -ItemType HardLink -Path '$<TARGET_FILE_DIR:nanovdb_editor_test>/${DLL_NAME}' -Target '${NANOVDB_EDITOR_BUILD_DIR}/$<CONFIG>/${DLL_NAME}' -Force | Out-Null } catch {}\" & exit /b 0"
            COMMENT "Creating hard link for ${DLL_NAME}"
            COMMAND_EXPAND_LISTS
        )
    endforeach()

    # Copy shaders directory from local build to one level above executable (build/shaders not build/Debug/shaders)
    set(SHADERS_SOURCE_DIR "${NANOVDB_EDITOR_BUILD_DIR}/shaders")
    set(SHADERS_DEST_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
    if(EXISTS "${SHADERS_SOURCE_DIR}")
        add_custom_command(TARGET nanovdb_editor_test POST_BUILD
            COMMAND cmd /c "powershell -ExecutionPolicy Bypass -Command \"try { if (Test-Path '${SHADERS_DEST_DIR}') { Remove-Item -Recurse -Force '${SHADERS_DEST_DIR}' }; Copy-Item -Path '${SHADERS_SOURCE_DIR}' -Destination '${CMAKE_CURRENT_BINARY_DIR}' -Recurse -Force | Out-Null } catch {}\" & exit /b 0"
            COMMENT "Copying shaders directory from local build"
        )
    endif()
endif()

set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT nanovdb_editor_test)
