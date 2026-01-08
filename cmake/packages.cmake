# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: Apache-2.0

# CPM Package Management for NanoVDB Editor

if(NANOVDB_EDITOR_FORCE_REBUILD_DEPS AND EXISTS "$ENV{CPM_SOURCE_CACHE}")
    message(STATUS "Forcing rebuild: Cleaning CPM cache at $ENV{CPM_SOURCE_CACHE}")
    file(REMOVE_RECURSE "$ENV{CPM_SOURCE_CACHE}")
    file(MAKE_DIRECTORY "$ENV{CPM_SOURCE_CACHE}")
    # Reset the option so it doesn't clean every time
    set(NANOVDB_EDITOR_FORCE_REBUILD_DEPS OFF CACHE BOOL "Force rebuild all dependencies (clears cache)" FORCE)
endif()

# Set global static build preference - all dependencies should be static
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build shared libraries" FORCE)
set(BUILD_STATIC_LIBS ON CACHE BOOL "Build static libraries" FORCE)

# Core dependencies
CPMAddPackage(
    NAME nanovdb
    GITHUB_REPOSITORY AcademySoftwareFoundation/openvdb
    GIT_TAG 5f0432b3387c169212a009ddaa05fdd703016549
    DOWNLOAD_ONLY YES
)

set(ZLIB_VERSION 1.3.1)
CPMAddPackage(
    NAME zlib
    URL
        https://github.com/madler/zlib/archive/refs/tags/v${ZLIB_VERSION}.tar.gz
        https://zlib.net/zlib-${ZLIB_VERSION}.tar.gz
        https://www.zlib.net/fossils/zlib-${ZLIB_VERSION}.tar.gz
    VERSION ${ZLIB_VERSION}
    OPTIONS
        "SKIP_INSTALL_LIBRARIES OFF"
        "SKIP_INSTALL_ALL OFF"
        "ZLIB_BUILD_EXAMPLES OFF"
)

# Blosc depends on zlib, so ensure it's available and configure paths
if(zlib_ADDED)
    set(ZLIB_ROOT ${zlib_SOURCE_DIR})
    set(ZLIB_INCLUDE_DIR "${zlib_SOURCE_DIR};${zlib_BINARY_DIR}")
    set(ZLIB_LIBRARY zlibstatic)
    # Ensure zlib is configured properly for blosc
    if(TARGET zlibstatic)
        set_target_properties(zlibstatic PROPERTIES POSITION_INDEPENDENT_CODE ON)
    endif()

    # Set ZLIB cache variables immediately for packages that use find_package(ZLIB)
    set(ZLIB_FOUND TRUE CACHE BOOL "ZLIB found" FORCE)
    set(ZLIB_LIBRARIES zlibstatic CACHE STRING "ZLIB libraries" FORCE)
    set(ZLIB_INCLUDE_DIRS "${zlib_SOURCE_DIR};${zlib_BINARY_DIR}" CACHE STRING "ZLIB include directories" FORCE)
    set(ZLIB_VERSION_STRING ${ZLIB_VERSION} CACHE STRING "ZLIB version" FORCE)
    set(ZLIB_LIBRARY zlibstatic CACHE STRING "ZLIB library target" FORCE)
    set(ZLIB_INCLUDE_DIR "${zlib_SOURCE_DIR};${zlib_BINARY_DIR}" CACHE STRING "ZLIB include directory" FORCE)
endif()

set(BLOSC_VERSION 1.21.4)
CPMAddPackage(
    NAME blosc
    URL
        https://github.com/Blosc/c-blosc/archive/refs/tags/v${BLOSC_VERSION}.tar.gz
        https://codeload.github.com/Blosc/c-blosc/tar.gz/refs/tags/v${BLOSC_VERSION}
    VERSION ${BLOSC_VERSION}
    OPTIONS
        "BUILD_TESTS OFF"
        "BUILD_BENCHMARKS OFF"
        "BUILD_FUZZERS OFF"
        "PREFER_EXTERNAL_ZLIB ON"
        "CMAKE_POSITION_INDEPENDENT_CODE ON"
)

# Graphics and UI dependencies
set(VULKAN_VERSION 1.3.300)

CPMAddPackage(
    NAME VulkanHeaders
    GITHUB_REPOSITORY KhronosGroup/Vulkan-Headers
    GIT_TAG v${VULKAN_VERSION}
    VERSION ${VULKAN_VERSION}
)

