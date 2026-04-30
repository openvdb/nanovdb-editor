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
#include <cstdint>
#include <cstring>

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

    // Storage to back regular map_params() calls without a real shader pipeline.
    // debug_set_object_shader_params points the object at this buffer.
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

        // Allocate a tiny nanovdb array placeholder; duplicate_array() inside
        // add_nanovdb_2 copies from it, so the pointer it stores is independent.
        std::array<uint8_t, 16> bytes{};
        owned_array = compute.create_array(sizeof(uint8_t), bytes.size(), bytes.data());
        ASSERT_NE(owned_array, nullptr);

        editor.add_nanovdb_2(&editor, scene_token, name_token, owned_array);

        // Install a known shader-params buffer on the object so map_params(regular)
        // returns non-null. Use the token's reflect type as a stand-in data type.
        const pnanovdb_reflect_data_type_t* stub_type = PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_editor_token_t);
        ASSERT_NE(stub_type, nullptr);
        ASSERT_EQ(pnanovdb_editor_test::set_object_shader_params(
                      &editor, scene_token, name_token, dummy_params_storage.data(), stub_type),
                  PNANOVDB_TRUE);

        // Preconditions: no pins, no pending unmaps on this thread.
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

// A plain regular map/unmap round-trip: stack grows by 1, pin count stays at 0.
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

// The pure shader-name map path: ref_count walks 0 -> 1 -> 0.
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

// The exact interleaving that triggered the bug. Pre-fix, the regular-params
// unmap would have erroneously called end_shader_name_map(), dropping
// ref_count from 1 to 0 between the two unmap calls.
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

    // The critical assertion: unmapping the Regular map must leave the shader-name
    // pin ref-count untouched. Pre-fix, ref_count dropped to 0 here.
    editor.unmap_params(&editor, scene_token, name_token);
    EXPECT_EQ(pnanovdb_editor::shader_name_map_ref_count(&editor, scene_token, name_token), 1u)
        << "Regression: regular-params unmap erroneously released shader-name pin";
    EXPECT_EQ(pnanovdb_editor::param_map_stack_depth(&editor), 1u);

    editor.unmap_params(&editor, scene_token, name_token);
    EXPECT_EQ(pnanovdb_editor::shader_name_map_ref_count(&editor, scene_token, name_token), 0u);
    EXPECT_EQ(pnanovdb_editor::param_map_stack_depth(&editor), 0u);
}

// Reverse nesting order: Regular first, then ShaderName. Each unmap must still
// release exactly the pin its matched map took.
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

    // Inner (LIFO top) is the shader-name map; this unmap must release it.
    editor.unmap_params(&editor, scene_token, name_token);
    EXPECT_EQ(pnanovdb_editor::shader_name_map_ref_count(&editor, scene_token, name_token), 0u);
    EXPECT_EQ(pnanovdb_editor::param_map_stack_depth(&editor), 1u);

    // Outer (now top) is the regular map; this unmap must not touch any pin.
    editor.unmap_params(&editor, scene_token, name_token);
    EXPECT_EQ(pnanovdb_editor::shader_name_map_ref_count(&editor, scene_token, name_token), 0u);
    EXPECT_EQ(pnanovdb_editor::param_map_stack_depth(&editor), 0u);
}

// An unmatched unmap_params() (no preceding successful map on this thread) must
// be a silent no-op. Pre-fix it could blindly decrement ref-counts.
TEST_F(MapPinTest, UnmatchedUnmapIsNoOp)
{
    ASSERT_EQ(pnanovdb_editor::param_map_stack_depth(&editor), 0u);
    editor.unmap_params(&editor, scene_token, name_token);
    EXPECT_EQ(pnanovdb_editor::shader_name_map_ref_count(&editor, scene_token, name_token), 0u);
    EXPECT_EQ(pnanovdb_editor::param_map_stack_depth(&editor), 0u);
}

// A failed map_params (returned nullptr because the data type does not match
// anything on the object) must leave the map-call stack untouched, so a
// caller that "pairs" it with an unmap does not pop an unrelated frame.
TEST_F(MapPinTest, FailedMapLeavesStackUntouched)
{
    const pnanovdb_reflect_data_type_t* stub_type = PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_editor_token_t);
    const pnanovdb_reflect_data_type_t* name_type = PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_editor_shader_name_t);

    // Prime the thread with a successful shader-name pin so we have something on
    // the stack for the failed map to potentially clobber.
    ASSERT_NE(editor.map_params(&editor, scene_token, name_token, name_type), nullptr);
    ASSERT_EQ(pnanovdb_editor::param_map_stack_depth(&editor), 1u);
    ASSERT_EQ(pnanovdb_editor::shader_name_map_ref_count(&editor, scene_token, name_token), 1u);

    // Map a regular params with a data_type that will match the one we installed
    // on the object in SetUp(). Confirm success to keep the test self-consistent.
    void* regular_ptr = editor.map_params(&editor, scene_token, name_token, stub_type);
    ASSERT_EQ(regular_ptr, static_cast<void*>(dummy_params_storage.data()));
    ASSERT_EQ(pnanovdb_editor::param_map_stack_depth(&editor), 2u);

    // Now try a mapping for a *different* object name: the lookup inside
    // map_params will find no SceneObject, so map returns nullptr and pushes no
    // frame. The stack depth must remain at 2.
    pnanovdb_editor_token_t* nonexistent = editor.get_token("pin_test_nonexistent_object");
    void* missing_ptr = editor.map_params(&editor, scene_token, nonexistent, stub_type);
    EXPECT_EQ(missing_ptr, nullptr);
    EXPECT_EQ(pnanovdb_editor::param_map_stack_depth(&editor), 2u)
        << "Failed map_params must not push a frame onto the map-call stack";

    // Unwind: Regular, then ShaderName. Pin bookkeeping should be clean.
    editor.unmap_params(&editor, scene_token, name_token);
    EXPECT_EQ(pnanovdb_editor::shader_name_map_ref_count(&editor, scene_token, name_token), 1u);
    editor.unmap_params(&editor, scene_token, name_token);
    EXPECT_EQ(pnanovdb_editor::shader_name_map_ref_count(&editor, scene_token, name_token), 0u);
    EXPECT_EQ(pnanovdb_editor::param_map_stack_depth(&editor), 0u);
}
