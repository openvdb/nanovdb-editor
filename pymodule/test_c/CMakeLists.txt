cmake_minimum_required(VERSION 3.15)
project(NanoVDBEditorTest C)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/get_nanovdb_editor.cmake)

# Set default build type if not specified
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the type of build." FORCE)
endif()

# Set C standard
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(TEST_COMPILER "Test Compiler" ON)
option(TEST_COMPUTE "Test Compute" ON)
option(TEST_EDITOR "Test Editor" ON)

# Add executable
add_executable(pnanovdbeditortestcapp
    main.c
)

foreach(TEST_OPTION IN ITEMS TEST_COMPILER TEST_COMPUTE TEST_EDITOR)
    if(${TEST_OPTION})
        target_compile_definitions(pnanovdbeditortestcapp PRIVATE ${TEST_OPTION})
    endif()
endforeach()

# Define PNANOVDB_C for C compilation compatibility
target_compile_definitions(pnanovdbeditortestcapp PRIVATE PNANOVDB_C)

set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT pnanovdbeditortestcapp)

# Include directories
target_include_directories(pnanovdbeditortestcapp PRIVATE
    ../../
    ../
    ${NANOVDB_EDITOR_INCLUDE_DIR}
)

# Link libdl on Linux for dynamic loading functionality
if(UNIX AND NOT APPLE)
    target_link_libraries(pnanovdbeditortestcapp PRIVATE dl)
endif()

target_link_libraries(pnanovdbeditortestcapp PRIVATE
    nanovdb_editor::nanovdb_editor
)

# Symlink shaders from python package
add_custom_command(TARGET pnanovdbeditortestcapp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E create_symlink "${NANOVDB_EDITOR_PACKAGE_DIR}/shaders" "${PROJECT_BINARY_DIR}/shaders"
)

# Ensure runtime can find shared libraries shipped in the Python wheel
# 1) Set RPATH to $ORIGIN/lib so it can discover the colocated lib dir
set_target_properties(pnanovdbeditortestcapp PROPERTIES
    INSTALL_RPATH "$ORIGIN/lib"
    BUILD_WITH_INSTALL_RPATH TRUE
)

# 2) Create a local lib/ symlink beside the executable that points to the wheel's lib dir
add_custom_command(TARGET pnanovdbeditortestcapp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E create_symlink "${NANOVDB_EDITOR_LIB_DIR}" "${PROJECT_BINARY_DIR}/lib"
)