if(VulkanHeaders_ADDED)
    set(VULKAN_LOADER_OPTIONS
        "BUILD_LOADER ON"
        "BUILD_TESTS OFF"
        "BUILD_SHARED_LOADER ON"
        "BUILD_STATIC_LOADER OFF"
        "VULKAN_HEADERS_INSTALL_DIR=${VulkanHeaders_SOURCE_DIR}"
        "BUILD_WSI_DIRECTFB_SUPPORT OFF"
    )

    if(NANOVDB_EDITOR_USE_GLFW)
        # Auto-detect available window system libraries
        set(WSI_XLIB_SUPPORT OFF)
        set(WSI_XCB_SUPPORT OFF)
        set(WSI_WAYLAND_SUPPORT OFF)

        # Check for X11/Xlib and required extensions
        find_package(X11 QUIET)
        set(X11_EXTENSIONS_FOUND TRUE)

        # Check for required X11 extension headers
        if(X11_FOUND)
            # Check for XRandR
            find_path(X11_XRANDR_INCLUDE_PATH X11/extensions/Xrandr.h
                HINTS ${X11_INCLUDE_DIR}
                PATH_SUFFIXES X11/extensions
            )
            if(NOT X11_XRANDR_INCLUDE_PATH)
                set(X11_EXTENSIONS_FOUND FALSE)
                message(STATUS "Vulkan: XRandR headers not found (install libxrandr-dev)")
            endif()
        endif()

        if(X11_FOUND AND X11_EXTENSIONS_FOUND)
            set(WSI_XLIB_SUPPORT ON)
            message(STATUS "Vulkan: X11/Xlib support enabled")
        else()
            set(WSI_XLIB_SUPPORT OFF)
            if(NOT X11_FOUND)
                message(STATUS "Vulkan: X11/Xlib not found, disabling support")
            else()
                message(STATUS "Vulkan: X11/Xlib extensions incomplete, disabling support")
            endif()
        endif()

        # Check for XCB
        find_package(PkgConfig QUIET)
        set(WSI_XCB_SUPPORT OFF)
        if(PKG_CONFIG_FOUND)
            pkg_check_modules(XCB QUIET xcb)
            if(XCB_FOUND)
                # Check for XCB headers
                find_path(XCB_INCLUDE_PATH xcb/xcb.h
                    HINTS ${XCB_INCLUDE_DIRS}
                    PATHS /usr/include /usr/local/include
                )
                if(XCB_INCLUDE_PATH)
                    set(WSI_XCB_SUPPORT ON)
                    message(STATUS "Vulkan: XCB support enabled")
                    if(XCB_INCLUDE_DIRS)
                        list(APPEND VULKAN_LOADER_OPTIONS "CMAKE_C_FLAGS=-I${XCB_INCLUDE_DIRS}")
                        list(APPEND VULKAN_LOADER_OPTIONS "CMAKE_CXX_FLAGS=-I${XCB_INCLUDE_DIRS}")
                    elseif(XCB_INCLUDE_PATH)
                        list(APPEND VULKAN_LOADER_OPTIONS "CMAKE_C_FLAGS=-I${XCB_INCLUDE_PATH}")
                        list(APPEND VULKAN_LOADER_OPTIONS "CMAKE_CXX_FLAGS=-I${XCB_INCLUDE_PATH}")
                    endif()
                else()
                    message(STATUS "Vulkan: XCB headers not found (install libxcb1-dev), disabling support")
                endif()
            else()
                message(STATUS "Vulkan: XCB not found via pkg-config, disabling support")
            endif()
        else()
            message(STATUS "Vulkan: pkg-config not found, disabling XCB support")
        endif()

        # Check for Wayland
        if(PKG_CONFIG_FOUND)
            pkg_check_modules(WAYLAND_CLIENT QUIET wayland-client)
            if(WAYLAND_CLIENT_FOUND)
                set(WSI_WAYLAND_SUPPORT ON)
                message(STATUS "Vulkan: Wayland support enabled")
            else()
                message(STATUS "Vulkan: Wayland not found, disabling support")
            endif()
        endif()

        list(APPEND VULKAN_LOADER_OPTIONS
            "BUILD_WSI_XLIB_SUPPORT ${WSI_XLIB_SUPPORT}"
            "BUILD_WSI_XCB_SUPPORT ${WSI_XCB_SUPPORT}"
            "BUILD_WSI_WAYLAND_SUPPORT ${WSI_WAYLAND_SUPPORT}"
        )
    else()
        list(APPEND VULKAN_LOADER_OPTIONS
            "BUILD_WSI_XLIB_SUPPORT OFF"
            "BUILD_WSI_XCB_SUPPORT OFF"
            "BUILD_WSI_WAYLAND_SUPPORT OFF"
        )
    endif()

    CPMAddPackage(
        NAME VulkanLoader
        GITHUB_REPOSITORY KhronosGroup/Vulkan-Loader
        GIT_TAG v${VULKAN_VERSION}
        VERSION ${VULKAN_VERSION}
        OPTIONS ${VULKAN_LOADER_OPTIONS}
    )
