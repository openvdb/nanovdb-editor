cmake_minimum_required(VERSION 3.15)
project(NanovdbEditorTest LANGUAGES CXX)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/get_nanovdb_editor.cmake)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the type of build." FORCE)
endif()

set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g -O0")

add_executable(nanovdb_editor_test
    main.cpp
)

# Add NOMINMAX definition to prevent Windows min/max macro conflicts
target_compile_definitions(nanovdb_editor_test PRIVATE NOMINMAX)

target_include_directories(nanovdb_editor_test PRIVATE
    ${nanovdb_SOURCE_DIR}/nanovdb
)

target_link_libraries(nanovdb_editor_test PRIVATE
    nanovdb_editor::nanovdb_editor
)

if(UNIX AND NOT APPLE)
    target_link_libraries(nanovdb_editor_test PRIVATE dl)
endif()

set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT nanovdb_editor_test)
