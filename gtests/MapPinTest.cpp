// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0
//

#include <gtest/gtest.h>

#include <nanovdb_editor/putil/Compiler.h>
#include <nanovdb_editor/putil/Compute.h>
#include <nanovdb_editor/putil/Editor.h>

#include "editor/EditorParamMapRegistry.h"
#include "EditorTestSupport.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

namespace
{

class MapPinTest : public ::testing::Test
{
protected:
    pnanovdb_compiler_t compiler{};
    pnanovdb_compute_t compute{};
    pnanovdb_editor_t editor{};

    pnanovdb_editor_token_t* scene_token = nullptr;
    pnanovdb_editor_token_t* name_token = nullptr;
    pnanovdb_compute_array_t* owned_array = nullptr;

    // Backing buffer for regular map_params() without a real shader pipeline.
    std::array<uint8_t, 64> dummy_params_storage{};

    void SetUp() override
    {
        pnanovdb_compiler_load(&compiler);
        ASSERT_NE(compiler.module, nullptr) << "Compiler module not available";

        pnanovdb_compute_load(&compute, &compiler);
        ASSERT_NE(compute.module, nullptr) << "Failed to load compute module";

        pnanovdb_editor_load(&editor, &compute, &compiler);
        ASSERT_NE(editor.module, nullptr) << "Editor module failed to load";
        ASSERT_NE(editor.impl, nullptr) << "Editor impl not initialized by init()";

        scene_token = editor.get_token("pin_test_scene");
        name_token = editor.get_token("pin_test_object");
        ASSERT_NE(scene_token, nullptr);
        ASSERT_NE(name_token, nullptr);

        // add_nanovdb_2 duplicates the array, so this source buffer is independent.
        std::array<uint8_t, 16> bytes{};
        owned_array = compute.create_array(sizeof(uint8_t), bytes.size(), bytes.data());
        ASSERT_NE(owned_array, nullptr);

        editor.add_nanovdb_2(&editor, scene_token, name_token, owned_array);

        // Point the object at our buffer so map_params(regular) returns non-null.
        const pnanovdb_reflect_data_type_t* stub_type = PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_editor_token_t);
        ASSERT_NE(stub_type, nullptr);
        ASSERT_EQ(pnanovdb_editor_test::set_object_shader_params(
                      &editor, scene_token, name_token, dummy_params_storage.data(), stub_type),
                  PNANOVDB_TRUE);

        ASSERT_EQ(pnanovdb_editor::shader_name_map_ref_count(&editor, scene_token, name_token), 0u);
        ASSERT_EQ(pnanovdb_editor::param_map_stack_depth(&editor), 0u);
    }

    void TearDown() override
    {
        if (editor.impl)
        {
            editor.remove(&editor, scene_token, name_token);
            pnanovdb_editor_free(&editor);
        }
        if (owned_array)
        {
            compute.destroy_array(owned_array);
        }
        pnanovdb_compute_free(&compute);
        pnanovdb_compiler_free(&compiler);
    }
};

} // namespace

// Regular map/unmap: stack +/-1, shader-name pin untouched.
TEST_F(MapPinTest, RegularMapDoesNotTouchShaderNamePin)
{
    const pnanovdb_reflect_data_type_t* stub_type = PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_editor_token_t);

    void* ptr = editor.map_params(&editor, scene_token, name_token, stub_type);
    ASSERT_EQ(ptr, static_cast<void*>(dummy_params_storage.data()));
    EXPECT_EQ(pnanovdb_editor::shader_name_map_ref_count(&editor, scene_token, name_token), 0u);
    EXPECT_EQ(pnanovdb_editor::param_map_stack_depth(&editor), 1u);

    editor.unmap_params(&editor, scene_token, name_token);
    EXPECT_EQ(pnanovdb_editor::shader_name_map_ref_count(&editor, scene_token, name_token), 0u);
    EXPECT_EQ(pnanovdb_editor::param_map_stack_depth(&editor), 0u);
}