endif()

if(NANOVDB_EDITOR_USE_GLFW)
    set(GLFW_RELEASE 3.4)
    set(GLFW_BASE_URL "https://github.com/glfw/glfw/releases/download/${GLFW_RELEASE}")

    # For Windows and Apple, download pre-built binaries
    if(WIN32)
        set(GLFW_URL ${GLFW_BASE_URL}/glfw-${GLFW_RELEASE}.bin.WIN64.zip)
        set(GLFW_PLATFORM_OPTIONS URL ${GLFW_URL})
    elseif(APPLE)
        set(GLFW_URL ${GLFW_BASE_URL}/glfw-${GLFW_RELEASE}.bin.MACOS.zip)
        set(GLFW_PLATFORM_OPTIONS URL ${GLFW_URL})
    else()
        # For Linux, only download headers
        set(GLFW_PLATFORM_OPTIONS
            GITHUB_REPOSITORY glfw/glfw
            GIT_TAG ${GLFW_RELEASE}
        )
    endif()

    CPMAddPackage(
        NAME glfw
        VERSION ${GLFW_RELEASE}
        DOWNLOAD_ONLY YES
        ${GLFW_PLATFORM_OPTIONS}
    )
endif()

# Create glfw target immediately to prevent CPM recursion
if(glfw_ADDED)
    if(WIN32)
        add_library(glfw SHARED IMPORTED)
        set_target_properties(glfw PROPERTIES
            IMPORTED_LOCATION ${glfw_SOURCE_DIR}/lib-vc2022/glfw3.dll
            IMPORTED_IMPLIB ${glfw_SOURCE_DIR}/lib-vc2022/glfw3.lib
            INTERFACE_INCLUDE_DIRECTORIES ${glfw_SOURCE_DIR}/include
        )
    elseif(APPLE)
        add_library(glfw SHARED IMPORTED)
        set_target_properties(glfw PROPERTIES
            IMPORTED_LOCATION ${glfw_SOURCE_DIR}/lib-universal/libglfw.3.dylib
            INTERFACE_INCLUDE_DIRECTORIES ${glfw_SOURCE_DIR}/include
        )
    else()
        # For Linux, create interface target
        add_library(glfw INTERFACE)
        target_include_directories(glfw INTERFACE ${glfw_SOURCE_DIR}/include)
    endif()
endif()

CPMAddPackage(
    NAME imgui
    GITHUB_REPOSITORY ocornut/imgui
    GIT_TAG v1.92.0-docking
    VERSION 1.92.0
    DOWNLOAD_ONLY YES
    PATCH_COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_LIST_DIR}/patch_imgui.cmake
)

CPMAddPackage(
    NAME ImGuiFileDialog
    GITHUB_REPOSITORY aiekick/ImGuiFileDialog
    GIT_TAG v0.6.7
    VERSION 0.6.7
    DOWNLOAD_ONLY YES
)

CPMAddPackage(
    NAME ImGuiColorTextEdit
    GITHUB_REPOSITORY goossens/ImGuiColorTextEdit
    GIT_TAG 453536464d686660f583db3809af85ffe2d54a68
    VERSION 1.0.0
    DOWNLOAD_ONLY YES
)

# Shader compilation
set(SLANG_VERSION 2025.24)

