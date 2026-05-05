cmake_minimum_required(VERSION 3.15)
project(NanovdbEditorTest LANGUAGES CXX)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the type of build." FORCE)
endif()

set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g -O0")

set(NANOVDB_EDITOR_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(NANOVDB_EDITOR_BUILD_DIR "${NANOVDB_EDITOR_ROOT}/build")
set(NANOVDB_EDITOR_INCLUDE_DIR "${NANOVDB_EDITOR_ROOT}/nanovdb_editor")

if(WIN32)
    set(_nve_local_build_default ON)
elseif(EXISTS "${NANOVDB_EDITOR_BUILD_DIR}/lib")
    set(_nve_local_build_default ON)
else()
    set(_nve_local_build_default OFF)
endif()
option(NANOVDB_EDITOR_TEST_USE_LOCAL_BUILD "Link nanovdb_editor_test against a local build tree instead of the installed Python package" ${_nve_local_build_default})

if(NANOVDB_EDITOR_TEST_USE_LOCAL_BUILD)
    message(STATUS "nanovdb_editor_test using local build:")
    message(STATUS "  NANOVDB_EDITOR_ROOT: ${NANOVDB_EDITOR_ROOT}")
    message(STATUS "  NANOVDB_EDITOR_BUILD_DIR: ${NANOVDB_EDITOR_BUILD_DIR}")
else()
    include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/get_nanovdb_editor.cmake)
endif()

add_executable(nanovdb_editor_test
    main.cpp
)

target_compile_definitions(nanovdb_editor_test PRIVATE NOMINMAX)

if(NANOVDB_EDITOR_TEST_USE_LOCAL_BUILD)
    target_include_directories(nanovdb_editor_test PRIVATE
        ${NANOVDB_EDITOR_INCLUDE_DIR}
        ${NANOVDB_EDITOR_ROOT}
        ${NANOVDB_EDITOR_BUILD_DIR}/shaders
    )

    set(_nve_lib_names pnanovdbcompiler pnanovdbcompute pnanovdbfileformat pnanovdbeditor)

    if(WIN32)
        set(_nve_link_prefix "${CMAKE_IMPORT_LIBRARY_PREFIX}")
        set(_nve_link_suffix "${CMAKE_IMPORT_LIBRARY_SUFFIX}")
    else()
        set(_nve_link_prefix "${CMAKE_SHARED_LIBRARY_PREFIX}")
        set(_nve_link_suffix "${CMAKE_SHARED_LIBRARY_SUFFIX}")
    endif()

    if(CMAKE_CONFIGURATION_TYPES)
        # Multi-config generators (Visual Studio, Xcode): libs are under lib/<Config>/
        set(_nve_lib_subdir "$<CONFIG>/")
    else()
        # Single-config generators (Unix Makefiles, Ninja): libs are directly under lib/
        set(_nve_lib_subdir "")
    endif()

    foreach(_lib ${_nve_lib_names})
        target_link_libraries(nanovdb_editor_test PRIVATE
            "${NANOVDB_EDITOR_BUILD_DIR}/lib/${_nve_lib_subdir}${_nve_link_prefix}${_lib}${_nve_link_suffix}"
        )
    endforeach()
else()
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

if(WIN32 AND NANOVDB_EDITOR_TEST_USE_LOCAL_BUILD)
    # On Windows, create hard links for DLLs to the executable directory for runtime
    set(DLL_NAMES
        pnanovdbcompiler.dll
        pnanovdbcompute.dll
        pnanovdbfileformat.dll
        pnanovdbeditor.dll
        glfw3.dll
        slang.dll
        slang-compiler.dll
        slang-glslang.dll
        slang-llvm.dll
        vulkan-1.dll
    )

    foreach(DLL_NAME ${DLL_NAMES})
        add_custom_command(TARGET nanovdb_editor_test POST_BUILD
            COMMAND cmd /c "powershell -ExecutionPolicy Bypass -Command \"try { if (Test-Path '$<TARGET_FILE_DIR:nanovdb_editor_test>/${DLL_NAME}') { Remove-Item -Force '$<TARGET_FILE_DIR:nanovdb_editor_test>/${DLL_NAME}' }; New-Item -ItemType HardLink -Path '$<TARGET_FILE_DIR:nanovdb_editor_test>/${DLL_NAME}' -Target '${NANOVDB_EDITOR_BUILD_DIR}/$<CONFIG>/${DLL_NAME}' -Force | Out-Null } catch {}\" & exit /b 0"
            COMMENT "Creating hard link for ${DLL_NAME}"
            COMMAND_EXPAND_LISTS
        )
    endforeach()

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