// Shader-name map/unmap: ref_count 0 -> 1 -> 0.
TEST_F(MapPinTest, ShaderNameMapIncrementsAndReleasesPin)
{
    const pnanovdb_reflect_data_type_t* name_type = PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_editor_shader_name_t);

    void* ptr = editor.map_params(&editor, scene_token, name_token, name_type);
    ASSERT_NE(ptr, nullptr);
    EXPECT_EQ(pnanovdb_editor::shader_name_map_ref_count(&editor, scene_token, name_token), 1u);
    EXPECT_EQ(pnanovdb_editor::param_map_stack_depth(&editor), 1u);

    editor.unmap_params(&editor, scene_token, name_token);
    EXPECT_EQ(pnanovdb_editor::shader_name_map_ref_count(&editor, scene_token, name_token), 0u);
    EXPECT_EQ(pnanovdb_editor::param_map_stack_depth(&editor), 0u);
}

// Regression: pre-fix, the regular-params unmap released the shader-name pin.
TEST_F(MapPinTest, RegularUnmapDoesNotReleaseActiveShaderNamePin)
{
    const pnanovdb_reflect_data_type_t* stub_type = PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_editor_token_t);
    const pnanovdb_reflect_data_type_t* name_type = PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_editor_shader_name_t);

    void* shader_name_ptr = editor.map_params(&editor, scene_token, name_token, name_type);
    ASSERT_NE(shader_name_ptr, nullptr);
    ASSERT_EQ(pnanovdb_editor::shader_name_map_ref_count(&editor, scene_token, name_token), 1u);
    ASSERT_EQ(pnanovdb_editor::param_map_stack_depth(&editor), 1u);

    void* regular_ptr = editor.map_params(&editor, scene_token, name_token, stub_type);
    ASSERT_EQ(regular_ptr, static_cast<void*>(dummy_params_storage.data()));
    EXPECT_EQ(pnanovdb_editor::shader_name_map_ref_count(&editor, scene_token, name_token), 1u)
        << "Regular map must not touch the shader-name pin";
    EXPECT_EQ(pnanovdb_editor::param_map_stack_depth(&editor), 2u);

    // The critical assertion: unmapping regular must not touch the shader-name pin.
    editor.unmap_params(&editor, scene_token, name_token);
    EXPECT_EQ(pnanovdb_editor::shader_name_map_ref_count(&editor, scene_token, name_token), 1u)
        << "Regression: regular-params unmap erroneously released shader-name pin";
    EXPECT_EQ(pnanovdb_editor::param_map_stack_depth(&editor), 1u);

    editor.unmap_params(&editor, scene_token, name_token);
    EXPECT_EQ(pnanovdb_editor::shader_name_map_ref_count(&editor, scene_token, name_token), 0u);
    EXPECT_EQ(pnanovdb_editor::param_map_stack_depth(&editor), 0u);
}

// Reverse nesting: Regular then ShaderName. Each unmap releases its matched map.
TEST_F(MapPinTest, ShaderNameMapUnderRegularIsReleasedFirst)
{
    const pnanovdb_reflect_data_type_t* stub_type = PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_editor_token_t);
    const pnanovdb_reflect_data_type_t* name_type = PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_editor_shader_name_t);

    void* regular_ptr = editor.map_params(&editor, scene_token, name_token, stub_type);
    ASSERT_EQ(regular_ptr, static_cast<void*>(dummy_params_storage.data()));

    void* shader_name_ptr = editor.map_params(&editor, scene_token, name_token, name_type);
    ASSERT_NE(shader_name_ptr, nullptr);
    EXPECT_EQ(pnanovdb_editor::shader_name_map_ref_count(&editor, scene_token, name_token), 1u);
    EXPECT_EQ(pnanovdb_editor::param_map_stack_depth(&editor), 2u);

    // Inner (top) = shader-name map; releases the pin.
    editor.unmap_params(&editor, scene_token, name_token);
    EXPECT_EQ(pnanovdb_editor::shader_name_map_ref_count(&editor, scene_token, name_token), 0u);
    EXPECT_EQ(pnanovdb_editor::param_map_stack_depth(&editor), 1u);

    // Outer = regular map; must not touch any pin.
    editor.unmap_params(&editor, scene_token, name_token);
    EXPECT_EQ(pnanovdb_editor::shader_name_map_ref_count(&editor, scene_token, name_token), 0u);
    EXPECT_EQ(pnanovdb_editor::param_map_stack_depth(&editor), 0u);
}