if(NANOVDB_EDITOR_BUILD_SLANG_FROM_SOURCE)
    if(NOT (UNIX AND NOT APPLE) OR CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64")
        message(FATAL_ERROR "NANOVDB_EDITOR_BUILD_SLANG_FROM_SOURCE is only supported on Linux x86_64")
    endif()
    if(NOT SKBUILD)
        message(FATAL_ERROR "NANOVDB_EDITOR_BUILD_SLANG_FROM_SOURCE is currently intended for wheel builds (SKBUILD=ON)")
    endif()

    include(ExternalProject)
    set(SLANG_INSTALL_DIR "${CMAKE_BINARY_DIR}/slang-install")
    set(SLANG_INSTALLED_LIB "${SLANG_INSTALL_DIR}/lib/libslang${CMAKE_SHARED_LIBRARY_SUFFIX}")
    set(SLANG_EP_INSTALL_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/slang_ep_install.cmake")

    ExternalProject_Add(slang_src_build
        GIT_REPOSITORY https://github.com/shader-slang/slang.git
        GIT_TAG v${SLANG_VERSION}
        GIT_SHALLOW TRUE
        UPDATE_DISCONNECTED TRUE
        CMAKE_ARGS
            -DCMAKE_BUILD_TYPE=Release
            -DCMAKE_INSTALL_PREFIX=${SLANG_INSTALL_DIR}
            -DCMAKE_INSTALL_LIBDIR=lib
            -DCMAKE_POSITION_INDEPENDENT_CODE=ON
            -DBUILD_SHARED_LIBS=ON
            -DSLANG_ENABLE_TESTS=OFF
            -DSLANG_ENABLE_EXAMPLES=OFF
            -DSLANG_ENABLE_SLANG_RHI=OFF
            -DSLANG_ENABLE_GFX=OFF
        BUILD_BYPRODUCTS
            ${SLANG_INSTALLED_LIB}
        BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --config Release
        INSTALL_COMMAND ${CMAKE_COMMAND} -DSLANG_EP_BINARY_DIR=<BINARY_DIR> -P ${SLANG_EP_INSTALL_SCRIPT}
    )

    # Match existing variable naming used throughout the project.
    set(Slang_SOURCE_DIR "${SLANG_INSTALL_DIR}")
    set(Slang_ADDED TRUE)

    # The imported target 'slang' uses ${Slang_SOURCE_DIR}/include as an
    # INTERFACE_INCLUDE_DIRECTORIES entry. CMake requires that include
    # directory to exist at generate time, but ExternalProject installs it
    # later during the build.
    file(MAKE_DIRECTORY "${Slang_SOURCE_DIR}/include")
    file(MAKE_DIRECTORY "${Slang_SOURCE_DIR}/lib")
    file(MAKE_DIRECTORY "${Slang_SOURCE_DIR}/bin")
else()
    if(WIN32)
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64")
            set(SLANG_URL https://github.com/shader-slang/slang/releases/download/v${SLANG_VERSION}/slang-${SLANG_VERSION}-windows-aarch64.zip)
        else()
            set(SLANG_URL https://github.com/shader-slang/slang/releases/download/v${SLANG_VERSION}/slang-${SLANG_VERSION}-windows-x86_64.zip)
        endif()
    elseif(APPLE)
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64")
            set(SLANG_URL https://github.com/shader-slang/slang/releases/download/v${SLANG_VERSION}/slang-${SLANG_VERSION}-macos-arm64.zip)
        else()
            set(SLANG_URL https://github.com/shader-slang/slang/releases/download/v${SLANG_VERSION}/slang-${SLANG_VERSION}-macos-x86_64.zip)
        endif()
    elseif(UNIX AND NOT APPLE)
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64")
            set(SLANG_URL https://github.com/shader-slang/slang/releases/download/v${SLANG_VERSION}/slang-${SLANG_VERSION}-linux-aarch64.zip)
        else()
            # Detect GLIBC version to choose appropriate Slang build
            execute_process(
                COMMAND ldd --version
                OUTPUT_VARIABLE GLIBC_VERSION_OUTPUT
                ERROR_QUIET
                OUTPUT_STRIP_TRAILING_WHITESPACE
            )

            # Extract GLIBC version number (e.g., "2.28" from "ldd (GNU libc) 2.28")
            string(REGEX MATCH "([0-9]+\\.[0-9]+)" GLIBC_VERSION "${GLIBC_VERSION_OUTPUT}")

            # Use compatibility build for older systems
            if(GLIBC_VERSION AND GLIBC_VERSION VERSION_LESS "2.29")
                message(STATUS "Detected GLIBC ${GLIBC_VERSION}, using compatibility Slang build")
                set(SLANG_URL https://github.com/shader-slang/slang/releases/download/v${SLANG_VERSION}/slang-${SLANG_VERSION}-linux-x86_64-glibc-2.27.zip)
            else()
                message(STATUS "Using standard Slang build (GLIBC ${GLIBC_VERSION})")
                set(SLANG_URL https://github.com/shader-slang/slang/releases/download/v${SLANG_VERSION}/slang-${SLANG_VERSION}-linux-x86_64.zip)
            endif()
        endif()
    else()
        message(WARNING "Unsupported platform for Slang. Please manually specify Slang package.")
    endif()

    CPMAddPackage(
        NAME Slang
        URL ${SLANG_URL}
        VERSION ${SLANG_VERSION}
    )
endif()

# File handling and utilities
CPMAddPackage(
    NAME FileWatch
    GITHUB_REPOSITORY ThomasMonkman/filewatch
    GIT_TAG a59891baf375b73ff28144973a6fafd3fe40aa21    # master
    VERSION 1.0.0
    PATCH_COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_LIST_DIR}/patch_filewatch.cmake
    OPTIONS
        "BuildTests OFF"
)

CPMAddPackage(
    NAME json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    VERSION 3.11.3
    OPTIONS
        "JSON_BuildTests OFF"
)

CPMAddPackage(
    NAME cnpy
    GITHUB_REPOSITORY rogersce/cnpy
    GIT_TAG 4e8810b1a8637695171ed346ce68f6984e585ef4    # master
    VERSION 1.0.0
    PATCH_COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_LIST_DIR}/patch_cnpy.cmake
    OPTIONS
        "CMAKE_POSITION_INDEPENDENT_CODE ON"
        "CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS OFF"
        "ENABLE_STATIC ON"
)

# Fix cnpy dependency on zlib
if(cnpy_ADDED AND zlib_ADDED)
    if(TARGET cnpy)
        target_include_directories(cnpy PRIVATE ${zlib_SOURCE_DIR} ${zlib_BINARY_DIR})
        target_link_libraries(cnpy zlibstatic)
    endif()
    if(TARGET cnpy-static)
        target_include_directories(cnpy-static PRIVATE ${zlib_SOURCE_DIR} ${zlib_BINARY_DIR})
        target_link_libraries(cnpy-static zlibstatic)
    endif()
endif()

CPMAddPackage(
    NAME zstr
    GITHUB_REPOSITORY mateidavid/zstr
    GIT_TAG v1.0.7
    VERSION 1.0.7
    OPTIONS
        "CMAKE_POSITION_INDEPENDENT_CODE ON"
)

# Networking and HTTP
CPMAddPackage(
    NAME llhttp
    GITHUB_REPOSITORY nodejs/llhttp
    GIT_TAG release/v9.3.0
    VERSION 9.3.0
    OPTIONS
        "CMAKE_POSITION_INDEPENDENT_CODE ON"
)

CPMAddPackage(
    NAME asio
    GITHUB_REPOSITORY chriskohlhoff/asio
    GIT_TAG asio-1-29-0
    VERSION 1.29.0
    DOWNLOAD_ONLY YES
)

CPMAddPackage(
    NAME restinio
    GITHUB_REPOSITORY Stiffstream/restinio
    GIT_TAG v0.7.7
    VERSION 0.7.7
    DOWNLOAD_ONLY YES
)

# Utilities
set(FMT_VERSION 11.2.0)
CPMAddPackage(
    NAME fmt
    GITHUB_REPOSITORY fmtlib/fmt
    GIT_TAG ${FMT_VERSION}
    VERSION ${FMT_VERSION}
    OPTIONS
        "FMT_HEADER_ONLY OFF"
        "FMT_TEST OFF"
        "FMT_DOC OFF"
        "CMAKE_POSITION_INDEPENDENT_CODE ON"
)

if(fmt_ADDED)
    include_directories(SYSTEM ${fmt_SOURCE_DIR}/include)

    # Set standard CMake variables that other packages might look for
    set(fmt_FOUND TRUE CACHE BOOL "fmt found" FORCE)
    set(fmt_INCLUDE_DIRS "${fmt_SOURCE_DIR}/include" CACHE STRING "fmt include directories" FORCE)
    set(fmt_VERSION ${FMT_VERSION} CACHE STRING "fmt version" FORCE)

    # Ensure the fmt target has proper PIC
    if(TARGET fmt)
        set_target_properties(fmt PROPERTIES POSITION_INDEPENDENT_CODE ON)
    endif()
endif()

CPMAddPackage(
    NAME argparse
    GITHUB_REPOSITORY morrisfranken/argparse
    GIT_TAG 58fcb68a409c182163b4784070a8eb083d76a82b    # master
    VERSION 1.0.0
    DOWNLOAD_ONLY YES
)

set(EXPECTED_VERSION 0.9.0)
CPMAddPackage(
    NAME expected
    URL
        https://github.com/martinmoene/expected-lite/archive/refs/tags/v${EXPECTED_VERSION}.tar.gz
        https://codeload.github.com/martinmoene/expected-lite/tar.gz/refs/tags/v${EXPECTED_VERSION}
    VERSION ${EXPECTED_VERSION}
)

# Optional dependencies
if(NANOVDB_EDITOR_E57_FORMAT)
    CPMAddPackage(
        NAME libE57Format
        GITHUB_REPOSITORY asmaloney/libE57Format
        GIT_TAG v3.2.0
        VERSION 3.2.0
        OPTIONS
            "E57FORMAT_BUILD_EXAMPLES OFF"
            "E57FORMAT_BUILD_TESTS OFF"
    )
endif()

if(NANOVDB_EDITOR_USE_H264)
    CPMAddPackage(
        NAME openh264
        GITHUB_REPOSITORY cisco/openh264
        GIT_TAG 2.5.1
        VERSION 2.5.1
        DOWNLOAD_ONLY YES
    )
endif()

# Google Test
if(NANOVDB_EDITOR_BUILD_TESTS)
    CPMAddPackage(
        NAME googletest
        GITHUB_REPOSITORY google/googletest
        GIT_TAG v1.15.2
        VERSION 1.15.2
            OPTIONS "INSTALL_GTEST OFF"
    )
endif()


if(nanovdb_ADDED)
    add_library(nanovdb INTERFACE)
    target_include_directories(nanovdb INTERFACE ${nanovdb_SOURCE_DIR}/nanovdb)
endif()

if(zlib_ADDED)
    if(TARGET zlibstatic)
        set_target_properties(zlibstatic PROPERTIES POSITION_INDEPENDENT_CODE ON)
    endif()
endif()

if(imgui_ADDED)
    add_library(imgui INTERFACE)
    target_include_directories(imgui INTERFACE ${imgui_SOURCE_DIR})
    file(GLOB IMGUI_SOURCE_FILES "${imgui_SOURCE_DIR}/imgui*.cpp")
    target_sources(imgui INTERFACE ${IMGUI_SOURCE_FILES})
    target_compile_features(imgui INTERFACE cxx_std_17)
endif()

if(ImGuiFileDialog_ADDED)
    add_library(ImGuiFileDialog INTERFACE)
    target_include_directories(ImGuiFileDialog INTERFACE ${ImGuiFileDialog_SOURCE_DIR})
    file(GLOB IMGUIFILEDIALOG_SOURCE_FILES "${ImGuiFileDialog_SOURCE_DIR}/*.cpp")
    target_sources(ImGuiFileDialog INTERFACE ${IMGUIFILEDIALOG_SOURCE_FILES})
    target_compile_features(ImGuiFileDialog INTERFACE cxx_std_17)
    target_link_libraries(ImGuiFileDialog INTERFACE imgui)
endif()

if(ImGuiColorTextEdit_ADDED)
    add_library(ImGuiColorTextEdit INTERFACE)
    target_include_directories(ImGuiColorTextEdit INTERFACE ${ImGuiColorTextEdit_SOURCE_DIR})
    file(GLOB IMGUICOLORTEXTEDIT_SOURCE_FILES "${ImGuiColorTextEdit_SOURCE_DIR}/*.cpp")
    target_sources(ImGuiColorTextEdit INTERFACE ${IMGUICOLORTEXTEDIT_SOURCE_FILES})
    target_compile_features(ImGuiColorTextEdit INTERFACE cxx_std_17)
    target_link_libraries(ImGuiColorTextEdit INTERFACE imgui)
endif()

if(asio_ADDED)
    add_library(asio INTERFACE)
    target_include_directories(asio INTERFACE ${asio_SOURCE_DIR}/asio/include)
    target_compile_definitions(asio INTERFACE ASIO_STANDALONE)
    target_compile_features(asio INTERFACE cxx_std_17)
endif()

if(argparse_ADDED)
    add_library(argparse INTERFACE)
    target_include_directories(argparse INTERFACE ${argparse_SOURCE_DIR}/include)
    target_compile_features(argparse INTERFACE cxx_std_17)
endif()

if(restinio_ADDED)
    add_library(restinio INTERFACE)
    target_include_directories(restinio INTERFACE ${restinio_SOURCE_DIR}/dev)
    target_compile_features(restinio INTERFACE cxx_std_17)
    # restinio headers depend on fmt, so link to fmt target
    if(TARGET fmt)
        target_link_libraries(restinio INTERFACE fmt)
    endif()
endif()

if(Slang_ADDED)
    add_library(slang SHARED IMPORTED)

    # Set library location using platform-specific naming
    if(WIN32)
        set_target_properties(slang PROPERTIES
            IMPORTED_LOCATION ${Slang_SOURCE_DIR}/bin/slang${CMAKE_SHARED_LIBRARY_SUFFIX}
            IMPORTED_IMPLIB ${Slang_SOURCE_DIR}/lib/slang${CMAKE_STATIC_LIBRARY_SUFFIX}
            INTERFACE_INCLUDE_DIRECTORIES ${Slang_SOURCE_DIR}/include
        )
    else()
        set_target_properties(slang PROPERTIES
            IMPORTED_LOCATION ${Slang_SOURCE_DIR}/lib/libslang${CMAKE_SHARED_LIBRARY_SUFFIX}
            INTERFACE_INCLUDE_DIRECTORIES ${Slang_SOURCE_DIR}/include
        )
    endif()

    # SKBUILD installs the library directly via install() rules.
    if(NOT SKBUILD)
        add_custom_target(copy_slang_libs
            COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_LIBRARY_OUTPUT_DIRECTORY}
            COMMENT "Copying Slang libraries to main lib directory"
        )

        if(WIN32)
            add_custom_command(TARGET copy_slang_libs POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${Slang_SOURCE_DIR}/bin/slang${CMAKE_SHARED_LIBRARY_SUFFIX}
                    ${CMAKE_BINARY_DIR}/$<CONFIG>/slang${CMAKE_SHARED_LIBRARY_SUFFIX}
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${Slang_SOURCE_DIR}/bin/slang-compiler${CMAKE_SHARED_LIBRARY_SUFFIX}
                    ${CMAKE_BINARY_DIR}/$<CONFIG>/slang-compiler${CMAKE_SHARED_LIBRARY_SUFFIX}
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${Slang_SOURCE_DIR}/bin/slang-glslang${CMAKE_SHARED_LIBRARY_SUFFIX}
                    ${CMAKE_BINARY_DIR}/$<CONFIG>/slang-glslang${CMAKE_SHARED_LIBRARY_SUFFIX}
            )
            # Copy slang-llvm only if it exists
            if(EXISTS ${Slang_SOURCE_DIR}/bin/slang-llvm${CMAKE_SHARED_LIBRARY_SUFFIX})
                add_custom_command(TARGET copy_slang_libs POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        ${Slang_SOURCE_DIR}/bin/slang-llvm${CMAKE_SHARED_LIBRARY_SUFFIX}
                        ${CMAKE_BINARY_DIR}/$<CONFIG>/slang-llvm${CMAKE_SHARED_LIBRARY_SUFFIX}
                )
            else()
                message(STATUS "slang-llvm library not found, skipping copy")
            endif()
        else()
            # Note: libslang-compiler and libslang-glslang have version suffix in filename (e.g., libslang-compiler-2025.23.1.so)
            add_custom_command(TARGET copy_slang_libs POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${Slang_SOURCE_DIR}/lib/libslang${CMAKE_SHARED_LIBRARY_SUFFIX}
                    ${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/libslang${CMAKE_SHARED_LIBRARY_SUFFIX}
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${Slang_SOURCE_DIR}/lib/libslang-compiler${CMAKE_SHARED_LIBRARY_SUFFIX}.0.${SLANG_VERSION}
                    ${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/libslang-compiler${CMAKE_SHARED_LIBRARY_SUFFIX}.0.${SLANG_VERSION}
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${Slang_SOURCE_DIR}/lib/libslang-glslang-${SLANG_VERSION}${CMAKE_SHARED_LIBRARY_SUFFIX}
                    ${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/libslang-glslang-${SLANG_VERSION}${CMAKE_SHARED_LIBRARY_SUFFIX}
            )
            # Copy slang-llvm only if it exists
            if(EXISTS ${Slang_SOURCE_DIR}/lib/libslang-llvm${CMAKE_SHARED_LIBRARY_SUFFIX})
                add_custom_command(TARGET copy_slang_libs POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        ${Slang_SOURCE_DIR}/lib/libslang-llvm${CMAKE_SHARED_LIBRARY_SUFFIX}
                        ${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/libslang-llvm${CMAKE_SHARED_LIBRARY_SUFFIX}
                )
            else()
                message(STATUS "libslang-llvm library not found, skipping copy")
            endif()
        endif()
    endif()
endif()

# Ensure static linking for all targets

if(blosc_ADDED)
    if(TARGET blosc_static)
        set_target_properties(blosc_static PROPERTIES POSITION_INDEPENDENT_CODE ON)
        # Ensure blosc can find zlib headers and libraries
        if(TARGET zlibstatic)
            target_include_directories(blosc_static PRIVATE ${zlib_SOURCE_DIR} ${zlib_BINARY_DIR})
            # Use plain signature to match existing usage in blosc CMakeLists.txt
            target_link_libraries(blosc_static zlibstatic)
        endif()
    endif()
endif()

if(VulkanHeaders_ADDED)
    add_library(VulkanHeaders INTERFACE)
    target_include_directories(VulkanHeaders INTERFACE ${VulkanHeaders_SOURCE_DIR}/include)
endif()

if(VulkanLoader_ADDED)
    if(TARGET vulkan AND NOT TARGET Vulkan::Vulkan)
        add_library(Vulkan::Vulkan ALIAS vulkan)
    endif()

    # SKBUILD installs the library directly via install() rules.
    if(NOT SKBUILD)
        if(TARGET vulkan)
            # Copy the produced Vulkan loader to the main lib directory for runtime loading
            add_custom_target(copy_vulkan_loader
                COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_LIBRARY_OUTPUT_DIRECTORY}
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_FILE:vulkan>
                    ${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/$<TARGET_FILE_NAME:vulkan>
                COMMENT "Copying Vulkan loader to main lib directory"
            )
        endif()
    endif()
endif()

if(cnpy_ADDED)
    if(TARGET cnpy-static)
        set_target_properties(cnpy-static PROPERTIES POSITION_INDEPENDENT_CODE ON)
    endif()
endif()

if(FileWatch_ADDED AND TARGET filewatch)
    set_target_properties(filewatch PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

if(json_ADDED AND TARGET nlohmann_json)
    set_target_properties(nlohmann_json PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

if(expected_ADDED AND TARGET expected_lite)
    set_target_properties(expected_lite PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

if(NANOVDB_EDITOR_E57_FORMAT AND libE57Format_ADDED AND TARGET E57Format)
    set_target_properties(E57Format PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

if(openh264_ADDED)
    file(MAKE_DIRECTORY ${CPM_PACKAGE_openh264_BINARY_DIR})     # DOWNLOAD_ONLY does not create build dir

    set(OPENH264_OUTPUT_LIB ${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/libopenh264${CMAKE_STATIC_LIBRARY_SUFFIX})
    set(OPENH264_BUILD_LIB ${CPM_PACKAGE_openh264_BINARY_DIR}/libopenh264${CMAKE_STATIC_LIBRARY_SUFFIX})

    add_custom_command(
        OUTPUT ${OPENH264_OUTPUT_LIB}
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CPM_PACKAGE_openh264_SOURCE_DIR}
            ${CPM_PACKAGE_openh264_BINARY_DIR}
        # Build encoder and common libraries from root directory
        COMMAND ${CMAKE_COMMAND} -E env CC=${CMAKE_C_COMPILER} CXX=${CMAKE_CXX_COMPILER}
                make -j${CMAKE_BUILD_PARALLEL_LEVEL}
                USE_ASM=No BUILDTYPE=static DECODER=No
                "CFLAGS=-fPIC -DWELS_X86_ASM=0"
                "CXXFLAGS=-fPIC -std=c++11 -DWELS_X86_ASM=0"
                libencoder.a libcommon.a libprocessing.a
        # Create our own static library from encoder objects
        COMMAND ${CMAKE_COMMAND} -E rm -rf temp_openh264_objects
        COMMAND ${CMAKE_COMMAND} -E make_directory temp_openh264_objects
        COMMAND ${CMAKE_COMMAND} -E chdir temp_openh264_objects ar x ../libencoder.a
        COMMAND ${CMAKE_COMMAND} -E chdir temp_openh264_objects ar x ../libcommon.a
        COMMAND ${CMAKE_COMMAND} -E chdir temp_openh264_objects ar x ../libprocessing.a
        COMMAND find temp_openh264_objects -name "*.o" -print0 | xargs -0 ar rcs ${OPENH264_BUILD_LIB}
        COMMAND ${CMAKE_COMMAND} -E rm -rf temp_openh264_objects
        # Ensure proper symbol index
        COMMAND ranlib ${OPENH264_BUILD_LIB}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_LIBRARY_OUTPUT_DIRECTORY}
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${OPENH264_BUILD_LIB}
            ${OPENH264_OUTPUT_LIB}
        # Ensure the final copied library also has proper symbol index
        COMMAND ranlib ${OPENH264_OUTPUT_LIB}
        WORKING_DIRECTORY ${CPM_PACKAGE_openh264_BINARY_DIR}
        DEPENDS ${CPM_PACKAGE_openh264_SOURCE_DIR}/Makefile
        COMMENT "Building openh264 encoder-only static library"
        VERBATIM
    )

    add_custom_target(openh264_build ALL
        DEPENDS ${OPENH264_OUTPUT_LIB}
    )

    # Create an imported library target pointing to the built library in output dir
    add_library(openh264 STATIC IMPORTED)
    set_target_properties(openh264 PROPERTIES
        IMPORTED_LOCATION ${OPENH264_OUTPUT_LIB}
        INTERFACE_INCLUDE_DIRECTORIES ${CPM_PACKAGE_openh264_SOURCE_DIR}/codec/api
    )
    add_dependencies(openh264 openh264_build)
endif()
