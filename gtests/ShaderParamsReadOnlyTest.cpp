// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0
//

#include <gtest/gtest.h>

#include <nanovdb_editor/putil/Compiler.h>
#include <nanovdb_editor/putil/Compute.h>
#include <nanovdb_editor/putil/Editor.h>

#include "EditorTestSupport.h"

#include <array>
#include <cstdint>
#include <cstring>

namespace
{

class ShaderParamsReadOnlyTest : public ::testing::Test
{
protected:
    pnanovdb_compiler_t compiler{};
    pnanovdb_compute_t compute{};
    pnanovdb_editor_t editor{};

    pnanovdb_editor_token_t* scene_token = nullptr;
    pnanovdb_editor_token_t* name_a = nullptr;
    pnanovdb_editor_token_t* name_b = nullptr;

    pnanovdb_compute_array_t* array_a = nullptr;
    pnanovdb_compute_array_t* array_b = nullptr;

    // Buffers owned by the test that stand in for real shader_params storage. The
    // debug_set_object_shader_params hook points each object at its own buffer, so
    // A and B have distinct bytes that a correct snapshot must preserve.
    static constexpr size_t kParamsSize = 32;
    std::array<uint8_t, kParamsSize> params_a{};
    std::array<uint8_t, kParamsSize> params_b{};

    void SetUp() override
    {
        pnanovdb_compiler_load(&compiler);
        ASSERT_NE(compiler.module, nullptr);
        pnanovdb_compute_load(&compute, &compiler);
        ASSERT_NE(compute.module, nullptr);
        pnanovdb_editor_load(&editor, &compute, &compiler);
        ASSERT_NE(editor.module, nullptr);
        ASSERT_NE(editor.impl, nullptr);

        scene_token = editor.get_token("snapshot_test_scene");
        name_a = editor.get_token("snapshot_test_object_A");
        name_b = editor.get_token("snapshot_test_object_B");
        ASSERT_NE(scene_token, nullptr);
        ASSERT_NE(name_a, nullptr);
        ASSERT_NE(name_b, nullptr);

        std::array<uint8_t, 16> bytes{};
        array_a = compute.create_array(sizeof(uint8_t), bytes.size(), bytes.data());
        array_b = compute.create_array(sizeof(uint8_t), bytes.size(), bytes.data());
        ASSERT_NE(array_a, nullptr);
        ASSERT_NE(array_b, nullptr);

        editor.add_nanovdb_2(&editor, scene_token, name_a, array_a);
        editor.add_nanovdb_2(&editor, scene_token, name_b, array_b);

        // Populate distinct, easily-distinguishable bytes into each buffer.
        for (size_t i = 0; i < kParamsSize; ++i)
        {
            params_a[i] = static_cast<uint8_t>(0xA0 | (i & 0x0F));
            params_b[i] = static_cast<uint8_t>(0xB0 | (i & 0x0F));
        }

        const pnanovdb_reflect_data_type_t* stub_type = PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_editor_token_t);
        ASSERT_NE(stub_type, nullptr);
        ASSERT_EQ(
            pnanovdb_editor_test::set_object_shader_params(&editor, scene_token, name_a, params_a.data(), stub_type),
            PNANOVDB_TRUE);
        ASSERT_EQ(
            pnanovdb_editor_test::set_object_shader_params(&editor, scene_token, name_b, params_b.data(), stub_type),
            PNANOVDB_TRUE);
    }

    void TearDown() override
    {
        if (editor.impl)
        {
            editor.remove(&editor, scene_token, name_a);
            editor.remove(&editor, scene_token, name_b);
            pnanovdb_editor_free(&editor);
        }
        if (array_a)
        {
            compute.destroy_array(array_a);
        }
        if (array_b)
        {
            compute.destroy_array(array_b);
        }
        pnanovdb_compute_free(&compute);
        pnanovdb_compiler_free(&compiler);
    }
};

} // namespace

// The snapshot must return each object's own bytes, not a shared cache value.
// Pre-fix (SyncDirection::UiToView), both snapshots would have returned bytes
// tied to whichever object the UI had most recently selected/synced.
TEST_F(ShaderParamsReadOnlyTest, SnapshotReturnsPerObjectBytes)
{
    std::array<uint8_t, kParamsSize> out{};

    ASSERT_EQ(pnanovdb_editor_test::snapshot_object_shader_params(&editor, scene_token, name_a, out.data(), out.size()),
              out.size());
    EXPECT_EQ(std::memcmp(out.data(), params_a.data(), out.size()), 0)
        << "Object A snapshot did not match A's own shader_params";

    out.fill(0);
    ASSERT_EQ(pnanovdb_editor_test::snapshot_object_shader_params(&editor, scene_token, name_b, out.data(), out.size()),
              out.size());
    EXPECT_EQ(std::memcmp(out.data(), params_b.data(), out.size()), 0)
        << "Object B snapshot did not match B's own shader_params";
}