// Unmatched unmap_params() is a silent no-op (pre-fix it decremented ref-counts).
TEST_F(MapPinTest, UnmatchedUnmapIsNoOp)
{
    ASSERT_EQ(pnanovdb_editor::param_map_stack_depth(&editor), 0u);
    editor.unmap_params(&editor, scene_token, name_token);
    EXPECT_EQ(pnanovdb_editor::shader_name_map_ref_count(&editor, scene_token, name_token), 0u);
    EXPECT_EQ(pnanovdb_editor::param_map_stack_depth(&editor), 0u);
}

// A failed (nullptr) map_params must not push a frame, so a paired unmap cannot pop the wrong one.
TEST_F(MapPinTest, FailedMapLeavesStackUntouched)
{
    const pnanovdb_reflect_data_type_t* stub_type = PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_editor_token_t);
    const pnanovdb_reflect_data_type_t* name_type = PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_editor_shader_name_t);

    // Prime the stack with a successful shader-name pin.
    ASSERT_NE(editor.map_params(&editor, scene_token, name_token, name_type), nullptr);
    ASSERT_EQ(pnanovdb_editor::param_map_stack_depth(&editor), 1u);
    ASSERT_EQ(pnanovdb_editor::shader_name_map_ref_count(&editor, scene_token, name_token), 1u);

    // Successful regular map to keep the test self-consistent.
    void* regular_ptr = editor.map_params(&editor, scene_token, name_token, stub_type);
    ASSERT_EQ(regular_ptr, static_cast<void*>(dummy_params_storage.data()));
    ASSERT_EQ(pnanovdb_editor::param_map_stack_depth(&editor), 2u);

    // Map a nonexistent object: returns nullptr and must not push a frame.
    pnanovdb_editor_token_t* nonexistent = editor.get_token("pin_test_nonexistent_object");
    void* missing_ptr = editor.map_params(&editor, scene_token, nonexistent, stub_type);
    EXPECT_EQ(missing_ptr, nullptr);
    EXPECT_EQ(pnanovdb_editor::param_map_stack_depth(&editor), 2u)
        << "Failed map_params must not push a frame onto the map-call stack";

    // Unwind: Regular, then ShaderName.
    editor.unmap_params(&editor, scene_token, name_token);
    EXPECT_EQ(pnanovdb_editor::shader_name_map_ref_count(&editor, scene_token, name_token), 1u);
    editor.unmap_params(&editor, scene_token, name_token);
    EXPECT_EQ(pnanovdb_editor::shader_name_map_ref_count(&editor, scene_token, name_token), 0u);
    EXPECT_EQ(pnanovdb_editor::param_map_stack_depth(&editor), 0u);
}

