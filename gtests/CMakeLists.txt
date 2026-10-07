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

  if (NANOVDB_EDITOR_ENABLE_TSAN AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${CMAKE_TEST_NAME} PRIVATE -fsanitize=thread -fno-omit-frame-pointer)
    target_link_options(${CMAKE_TEST_NAME} PRIVATE -fsanitize=thread)
  endif()

  if(UNIX AND NOT APPLE)
    target_link_libraries(${CMAKE_TEST_NAME} PRIVATE dl)
  endif()

  add_test(NAME ${CMAKE_TEST_NAME}
           COMMAND $<TARGET_FILE:${CMAKE_TEST_NAME}>
           WORKING_DIRECTORY $<TARGET_FILE_DIR:${CMAKE_TEST_NAME}>)
endfunction()

ConfigureTest(ShaderCompileTest ShaderCompileTest.cpp)
ConfigureTest(PipelineShaderCompileTest PipelineShaderCompileTest.cpp)
ConfigureTest(ComputeDispatchTest ComputeDispatchTest.cpp)
# ConfigureTest(ComputeDispatchCpuInterfaceTest ComputeDispatchCpuInterfaceTest.cpp)
ConfigureTest(ShaderCompileCpuTest ShaderCompileCpuTest.cpp)
ConfigureTest(FileFormatTest FileFormatTest.cpp)
ConfigureTest(EditorStartStopTest EditorStartStopTest.cpp)
ConfigureTest(RenderThreadTaskQueueTest RenderThreadTaskQueueTest.cpp)
ConfigureTest(EditorRapidAddRemoveTest EditorRapidAddRemoveTest.cpp GpuTestSupport.cpp)
ConfigureTest(EditorHeadlessNonStreamingTest EditorHeadlessNonStreamingTest.cpp)
ConfigureTest(EditorViewportCameraSyncTest
  EditorViewportCameraSyncTest.cpp
  ../editor/SceneView.cpp
)
ConfigureTest(EditorSlangCompileSpeedTest EditorSlangCompileSpeedTest.cpp)
ConfigureTest(CustomSceneParamsTest CustomSceneParamsTest.cpp ../editor/CustomSceneParams.cpp)
ConfigureTest(MapPinTest MapPinTest.cpp EditorTestSupport.cpp)
ConfigureTest(ShaderParamsReadOnlyTest ShaderParamsReadOnlyTest.cpp EditorTestSupport.cpp)
ConfigureTest(ShaderNameSwapResetsParamsTest ShaderNameSwapResetsParamsTest.cpp EditorTestSupport.cpp)
ConfigureTest(ShaderParamsResetToDefaultsTest ShaderParamsResetToDefaultsTest.cpp EditorTestSupport.cpp)
ConfigureTest(ShaderParamsConcurrencyTest ShaderParamsConcurrencyTest.cpp)
ConfigureTest(VoxelBVHBuildPipelineTest VoxelBVHBuildPipelineTest.cpp GpuTestSupport.cpp)
ConfigureTest(StreamingUiToViewSyncTest StreamingUiToViewSyncTest.cpp EditorTestSupport.cpp GpuTestSupport.cpp)
ConfigureTest(MultiEditorPipelineRuntimeTest MultiEditorPipelineRuntimeTest.cpp GpuTestSupport.cpp)
ConfigureTest(PipelineWorkerShutdownTest PipelineWorkerShutdownTest.cpp)
ConfigureTest(ConcurrentPipelineTest ConcurrentPipelineTest.cpp)
ConfigureTest(PipelineApiTest PipelineApiTest.cpp EditorTestSupport.cpp GpuTestSupport.cpp)
ConfigureTest(SceneObjectSourceTest SceneObjectSourceTest.cpp)
ConfigureTest(SceneObjectProcessChainTest SceneObjectProcessChainTest.cpp)
ConfigureTest(SceneObjectReplacementTest SceneObjectReplacementTest.cpp)
ConfigureTest(SceneSerializerTest
  SceneSerializerTest.cpp
  ../editor/Console.cpp
  ../editor/CustomSceneParams.cpp
  ../editor/EditorSceneManager.cpp
  ../editor/ParamWidget.cpp
  ../editor/PipelineParams.cpp
  ../editor/PipelineRegistry.cpp
  ../editor/SceneSerializer.cpp
  ../editor/SceneObject.cpp
  ../editor/SceneView.cpp
  ../editor/ShaderParams.cpp
)

# Remove the editor library from the SceneSerializerTest target to avoid linking to it
get_target_property(_scene_serializer_test_libs SceneSerializerTest LINK_LIBRARIES)
list(REMOVE_ITEM _scene_serializer_test_libs pnanovdbeditor)
set_target_properties(SceneSerializerTest PROPERTIES LINK_LIBRARIES "${_scene_serializer_test_libs}")
target_compile_definitions(SceneSerializerTest PRIVATE pnanovdbeditor_EXPORTS)
target_link_libraries(SceneSerializerTest PRIVATE ImGuiColorTextEdit)

# CPU-only concurrency tests - run locally with: ctest -L threadsafety
set_tests_properties(RenderThreadTaskQueueTest ShaderParamsConcurrencyTest PROPERTIES LABELS "threadsafety")
