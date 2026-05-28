// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <nanovdb_editor/putil/Compiler.h>
#include <nanovdb_editor/putil/Compute.h>
#include <nanovdb_editor/putil/Editor.h>

#include "editor/Editor.h" // pnanovdb_editor_impl_t, s_default_editor_shader
#include "editor/EditorSceneManager.h"
#include "EditorTestSupport.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <vector>

namespace
{

constexpr const char* kDefaultShaderFile = "editor/shaders/editor.slang";

class ShaderParamsResetToDefaultsTest : public ::testing::Test
{
protected:
    pnanovdb_compiler_t compiler{};
    pnanovdb_compute_t compute{};
    pnanovdb_editor_t editor{};
    pnanovdb_compiler_instance_t* compiler_inst = nullptr;

    pnanovdb_editor_token_t* scene_token = nullptr;
    pnanovdb_editor_token_t* name_a = nullptr;
    pnanovdb_editor_token_t* name_b = nullptr;
    pnanovdb_compute_array_t* owned_array_a = nullptr;
    pnanovdb_compute_array_t* owned_array_b = nullptr;

    static constexpr size_t kBufSize = PNANOVDB_COMPUTE_CONSTANT_BUFFER_MAX_SIZE;

    // JSON defaults captured before any pool mutation so we have a stable
    // ground truth to compare every subsequent pool/object snapshot against.
    std::vector<char> editor_defaults;

    bool compileToCache(const char* rel_path)
    {
        const std::filesystem::path shader = std::filesystem::path(__FILE__).parent_path().parent_path() / rel_path;
        if (!std::filesystem::exists(shader))
        {
            return false;
        }
        pnanovdb_compiler_settings_t settings{};
        pnanovdb_compiler_settings_init(&settings);
        settings.compile_target = PNANOVDB_COMPILE_TARGET_VULKAN;
        std::strcpy(settings.entry_point_name, "main");
        const std::string path = shader.string();
        return compiler.compile_shader_from_file(compiler_inst, path.c_str(), &settings, nullptr) != PNANOVDB_FALSE;
    }

    std::vector<char> capturePoolBytes(const char* shader_name)
    {
        std::vector<char> out(kBufSize, 0);
        const size_t written = pnanovdb_editor::capture_shader_default_params(
            *editor.impl->scene_manager, editor.impl->compute, shader_name, out.size(), out.data());
        if (written == 0u)
        {
            out.clear();
        }
        else
        {
            out.resize(written);
        }
        return out;
    }

    void stampPoolWithPattern(const char* shader_name, uint8_t pattern)
    {
        std::vector<uint8_t> stamp(kBufSize, pattern);
        pnanovdb_compute_array_t* stamp_arr = compute.create_array(sizeof(uint8_t), stamp.size(), stamp.data());
        ASSERT_NE(stamp_arr, nullptr);
        editor.impl->scene_manager->shader_params.set_compute_array_for_shader(shader_name, stamp_arr);
        compute.destroy_array(stamp_arr);
    }