TEST_F(MapPinTest, ConcurrentShaderNameMapsAreThreadSafe)
{
    const pnanovdb_reflect_data_type_t* name_type = PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_editor_shader_name_t);

    constexpr int kThreadCount = 8;
    std::atomic<int> mapped_count{ 0 };
    std::atomic<bool> release_gate{ false };

    auto worker = [&]()
    {
        // Each worker independently maps the shader-name pin.
        void* ptr = editor.map_params(&editor, scene_token, name_token, name_type);
        EXPECT_NE(ptr, nullptr);
        // The map-call frame stack is per-thread, so each worker sees depth 1.
        EXPECT_EQ(pnanovdb_editor::param_map_stack_depth(&editor), 1u);

        mapped_count.fetch_add(1, std::memory_order_acq_rel);

        // Hold the pin until the main thread observes the peak ref-count.
        while (!release_gate.load(std::memory_order_acquire))
        {
            std::this_thread::yield();
        }

        editor.unmap_params(&editor, scene_token, name_token);
        EXPECT_EQ(pnanovdb_editor::param_map_stack_depth(&editor), 0u);
    };

    std::vector<std::thread> threads;
    threads.reserve(kThreadCount);
    for (int i = 0; i < kThreadCount; ++i)
    {
        threads.emplace_back(worker);
    }

    // Wait for every worker to acquire its pin, then observe peak ref-count.
    while (mapped_count.load(std::memory_order_acquire) < kThreadCount)
    {
        std::this_thread::yield();
    }
    EXPECT_EQ(
        pnanovdb_editor::shader_name_map_ref_count(&editor, scene_token, name_token), static_cast<size_t>(kThreadCount))
        << "ref-count must reflect contributions from every thread holding the pin";
    // The main thread never called map_params, so its own per-thread stack is empty.
    EXPECT_EQ(pnanovdb_editor::param_map_stack_depth(&editor), 0u);

    release_gate.store(true, std::memory_order_release);
    for (auto& t : threads)
    {
        t.join();
    }

    EXPECT_EQ(pnanovdb_editor::shader_name_map_ref_count(&editor, scene_token, name_token), 0u);
    EXPECT_EQ(pnanovdb_editor::param_map_stack_depth(&editor), 0u);
}

TEST_F(MapPinTest, CrossThreadUnmapDoesNotReleaseOtherThreadsPin)
{
    const pnanovdb_reflect_data_type_t* name_type = PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_editor_shader_name_t);

    std::atomic<bool> a_mapped{ false };
    std::atomic<bool> b_finished{ false };

    std::thread thread_a(
        [&]()
        {
            void* ptr = editor.map_params(&editor, scene_token, name_token, name_type);
            EXPECT_NE(ptr, nullptr);
            EXPECT_EQ(pnanovdb_editor::param_map_stack_depth(&editor), 1u);
            a_mapped.store(true, std::memory_order_release);

            // Wait for thread B's stray unmap_params() to complete.
            while (!b_finished.load(std::memory_order_acquire))
            {
                std::this_thread::yield();
            }

            // The cross-thread unmap on thread B must not have torn down A's frame.
            EXPECT_EQ(pnanovdb_editor::param_map_stack_depth(&editor), 1u)
                << "A's per-thread frame stack must survive a stray unmap_params() on another thread";
            EXPECT_EQ(pnanovdb_editor::shader_name_map_ref_count(&editor, scene_token, name_token), 1u)
                << "Cross-thread unmap_params() must not release a pin owned by another thread";

            editor.unmap_params(&editor, scene_token, name_token);
            EXPECT_EQ(pnanovdb_editor::param_map_stack_depth(&editor), 0u);
        });

    while (!a_mapped.load(std::memory_order_acquire))
    {
        std::this_thread::yield();
    }
    ASSERT_EQ(pnanovdb_editor::shader_name_map_ref_count(&editor, scene_token, name_token), 1u);

    std::thread thread_b(
        [&]()
        {
            // Thread B has an empty per-thread frame stack; unmap_params() is a silent no-op.
            EXPECT_EQ(pnanovdb_editor::param_map_stack_depth(&editor), 0u);
            editor.unmap_params(&editor, scene_token, name_token);
            EXPECT_EQ(pnanovdb_editor::param_map_stack_depth(&editor), 0u);
        });
    thread_b.join();

    // From the main thread we can also confirm the pin is still held by A.
    EXPECT_EQ(pnanovdb_editor::shader_name_map_ref_count(&editor, scene_token, name_token), 1u);

    b_finished.store(true, std::memory_order_release);
    thread_a.join();

    EXPECT_EQ(pnanovdb_editor::shader_name_map_ref_count(&editor, scene_token, name_token), 0u);
    EXPECT_EQ(pnanovdb_editor::param_map_stack_depth(&editor), 0u);
}
