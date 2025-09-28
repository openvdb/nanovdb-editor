cmake_minimum_required(VERSION 3.15)
project(NanovdbEditorTest LANGUAGES CXX)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/get_nanovdb_editor.cmake)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the type of build." FORCE)
endif()

# Only add GCC/Clang-style debug flags on non-MSVC toolchains
if(NOT MSVC)
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g -O0")
endif()

add_executable(nanovdb_editor_test
    main.cpp
)

# Add NOMINMAX definition to prevent Windows min/max macro conflicts
target_compile_definitions(nanovdb_editor_test PRIVATE NOMINMAX)

target_include_directories(nanovdb_editor_test PRIVATE
    ${NANOVDB_EDITOR_INCLUDE_DIRS}
)

if(MSVC)
    # Avoid Visual Studio auto-appending \\$(Configuration) by using an explicit /LIBPATH
    target_link_options(nanovdb_editor_test PRIVATE
        "/LIBPATH:${NANOVDB_EDITOR_LIB_DIR}"
    )
else()
    target_link_directories(nanovdb_editor_test PRIVATE
        ${NANOVDB_EDITOR_LIB_DIR}
    )
endif()

if(UNIX AND NOT APPLE)
    target_link_libraries(nanovdb_editor_test PRIVATE dl)
endif()

set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT nanovdb_editor_test)

# Ensure DLLs are discoverable at runtime on Windows without explicit linking
if(WIN32)
    # Add the nanovdb_editor lib dir to PATH for the Visual Studio debugger
    # Note: semicolon must be escaped for CMake string
    set(_dbg_env "PATH=${NANOVDB_EDITOR_LIB_DIR}\\;%PATH%")
    set_property(TARGET nanovdb_editor_test PROPERTY VS_DEBUGGER_ENVIRONMENT "${_dbg_env}")
endif()
