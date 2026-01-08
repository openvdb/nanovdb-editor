# Runs as ExternalProject INSTALL_COMMAND for slang_src_build.
# Ensures libslang-llvm is present where Slang's own install step expects it,
# then invokes the upstream install target.

if(NOT DEFINED SLANG_EP_BINARY_DIR)
    message(FATAL_ERROR "SLANG_EP_BINARY_DIR is required")
endif()

set(_bin_dir "${SLANG_EP_BINARY_DIR}")
set(_dst_dir "${_bin_dir}/Release/lib")
file(MAKE_DIRECTORY "${_dst_dir}")

# Slang's build can fetch a prebuilt LLVM support library into a versioned
# bundle directory (slang-<ver>-<platform>/lib). Slang's install step expects
# it in the build output directory (Release/lib) when using multi-config.
set(_preferred_glob "${_bin_dir}/slang-*/lib/libslang-llvm${CMAKE_SHARED_LIBRARY_SUFFIX}")
file(GLOB _preferred_matches ${_preferred_glob})

set(_src "")
if(_preferred_matches)
    list(GET _preferred_matches 0 _src)
else()
    file(GLOB _candidates "${_bin_dir}/slang-*/lib/libslang-llvm${CMAKE_SHARED_LIBRARY_SUFFIX}*")
    if(_candidates)
        list(GET _candidates 0 _src)
    endif()
endif()

if(_src)
    message(STATUS "slang_ep_install: Copying ${_src} -> ${_dst_dir}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_src}" "${_dst_dir}/"
        RESULT_VARIABLE _copy_rv
    )
    if(NOT _copy_rv EQUAL 0)
        message(FATAL_ERROR "slang_ep_install: Failed to copy libslang-llvm (rv=${_copy_rv})")
    endif()
else()
    message(STATUS "slang_ep_install: No libslang-llvm found under ${_bin_dir}/slang-*/lib; proceeding")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${_bin_dir}" --config Release --target install
    RESULT_VARIABLE _install_rv
)

if(NOT _install_rv EQUAL 0)
    message(FATAL_ERROR "slang_ep_install: Slang install failed (rv=${_install_rv})")
endif()