    void SetUp() override
    {
        pnanovdb_compiler_load(&compiler);
        ASSERT_NE(compiler.module, nullptr) << "Compiler module not available";
        compiler_inst = compiler.create_instance();
        ASSERT_NE(compiler_inst, nullptr);

        pnanovdb_compute_load(&compute, &compiler);
        ASSERT_NE(compute.module, nullptr);

        // ShaderParams::load() needs the compiled-shader JSON alongside the
        // source-level <shader>.json to populate JSON defaults. Pre-warm the
        // cache so this test doesn't depend on a prior run.
        if (!compileToCache(kDefaultShaderFile))
        {
            GTEST_SKIP() << "Slang compilation unavailable in this environment";
        }

        pnanovdb_editor_load(&editor, &compute, &compiler);
        ASSERT_NE(editor.module, nullptr);
        ASSERT_NE(editor.impl, nullptr);
        ASSERT_NE(editor.impl->scene_manager, nullptr);
        ASSERT_NE(editor.impl->compute, nullptr);

        // Capture the pool *before* any object exists so we record the actual
        // JSON defaults (capture_shader_default_params returns live pool
        // state, which equals defaults only on a fresh pool).
        editor_defaults = capturePoolBytes(pnanovdb_editor::s_default_editor_shader);
        ASSERT_FALSE(editor_defaults.empty())
            << "Could not load JSON defaults for " << pnanovdb_editor::s_default_editor_shader;

        scene_token = editor.get_token("reset_defaults_scene");
        name_a = editor.get_token("reset_defaults_object_a");
        name_b = editor.get_token("reset_defaults_object_b");
        ASSERT_NE(scene_token, nullptr);
        ASSERT_NE(name_a, nullptr);
        ASSERT_NE(name_b, nullptr);

        std::array<uint8_t, 16> bytes{};
        owned_array_a = compute.create_array(sizeof(uint8_t), bytes.size(), bytes.data());
        owned_array_b = compute.create_array(sizeof(uint8_t), bytes.size(), bytes.data());
        ASSERT_NE(owned_array_a, nullptr);
        ASSERT_NE(owned_array_b, nullptr);

        // Both objects use s_default_editor_shader; refresh_params_for_shader
        // should hit them both.
        editor.add_nanovdb_2(&editor, scene_token, name_a, owned_array_a);
        editor.add_nanovdb_2(&editor, scene_token, name_b, owned_array_b);
    }

    void TearDown() override
    {
        if (editor.impl)
        {
            editor.remove(&editor, scene_token, name_a);
            editor.remove(&editor, scene_token, name_b);
            pnanovdb_editor_free(&editor);
        }
        if (owned_array_a)
        {
            compute.destroy_array(owned_array_a);
        }
        if (owned_array_b)
        {
            compute.destroy_array(owned_array_b);
        }
        if (compiler_inst)
        {
            compiler.destroy_instance(compiler_inst);
        }
        pnanovdb_compute_free(&compute);
        pnanovdb_compiler_free(&compiler);
    }
};

} // namespace

// Sanity: capturePoolBytes on a fresh-load pool returns the JSON defaults.
TEST_F(ShaderParamsResetToDefaultsTest, FreshPoolMatchesJsonDefaults)
{
    const auto snap = capturePoolBytes(pnanovdb_editor::s_default_editor_shader);
    ASSERT_EQ(snap.size(), editor_defaults.size());
    EXPECT_EQ(std::memcmp(snap.data(), editor_defaults.data(), snap.size()), 0)
        << "Adding objects should not have mutated the pool away from JSON defaults";
}

// After stamping the pool with non-default bytes, reset_shader_params_to_defaults
// must put it back to JSON defaults — proving the cached default_value path
// produces the right answer.
TEST_F(ShaderParamsResetToDefaultsTest, ResetRestoresPoolToJsonDefaults)
{
    stampPoolWithPattern(pnanovdb_editor::s_default_editor_shader, 0x55);

    auto stamped = capturePoolBytes(pnanovdb_editor::s_default_editor_shader);
    ASSERT_EQ(stamped.size(), editor_defaults.size());
    ASSERT_NE(std::memcmp(stamped.data(), editor_defaults.data(), stamped.size()), 0)
        << "Precondition: stamped pool should differ from defaults";

    ASSERT_TRUE(editor.impl->scene_manager->reset_shader_params_to_defaults(
        &compute, pnanovdb_editor::s_default_editor_shader));

    const auto after = capturePoolBytes(pnanovdb_editor::s_default_editor_shader);
    ASSERT_EQ(after.size(), editor_defaults.size());
    EXPECT_EQ(std::memcmp(after.data(), editor_defaults.data(), after.size()), 0)
        << "Pool was not restored to JSON defaults after reset_shader_params_to_defaults";
}

