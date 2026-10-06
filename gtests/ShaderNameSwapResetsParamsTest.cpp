// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0
//
//  Regression test for the --shader CLI bug

#include <gtest/gtest.h>

#include <nanovdb_editor/putil/Compiler.h>
#include <nanovdb_editor/putil/Compute.h>
#include <nanovdb_editor/putil/Editor.h>
#include <nanovdb_editor/putil/Shader.hpp>

#include "editor/Editor.h" // pnanovdb_editor_impl_t
#include "editor/EditorSceneManager.h"
#include "EditorTestSupport.h"

#include <array>
#include <chrono>
#include <fstream>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <vector>

namespace
{


inline const char* default_editor_shader()
{
    return pnanovdb_pipeline_get_shader_name(pnanovdb_pipeline_type_nanovdb_render);
}

inline const char* alt_shader()
{
    return pnanovdb_pipeline_get_shader_name(pnanovdb_pipeline_type_image2d_render);
}

class ShaderNameSwapResetsParamsTest : public ::testing::Test
{
protected:
    pnanovdb_compiler_t compiler{};
    pnanovdb_compute_t compute{};
    pnanovdb_editor_t editor{};
    std::shared_ptr<pnanovdb_editor::EditorWorker> worker;
    pnanovdb_compiler_instance_t* compiler_inst = nullptr;

    pnanovdb_editor_token_t* scene_token = nullptr;
    pnanovdb_editor_token_t* name_token = nullptr;
    pnanovdb_compute_array_t* owned_array = nullptr;

    static constexpr size_t kBufSize = PNANOVDB_COMPUTE_CONSTANT_BUFFER_MAX_SIZE;

    // Captured "ground truth" bytes for each shader's JSON-default buffer.
    std::vector<char> editor_defaults;
    std::vector<char> alt_defaults;
    std::vector<std::filesystem::path> fixture_files;

    std::string compileFixtureShader(const char* source)
    {
        const auto id = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto path = std::filesystem::temp_directory_path() / ("set_shader_" + std::to_string(id) + ".slang");
        std::ofstream(path) << source;
        fixture_files.push_back(path);
        fixture_files.push_back(pnanovdb_shader::getCompiledShaderParamsFilePath(path.string().c_str()));
        pnanovdb_compiler_settings_t settings{};
        pnanovdb_compiler_settings_init(&settings);
        settings.compile_target = PNANOVDB_COMPILE_TARGET_VULKAN;
        std::strcpy(settings.entry_point_name, "main");
        EXPECT_TRUE(compiler.compile_shader_from_file(compiler_inst, path.string().c_str(), &settings, nullptr));
        return path.string();
    }

    // Maps an editor-visible shader name (e.g. "editor/foo.slang") to its on-disk
    // source under editor/shaders/ and compiles it so the JSON cache is warm.
    bool compileToCache(const char* shader_name)
    {
        const std::filesystem::path repo_root = std::filesystem::path(__FILE__).parent_path().parent_path();
        const std::filesystem::path shader =
            repo_root / "editor" / "shaders" / std::filesystem::path(shader_name).filename();
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
        if (!compileToCache(default_editor_shader()) || !compileToCache(alt_shader()))
        {
            GTEST_SKIP() << "Slang compilation unavailable in this environment";
        }

        pnanovdb_editor_load(&editor, &compute, &compiler);
        ASSERT_NE(editor.module, nullptr);
        ASSERT_NE(editor.impl, nullptr);
        ASSERT_NE(editor.impl->scene_manager, nullptr);
        ASSERT_NE(editor.impl->compute, nullptr);
        worker = std::make_shared<pnanovdb_editor::EditorWorker>();
        worker->is_starting.store(false, std::memory_order_release);
        worker->render_thread_id.store(std::this_thread::get_id());
        editor.impl->editor_worker = worker;

        // Capture per-shader JSON defaults before adding the object. Doing it
        // first means the pool entries are populated from JSON (not from any
        // later object buffer) so these captures are the canonical defaults.
        alt_defaults = captureDefaults(alt_shader());
        editor_defaults = captureDefaults(default_editor_shader());
        ASSERT_FALSE(alt_defaults.empty()) << "Could not load JSON defaults for " << alt_shader();
        ASSERT_FALSE(editor_defaults.empty()) << "Could not load JSON defaults for " << default_editor_shader();
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

        // add_nanovdb_2() uses editor->impl->shader_name (the default editor shader)
        // and creates the per-object params buffer pre-populated with its
        // JSON defaults.
        editor.add_nanovdb_2(&editor, scene_token, name_token, owned_array);
    }

