// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0
//
//  Regression test for the --shader CLI bug

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

// Shader name as the editor sees it (relative to the install-time shaders dir).
constexpr const char* kAltShaderName = "editor/image2d.slang";
// Absolute file path components for direct invocation of the Slang compiler.
constexpr const char* kAltShaderFile = "editor/shaders/image2d.slang";
constexpr const char* kDefaultShaderFile = "editor/shaders/editor.slang";

class ShaderNameSwapResetsParamsTest : public ::testing::Test
{
protected:
    pnanovdb_compiler_t compiler{};
    pnanovdb_compute_t compute{};
    pnanovdb_editor_t editor{};
    pnanovdb_compiler_instance_t* compiler_inst = nullptr;

    pnanovdb_editor_token_t* scene_token = nullptr;
    pnanovdb_editor_token_t* name_token = nullptr;
    pnanovdb_compute_array_t* owned_array = nullptr;

    static constexpr size_t kBufSize = PNANOVDB_COMPUTE_CONSTANT_BUFFER_MAX_SIZE;

    // Captured "ground truth" bytes for each shader's JSON-default buffer.
    std::vector<char> editor_defaults;
    std::vector<char> alt_defaults;

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

    std::vector<char> captureDefaults(const char* shader_name)
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

    void SetUp() override
    {
        pnanovdb_compiler_load(&compiler);
        ASSERT_NE(compiler.module, nullptr) << "Compiler module not available";
        compiler_inst = compiler.create_instance();
        ASSERT_NE(compiler_inst, nullptr);

        pnanovdb_compute_load(&compute, &compiler);
        ASSERT_NE(compute.module, nullptr);

        // The compiled-shader JSON (alongside the source-level <shader>.json)
        // must exist on disk for ShaderParams::load() to populate the pool
        // with non-zero defaults. Pre-compile both shaders so the test does
        // not depend on a prior run having warmed the cache.
        if (!compileToCache(kDefaultShaderFile) || !compileToCache(kAltShaderFile))
        {
            GTEST_SKIP() << "Slang compilation unavailable in this environment";
        }

        pnanovdb_editor_load(&editor, &compute, &compiler);
        ASSERT_NE(editor.module, nullptr);
        ASSERT_NE(editor.impl, nullptr);
        ASSERT_NE(editor.impl->scene_manager, nullptr);
        ASSERT_NE(editor.impl->compute, nullptr);

        // Capture per-shader JSON defaults before adding the object. Doing it
        // first means the pool entries are populated from JSON (not from any
        // later object buffer) so these captures are the canonical defaults.
        alt_defaults = captureDefaults(kAltShaderName);
        editor_defaults = captureDefaults(pnanovdb_editor::s_default_editor_shader);
        ASSERT_FALSE(alt_defaults.empty()) << "Could not load JSON defaults for " << kAltShaderName;
        ASSERT_FALSE(editor_defaults.empty())
            << "Could not load JSON defaults for " << pnanovdb_editor::s_default_editor_shader;
        ASSERT_EQ(alt_defaults.size(), editor_defaults.size()) << "Both shaders allocate the 64KB constant buffer";

        // The two shaders' default buffers must differ; otherwise the test
        // cannot distinguish a stale buffer from a freshly-refreshed one.
        ASSERT_NE(std::memcmp(alt_defaults.data(), editor_defaults.data(), alt_defaults.size()), 0)
            << "Test precondition: the two shaders' JSON defaults must differ";

        scene_token = editor.get_token("shader_swap_scene");
        name_token = editor.get_token("shader_swap_object");
        ASSERT_NE(scene_token, nullptr);
        ASSERT_NE(name_token, nullptr);

        std::array<uint8_t, 16> bytes{};
        owned_array = compute.create_array(sizeof(uint8_t), bytes.size(), bytes.data());
        ASSERT_NE(owned_array, nullptr);

        // add_nanovdb_2() uses editor->impl->shader_name (s_default_editor_shader)
        // and creates the per-object params buffer pre-populated with its
        // JSON defaults.
        editor.add_nanovdb_2(&editor, scene_token, name_token, owned_array);
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
        if (compiler_inst)
        {
            compiler.destroy_instance(compiler_inst);
        }
        pnanovdb_compute_free(&compute);
        pnanovdb_compiler_free(&compiler);
    }