// Reset must refresh *every* NanoVDB using the shader, not only the current
// viewport's object. This is the V3 (PR #182-style proactive refresh)
// guarantee — without it, secondary objects would only catch up on their
// next render.
TEST_F(ShaderParamsResetToDefaultsTest, ResetRefreshesPerObjectBuffersOfAllObjects)
{
    const size_t copy_size = editor_defaults.size();

    void* ptr_a = pnanovdb_editor_test::get_object_shader_params_ptr(&editor, scene_token, name_a);
    void* ptr_b = pnanovdb_editor_test::get_object_shader_params_ptr(&editor, scene_token, name_b);
    ASSERT_NE(ptr_a, nullptr);
    ASSERT_NE(ptr_b, nullptr);

    // Sanity: both per-object buffers were initialised to JSON defaults by
    // add_nanovdb_2 -> create_initialized_shader_params.
    std::vector<char> snap(copy_size);
    std::memcpy(snap.data(), ptr_a, copy_size);
    ASSERT_EQ(std::memcmp(snap.data(), editor_defaults.data(), copy_size), 0)
        << "A's buffer should start at JSON defaults";
    std::memcpy(snap.data(), ptr_b, copy_size);
    ASSERT_EQ(std::memcmp(snap.data(), editor_defaults.data(), copy_size), 0)
        << "B's buffer should start at JSON defaults";

    // Stamp non-default bytes into both per-object buffers AND the pool, so
    // any code path (pool readback or lazy sync) would still see staleness
    // until the proactive refresh fires.
    std::memset(ptr_a, 0xAA, copy_size);
    std::memset(ptr_b, 0xAA, copy_size);
    stampPoolWithPattern(pnanovdb_editor::s_default_editor_shader, 0x77);

    ASSERT_TRUE(editor.impl->scene_manager->reset_shader_params_to_defaults(
        &compute, pnanovdb_editor::s_default_editor_shader));

    // refresh_params_for_shader reallocates each object's params array, so
    // the data pointer may have moved.
    void* ptr_a_after = pnanovdb_editor_test::get_object_shader_params_ptr(&editor, scene_token, name_a);
    void* ptr_b_after = pnanovdb_editor_test::get_object_shader_params_ptr(&editor, scene_token, name_b);
    ASSERT_NE(ptr_a_after, nullptr);
    ASSERT_NE(ptr_b_after, nullptr);

    std::memcpy(snap.data(), ptr_a_after, copy_size);
    EXPECT_EQ(std::memcmp(snap.data(), editor_defaults.data(), copy_size), 0)
        << "Object A's buffer was not refreshed to defaults — proactive refresh did not fire";
    std::memcpy(snap.data(), ptr_b_after, copy_size);
    EXPECT_EQ(std::memcmp(snap.data(), editor_defaults.data(), copy_size), 0)
        << "Object B's buffer was not refreshed to defaults — refresh missed a sibling object";
}

// Reset is idempotent and safe to call when the pool is already at defaults.
TEST_F(ShaderParamsResetToDefaultsTest, ResetIsIdempotent)
{
    auto& scene_manager = *editor.impl->scene_manager;

    ASSERT_TRUE(scene_manager.reset_shader_params_to_defaults(&compute, pnanovdb_editor::s_default_editor_shader));
    ASSERT_TRUE(scene_manager.reset_shader_params_to_defaults(&compute, pnanovdb_editor::s_default_editor_shader));

    const auto snap = capturePoolBytes(pnanovdb_editor::s_default_editor_shader);
    ASSERT_EQ(snap.size(), editor_defaults.size());
    EXPECT_EQ(std::memcmp(snap.data(), editor_defaults.data(), snap.size()), 0)
        << "Repeated reset must keep the pool at JSON defaults";
}

// Reset rejects invalid inputs cleanly (does not crash, returns false).
TEST_F(ShaderParamsResetToDefaultsTest, ResetRejectsInvalidShaderName)
{
    auto& scene_manager = *editor.impl->scene_manager;

    EXPECT_FALSE(scene_manager.reset_shader_params_to_defaults(&compute, nullptr));
    EXPECT_FALSE(scene_manager.reset_shader_params_to_defaults(&compute, ""));
    EXPECT_FALSE(scene_manager.reset_shader_params_to_defaults(&compute, "editor/does_not_exist.slang"));
}