// Snapshots must not mutate the source buffer. Pre-fix, the UI-to-view sync
// would have written shared-cache bytes into obj->params.shader_params, so
// repeated snapshots of A interleaved with a snapshot of B would have
// progressively corrupted A's buffer.
TEST_F(ShaderParamsReadOnlyTest, SnapshotDoesNotMutateSource)
{
    const std::array<uint8_t, kParamsSize> baseline_a = params_a;
    const std::array<uint8_t, kParamsSize> baseline_b = params_b;

    std::array<uint8_t, kParamsSize> out{};

    ASSERT_EQ(pnanovdb_editor_test::snapshot_object_shader_params(&editor, scene_token, name_a, out.data(), out.size()),
              out.size());
    ASSERT_EQ(pnanovdb_editor_test::snapshot_object_shader_params(&editor, scene_token, name_b, out.data(), out.size()),
              out.size());
    ASSERT_EQ(pnanovdb_editor_test::snapshot_object_shader_params(&editor, scene_token, name_a, out.data(), out.size()),
              out.size());

    EXPECT_EQ(std::memcmp(params_a.data(), baseline_a.data(), kParamsSize), 0) << "Snapshot mutated A's source buffer";
    EXPECT_EQ(std::memcmp(params_b.data(), baseline_b.data(), kParamsSize), 0) << "Snapshot mutated B's source buffer";
}

// The shader_params pointer itself must not be swapped out by a snapshot call.
// This guards against a future regression where the read path "helpfully"
// reassigns obj->params.shader_params (e.g. to a shared UI buffer).
TEST_F(ShaderParamsReadOnlyTest, SnapshotPreservesObjectBufferPointer)
{
    void* ptr_before_a = pnanovdb_editor_test::get_object_shader_params_ptr(&editor, scene_token, name_a);
    void* ptr_before_b = pnanovdb_editor_test::get_object_shader_params_ptr(&editor, scene_token, name_b);
    ASSERT_EQ(ptr_before_a, static_cast<void*>(params_a.data()));
    ASSERT_EQ(ptr_before_b, static_cast<void*>(params_b.data()));

    std::array<uint8_t, kParamsSize> out{};
    for (int i = 0; i < 4; ++i)
    {
        pnanovdb_editor_test::snapshot_object_shader_params(&editor, scene_token, name_a, out.data(), out.size());
        pnanovdb_editor_test::snapshot_object_shader_params(&editor, scene_token, name_b, out.data(), out.size());
    }

    EXPECT_EQ(pnanovdb_editor_test::get_object_shader_params_ptr(&editor, scene_token, name_a), ptr_before_a);
    EXPECT_EQ(pnanovdb_editor_test::get_object_shader_params_ptr(&editor, scene_token, name_b), ptr_before_b);
}

// End-to-end behaviour through the public map_params() API: writing per-object
// bytes via map_params(A) must stay in A's buffer and must not leak into B's.
// This composes the fix for Bug 3 with the map/unmap path and would have
// failed pre-fix because a subsequent render-loop snapshot of B would have
// written UI-cached values back into B.
TEST_F(ShaderParamsReadOnlyTest, MapParamsWritesAreIsolatedPerObject)
{
    const pnanovdb_reflect_data_type_t* stub_type = PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_editor_token_t);

    // Write distinctive patterns through map_params.
    void* ptr_a = editor.map_params(&editor, scene_token, name_a, stub_type);
    ASSERT_EQ(ptr_a, static_cast<void*>(params_a.data()));
    std::memset(ptr_a, 0x11, kParamsSize);
    editor.unmap_params(&editor, scene_token, name_a);

    void* ptr_b = editor.map_params(&editor, scene_token, name_b, stub_type);
    ASSERT_EQ(ptr_b, static_cast<void*>(params_b.data()));
    std::memset(ptr_b, 0x22, kParamsSize);
    editor.unmap_params(&editor, scene_token, name_b);

    // Snapshot each object; each must read back its own pattern.
    std::array<uint8_t, kParamsSize> out{};
    std::array<uint8_t, kParamsSize> expected_a{};
    std::array<uint8_t, kParamsSize> expected_b{};
    expected_a.fill(0x11);
    expected_b.fill(0x22);

    pnanovdb_editor_test::snapshot_object_shader_params(&editor, scene_token, name_a, out.data(), out.size());
    EXPECT_EQ(std::memcmp(out.data(), expected_a.data(), out.size()), 0)
        << "Regression: A's map_params write did not survive a subsequent B snapshot";

    pnanovdb_editor_test::snapshot_object_shader_params(&editor, scene_token, name_b, out.data(), out.size());
    EXPECT_EQ(std::memcmp(out.data(), expected_b.data(), out.size()), 0);

    // Snapshot A again; it must still carry A's pattern, not B's.
    pnanovdb_editor_test::snapshot_object_shader_params(&editor, scene_token, name_a, out.data(), out.size());
    EXPECT_EQ(std::memcmp(out.data(), expected_a.data(), out.size()), 0)
        << "Regression: A's shader_params overwritten by shared UI-cached state";
}