    std::vector<char> snapshotObjectBuffer()
    {
        std::vector<char> out(kBufSize, 0);
        void* ptr = pnanovdb_editor_test::get_object_shader_params_ptr(&editor, scene_token, name_token);
        if (ptr)
        {
            std::memcpy(out.data(), ptr, kBufSize);
        }
        return out;
    }
};

} // namespace

// Sanity: a freshly-added object holds the default shader's JSON defaults.
TEST_F(ShaderNameSwapResetsParamsTest, FreshlyAddedObjectMatchesDefaultShaderDefaults)
{
    const auto buf = snapshotObjectBuffer();
    ASSERT_EQ(buf.size(), editor_defaults.size());
    EXPECT_EQ(std::memcmp(buf.data(), editor_defaults.data(), editor_defaults.size()), 0)
        << "add_nanovdb_2 should leave the object's buffer initialised from the default shader's JSON";
}

// Regression: after swapping the shader name via map_params/unmap_params, the
// per-object buffer must reflect the new shader's JSON defaults, not the old
// one. Pre-fix this assertion fails on a cache hit: the buffer still holds
// editor.slang's bytes, which the renderer then copies into the new shader's
// parameter pool, presenting as "garbage" UI defaults.
TEST_F(ShaderNameSwapResetsParamsTest, SwappingShaderNameResetsObjectParamsToNewDefaults)
{
    const pnanovdb_reflect_data_type_t* name_type = PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_editor_shader_name_t);

    auto* mapped =
        static_cast<pnanovdb_editor_shader_name_t*>(editor.map_params(&editor, scene_token, name_token, name_type));
    ASSERT_NE(mapped, nullptr);
    mapped->shader_name = editor.get_token(kAltShaderName);
    editor.unmap_params(&editor, scene_token, name_token);

    const auto buf = snapshotObjectBuffer();
    ASSERT_EQ(buf.size(), alt_defaults.size());

    EXPECT_EQ(std::memcmp(buf.data(), alt_defaults.data(), alt_defaults.size()), 0)
        << "Regression: object's params buffer was not refreshed for the new shader";
    EXPECT_NE(std::memcmp(buf.data(), editor_defaults.data(), editor_defaults.size()), 0)
        << "Regression: object's params buffer still contains the previous shader's defaults";
}

// A no-op swap (re-assigning the same shader name) must not disturb the
// object's existing buffer. This guards the optimisation that only refreshes
// on an *actual* shader name change so callers do not lose user-modified
// parameter values just by opening and closing the map.
TEST_F(ShaderNameSwapResetsParamsTest, ReassigningSameShaderNamePreservesUserBytes)
{
    void* ptr = pnanovdb_editor_test::get_object_shader_params_ptr(&editor, scene_token, name_token);
    ASSERT_NE(ptr, nullptr);

    // Stamp a recognisable pattern over the first few bytes of the buffer.
    static constexpr size_t kSentinelBytes = 32;
    std::array<uint8_t, kSentinelBytes> sentinel{};
    for (size_t i = 0; i < kSentinelBytes; ++i)
    {
        sentinel[i] = static_cast<uint8_t>(0xC0 | (i & 0x0F));
    }
    std::memcpy(ptr, sentinel.data(), sentinel.size());

    const pnanovdb_reflect_data_type_t* name_type = PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_editor_shader_name_t);
    auto* mapped =
        static_cast<pnanovdb_editor_shader_name_t*>(editor.map_params(&editor, scene_token, name_token, name_type));
    ASSERT_NE(mapped, nullptr);
    // Re-assign the same shader name (no actual change).
    mapped->shader_name = editor.get_token(pnanovdb_editor::s_default_editor_shader);
    editor.unmap_params(&editor, scene_token, name_token);

    const auto buf = snapshotObjectBuffer();
    EXPECT_EQ(std::memcmp(buf.data(), sentinel.data(), sentinel.size()), 0)
        << "Same-shader-name unmap must not clobber the object's existing buffer";
}
