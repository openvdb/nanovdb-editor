# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: Apache-2.0

enable_testing()

include(GoogleTest)

function(ConfigureTest CMAKE_TEST_NAME)
  add_executable(${CMAKE_TEST_NAME} ${ARGN})

  set_target_properties(
    ${CMAKE_TEST_NAME}
    PROPERTIES
      CXX_STANDARD 17
      CXX_STANDARD_REQUIRED ON
  )

  target_include_directories(${CMAKE_TEST_NAME} PRIVATE
    ..
    ${nanovdb_SOURCE_DIR}/nanovdb
    ${Slang_SOURCE_DIR}/include
    ${imgui_SOURCE_DIR}
  )

  # Link against the editor libraries and gtest
  target_link_libraries(${CMAKE_TEST_NAME} PRIVATE
    pnanovdbcompiler
    pnanovdbcompute
    pnanovdbfileformat
    pnanovdbeditor
    nlohmann_json
    GTest::gtest
    GTest::gtest_main
  )

  if (NANOVDB_EDITOR_ENABLE_SANITIZERS AND CMAKE_BUILD_TYPE STREQUAL "Debug" AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${CMAKE_TEST_NAME} PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(${CMAKE_TEST_NAME} PRIVATE -fsanitize=address)
  endif()

  if(UNIX AND NOT APPLE)
    target_link_libraries(${CMAKE_TEST_NAME} PRIVATE dl)
  endif()

  add_test(NAME ${CMAKE_TEST_NAME}
           COMMAND $<TARGET_FILE:${CMAKE_TEST_NAME}>
           WORKING_DIRECTORY $<TARGET_FILE_DIR:${CMAKE_TEST_NAME}>)
endfunction()

ConfigureTest(ShaderCompileTest ShaderCompileTest.cpp)
ConfigureTest(ComputeDispatchTest ComputeDispatchTest.cpp)
ConfigureTest(ShaderCompileCpuTest ShaderCompileCpuTest.cpp)
ConfigureTest(FileFormatTest FileFormatTest.cpp)
ConfigureTest(EditorStartStopTest EditorStartStopTest.cpp)
ConfigureTest(EditorHeadlessNonStreamingTest EditorHeadlessNonStreamingTest.cpp)