    void TearDown() override
    {
        if (editor.impl)
        {
            editor.impl->editor_worker = nullptr;
            worker.reset();
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
        for (const auto& path : fixture_files)
        {
            std::filesystem::remove(path);
        }
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
    mapped->shader_name = editor.get_token(alt_shader());
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
    mapped->shader_name = editor.get_token(default_editor_shader());
    editor.unmap_params(&editor, scene_token, name_token);

    const auto buf = snapshotObjectBuffer();
    EXPECT_EQ(std::memcmp(buf.data(), sentinel.data(), sentinel.size()), 0)
        << "Same-shader-name unmap must not clobber the object's existing buffer";
}

TEST_F(ShaderNameSwapResetsParamsTest, SetShaderAppliesObjectValuesOverDefaults)
{
    char error[256]{};
    ASSERT_TRUE(editor.set_shader(&editor, scene_token, name_token, default_editor_shader(),
                                  "{\"alpha_scale\":0.75}", error, sizeof(error))) << error;
    const auto actual = snapshotObjectBuffer();
    auto expected = editor_defaults;
    const float alpha = 0.75f;
    std::memcpy(expected.data(), &alpha, sizeof(alpha));
    EXPECT_EQ(actual, expected);

    ASSERT_TRUE(editor.set_shader(&editor, scene_token, name_token, alt_shader(), "{}", error, sizeof(error))) << error;
    EXPECT_EQ(snapshotObjectBuffer(), alt_defaults);
}

TEST_F(ShaderNameSwapResetsParamsTest, SetShaderRejectsInvalidValuesWithoutMutation)
{
    char error[256]{};
    for (const char* json : { "{\"unknown\":1}", "{\"alpha_scale\":\"bad\"}", "{\"slice_plane\":[1,2]}",
                              "{\"narrow_band_only\":-1}", "{\"alpha_scale\":1e100}" })
    {
        EXPECT_FALSE(
            editor.set_shader(&editor, scene_token, name_token, default_editor_shader(), json, error, sizeof(error)))
            << json;
        EXPECT_NE(error[0], '\0');
        EXPECT_EQ(snapshotObjectBuffer(), editor_defaults);
    }
    EXPECT_FALSE(editor.set_shader(
        &editor, scene_token, editor.get_token("missing"), default_editor_shader(), "{}", error, sizeof(error)));
}

TEST_F(ShaderNameSwapResetsParamsTest, RawBufferReplacementPreservesShaderStateAndOwnsCopy)
{
    char error[256]{};
    ASSERT_TRUE(editor.set_shader(&editor, scene_token, name_token, alt_shader(), "{}", error, sizeof(error))) << error;
    auto expected = snapshotObjectBuffer();
    const float edited_value = 0.375f;
    std::memcpy(expected.data(), &edited_value, sizeof(edited_value));

    std::shared_ptr<pnanovdb_compute_array_t> params_owner;
    std::weak_ptr<pnanovdb_compute_array_t> previous_array;
    editor.impl->scene_manager->with_object(scene_token, name_token,
                                            [&](pnanovdb_editor::SceneObject* obj)
                                            {
                                                ASSERT_NE(obj, nullptr);
                                                std::memcpy(obj->shader_params(), expected.data(), expected.size());
                                                params_owner = obj->params.shader_params_array_owner;
                                                previous_array = obj->resources.nanovdb_array_owner;
                                                obj->visible = false;
                                            });

    const std::array<uint8_t, 16> bytes{ 1, 3, 5, 7, 9, 11, 13, 15 };
    auto* replacement = compute.create_array(sizeof(uint8_t), bytes.size(), bytes.data());
    ASSERT_NE(replacement, nullptr);
    editor.add_nanovdb_2(&editor, scene_token, name_token, replacement);
    std::memset(replacement->data, 0, bytes.size());
    compute.destroy_array(replacement);

    EXPECT_TRUE(previous_array.expired());
    EXPECT_EQ(snapshotObjectBuffer(), expected);
    editor.impl->scene_manager->with_object(
        scene_token, name_token,
        [&](pnanovdb_editor::SceneObject* obj)
        {
            ASSERT_NE(obj, nullptr);
            EXPECT_EQ(obj->shader_name(), editor.get_token(alt_shader()));
            EXPECT_EQ(obj->params.shader_params_array_owner, params_owner);
            EXPECT_FALSE(obj->visible);
            ASSERT_NE(obj->nanovdb_array(), nullptr);
            EXPECT_EQ(std::memcmp(obj->nanovdb_array()->data, bytes.data(), bytes.size()), 0);
            EXPECT_EQ(obj->pipeline.load().output.get_array_owner(pnanovdb_editor::k_stage_output_nanovdb),
                      obj->resources.nanovdb_array_owner);
        });
}


TEST_F(ShaderNameSwapResetsParamsTest, SetShaderAcceptsParameterlessReflection)
{
    const auto shader = compileFixtureShader(R"(
RWTexture2D<float4> texture_out;
[shader("compute")][numthreads(1, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) { texture_out[id.xy] = float4(0, 1, 0, 0); }
)");
    char error[256]{};
    ASSERT_TRUE(editor.set_shader(&editor, scene_token, name_token, shader.c_str(), "{}", error, sizeof(error))) << error;
    const auto expected = snapshotObjectBuffer();
    EXPECT_TRUE(std::all_of(expected.begin(), expected.end(), [](char value) { return value == 0; }));
    EXPECT_FALSE(editor.set_shader(&editor, scene_token, name_token, shader.c_str(),
                                   "{\"unknown\":1}", error, sizeof(error)));
    EXPECT_NE(std::string(error).find("Unknown shader parameter"), std::string::npos);
    EXPECT_EQ(snapshotObjectBuffer(), expected);
    const auto missing_shader = shader + ".missing";
    EXPECT_FALSE(editor.set_shader(&editor, scene_token, name_token, missing_shader.c_str(), "{}", error, sizeof(error)));
    EXPECT_NE(std::string(error).find("reflection is unavailable"), std::string::npos);
    EXPECT_EQ(snapshotObjectBuffer(), expected);
}

TEST_F(ShaderNameSwapResetsParamsTest, SetShaderEncodesNumericHalfScalarsAndVectors)
{
    const auto shader = compileFixtureShader(R"(
struct shader_params_t { half value; half3 vector; };
ConstantBuffer<shader_params_t> shader_params;
RWTexture2D<float4> texture_out;
[shader("compute")][numthreads(1, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) { texture_out[id.xy] = float4(shader_params.value, shader_params.vector); }
)");
    char error[256]{};
    for (const char* value : { "1", "0.5" })
    {
        const auto json = std::string("{\"value\":") + value + ",\"vector\":[-2,0.5,65504]}";
        ASSERT_TRUE(editor.set_shader(&editor, scene_token, name_token, shader.c_str(),
                                      json.c_str(), error, sizeof(error))) << error;
        const auto actual = snapshotObjectBuffer();
        std::array<uint16_t, 4> bits{};
        std::memcpy(bits.data(), actual.data(), sizeof(bits));
        const uint16_t scalar = std::strcmp(value, "1") == 0 ? 0x3c00 : 0x3800;
        EXPECT_EQ(bits, (std::array<uint16_t, 4>{ scalar, 0xc000, 0x3800, 0x7bff }));
    }
    const auto expected = snapshotObjectBuffer();
    for (const char* json : { "{\"value\":1e10}", "{\"vector\":[1,1e10,2]}", "{\"value\":false}" })
    {
        EXPECT_FALSE(editor.set_shader(&editor, scene_token, name_token, shader.c_str(), json, error, sizeof(error)));
        EXPECT_NE(std::string(error).find("Invalid shader parameter"), std::string::npos);
        EXPECT_EQ(snapshotObjectBuffer(), expected);
    }
}
