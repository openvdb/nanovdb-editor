// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0
//
//  Regression test for the --shader CLI bug

#include <gtest/gtest.h>

#include <nanovdb_editor/putil/Compiler.h>
#include <nanovdb_editor/putil/Compute.h>
#include <nanovdb_editor/putil/Editor.h>

#include "editor/Editor.h" // pnanovdb_editor_impl_t
#include "editor/EditorSceneManager.h"
#include "EditorTestSupport.h"
#include "ShaderMappingTestSupport.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <thread>
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
    }

    struct MaterialSnapshot
    {
        pnanovdb_editor_token_t* shader_name = nullptr;
        std::shared_ptr<pnanovdb_compute_array_t> owner;
        std::vector<pnanovdb_editor::ShaderParamLayout> layout;
        std::vector<char> bytes;

        bool operator==(const MaterialSnapshot& other) const
        {
            return shader_name == other.shader_name && owner == other.owner && layout == other.layout &&
                   bytes == other.bytes;
        }
    };

    MaterialSnapshot snapshotMaterial()
    {
        MaterialSnapshot result;
        editor.impl->scene_manager->with_object(scene_token, name_token,
            [&](pnanovdb_editor::SceneObject* obj)
            {
                ASSERT_NE(obj, nullptr);
                result.shader_name = obj->shader_name();
                result.owner = obj->params.shader_params_array_owner;
                result.layout = obj->params.shader_params_layout;
                result.bytes.resize(kBufSize);
                std::memcpy(result.bytes.data(), obj->shader_params(), result.bytes.size());
            });
        return result;
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

TEST_F(ShaderNameSwapResetsParamsTest, WholeShaderMapAppliesSameShaderValuesOverDefaults)
{
    const float alpha = 0.75f;
    ASSERT_TRUE(pnanovdb_editor_test::map_shader_defaults(
        editor, compute, scene_token, name_token, default_editor_shader(),
        [&](pnanovdb_uint8_t* params) { std::memcpy(params, &alpha, sizeof(alpha)); }));
    auto expected = editor_defaults;
    std::memcpy(expected.data(), &alpha, sizeof(alpha));
    EXPECT_EQ(snapshotObjectBuffer(), expected);

    ASSERT_TRUE(pnanovdb_editor_test::map_shader_defaults(editor, compute, scene_token, name_token, alt_shader()));
    EXPECT_EQ(snapshotObjectBuffer(), alt_defaults);
}

TEST_F(ShaderNameSwapResetsParamsTest, WholeShaderMapCommitsOnlyOnOutermostUnmap)
{
    const auto original = snapshotMaterial();
    worker->params_dirty.store(false);
    const auto* type = PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_editor_shader_t);
    auto* mapped = static_cast<pnanovdb_editor_shader_t*>(editor.map_params(&editor, scene_token, name_token, type));
    ASSERT_NE(mapped, nullptr);
    EXPECT_EQ(mapped->shader_name, original.shader_name);
    EXPECT_EQ(std::memcmp(mapped->shader_params, original.bytes.data(), original.bytes.size()), 0);
    auto* nested = static_cast<pnanovdb_editor_shader_t*>(editor.map_params(&editor, scene_token, name_token, type));
    ASSERT_EQ(nested, mapped);

    auto expected = alt_defaults;
    const float alpha = 0.375f;
    std::memcpy(expected.data(), &alpha, sizeof(alpha));
    mapped->shader_name = editor.get_token(alt_shader());
    std::memcpy(mapped->shader_params, expected.data(), expected.size());
    EXPECT_EQ(snapshotMaterial(), original);
    editor.unmap_params(&editor, scene_token, name_token);
    EXPECT_EQ(snapshotMaterial(), original);
    EXPECT_FALSE(worker->params_dirty.load());
    editor.unmap_params(&editor, scene_token, name_token);

    const auto actual = snapshotMaterial();
    EXPECT_EQ(actual.shader_name, editor.get_token(alt_shader()));
    EXPECT_EQ(actual.bytes, expected);
    EXPECT_EQ(actual.layout, pnanovdb_editor::EditorSceneManager::load_shader_params_layout(alt_shader()));
    EXPECT_NE(actual.owner, original.owner);
    EXPECT_TRUE(worker->params_dirty.load());
}

TEST_F(ShaderNameSwapResetsParamsTest, ConcurrentWholeShaderMapsKeepIndependentMaterialsWithoutWorker)
{
    editor.impl->editor_worker.reset();
    worker.reset();
    const auto* type = PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_editor_shader_t);
    auto* first = static_cast<pnanovdb_editor_shader_t*>(editor.map_params(&editor, scene_token, name_token, type));
    ASSERT_NE(first, nullptr);
    auto* nested = editor.map_params(&editor, scene_token, name_token, type);
    EXPECT_EQ(nested, first);

    auto expected = editor_defaults;
    const float alpha = 0.25f;
    std::memcpy(expected.data(), &alpha, sizeof(alpha));
    std::memcpy(first->shader_params, expected.data(), expected.size());
    const auto* name_type = PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_editor_shader_name_t);
    std::thread writer(
        [&]()
        {
            EXPECT_EQ(editor.map_params(&editor, scene_token, name_token, name_type), nullptr);
            auto* second =
                static_cast<pnanovdb_editor_shader_t*>(editor.map_params(&editor, scene_token, name_token, type));
            ASSERT_NE(second, nullptr);
            EXPECT_NE(second, first);
            second->shader_name = editor.get_token(alt_shader());
            std::memcpy(second->shader_params, alt_defaults.data(), alt_defaults.size());
            editor.unmap_params(&editor, scene_token, name_token);
            EXPECT_EQ(editor.map_params(&editor, scene_token, name_token, name_type), nullptr);
        });
    writer.join();

    EXPECT_EQ(snapshotMaterial().shader_name, editor.get_token(alt_shader()));
    EXPECT_EQ(snapshotObjectBuffer(), alt_defaults);
    EXPECT_EQ(std::memcmp(first->shader_params, expected.data(), expected.size()), 0);
    first->shader_name = editor.get_token(default_editor_shader());
    if (nested)
    {
        editor.unmap_params(&editor, scene_token, name_token);
        EXPECT_EQ(snapshotObjectBuffer(), alt_defaults);
    }
    editor.unmap_params(&editor, scene_token, name_token);
    EXPECT_EQ(snapshotMaterial().shader_name, editor.get_token(default_editor_shader()));
    EXPECT_EQ(snapshotObjectBuffer(), expected);
    ASSERT_NE(editor.map_params(&editor, scene_token, name_token, name_type), nullptr);
    editor.unmap_params(&editor, scene_token, name_token);
}

TEST_F(ShaderNameSwapResetsParamsTest, UnchangedAndRevertedWholeShaderMapsPreserveMaterial)
{
    const auto original = snapshotMaterial();
    const auto* type = PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_editor_shader_t);
    for (bool revert_edit : { false, true })
    {
        worker->params_dirty.store(false);
        auto* mapped = static_cast<pnanovdb_editor_shader_t*>(editor.map_params(&editor, scene_token, name_token, type));
        ASSERT_NE(mapped, nullptr);
        if (revert_edit)
        {
            mapped->shader_name = editor.get_token(alt_shader());
            std::memcpy(mapped->shader_params, alt_defaults.data(), alt_defaults.size());
            EXPECT_EQ(snapshotMaterial(), original);
            mapped->shader_name = original.shader_name;
            std::memcpy(mapped->shader_params, original.bytes.data(), original.bytes.size());
        }
        editor.unmap_params(&editor, scene_token, name_token);
        EXPECT_EQ(snapshotMaterial(), original);
        EXPECT_FALSE(worker->params_dirty.load());
    }
}

TEST_F(ShaderNameSwapResetsParamsTest, UnchangedWholeShaderMapPreservesAnotherThreadCommit)
{
    editor.impl->editor_worker.reset();
    worker.reset();
    const auto* type = PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_editor_shader_t);
    ASSERT_NE(editor.map_params(&editor, scene_token, name_token, type), nullptr);
    std::thread writer(
        [&]()
        {
            EXPECT_TRUE(pnanovdb_editor_test::map_shader_defaults(editor, compute, scene_token, name_token, alt_shader()));
        });
    writer.join();

    const auto committed = snapshotMaterial();
    EXPECT_EQ(committed.shader_name, editor.get_token(alt_shader()));
    EXPECT_EQ(committed.bytes, alt_defaults);
    editor.unmap_params(&editor, scene_token, name_token);
    EXPECT_EQ(snapshotMaterial(), committed);
}

TEST_F(ShaderNameSwapResetsParamsTest, WholeShaderMapAndShaderNameMapCannotOverlap)
{
    const auto original = snapshotMaterial();
    const auto* material_type = PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_editor_shader_t);
    const auto* name_type = PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_editor_shader_name_t);
    for (bool material_first : { false, true })
    {
        const auto* first_type = material_first ? material_type : name_type;
        const auto* second_type = material_first ? name_type : material_type;
        ASSERT_NE(editor.map_params(&editor, scene_token, name_token, first_type), nullptr);
        EXPECT_EQ(editor.map_params(&editor, scene_token, name_token, second_type), nullptr);
        editor.unmap_params(&editor, scene_token, name_token);
        ASSERT_NE(editor.map_params(&editor, scene_token, name_token, second_type), nullptr);
        editor.unmap_params(&editor, scene_token, name_token);
        EXPECT_EQ(snapshotMaterial(), original);
    }
}

TEST_F(ShaderNameSwapResetsParamsTest, MissingWholeShaderMapPreservesMaterial)
{
    const auto original = snapshotMaterial();
    const auto* type = PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_editor_shader_t);
    EXPECT_EQ(editor.map_params(&editor, scene_token, editor.get_token("missing"), type), nullptr);
    EXPECT_EQ(snapshotMaterial(), original);
    ASSERT_NE(editor.map_params(&editor, scene_token, name_token, type), nullptr);
    editor.unmap_params(&editor, scene_token, name_token);
    EXPECT_EQ(snapshotMaterial(), original);
}

TEST_F(ShaderNameSwapResetsParamsTest, WholeShaderMapCannotOverwriteReplacementObject)
{
    const auto* type = PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_editor_shader_t);
    auto* mapped = static_cast<pnanovdb_editor_shader_t*>(editor.map_params(&editor, scene_token, name_token, type));
    ASSERT_NE(mapped, nullptr);
    mapped->shader_name = editor.get_token(alt_shader());
    std::memcpy(mapped->shader_params, alt_defaults.data(), alt_defaults.size());

    ASSERT_TRUE(editor.impl->scene_manager->remove(scene_token, name_token));
    editor.add_nanovdb_2(&editor, scene_token, name_token, owned_array);
    const auto replacement = snapshotMaterial();
    editor.unmap_params(&editor, scene_token, name_token);
    EXPECT_EQ(snapshotMaterial(), replacement);
    EXPECT_EQ(replacement.shader_name, editor.get_token(default_editor_shader()));
}

TEST_F(ShaderNameSwapResetsParamsTest, RawBufferUpdatePreservesShaderStateAndOwnsCopy)
{
    ASSERT_TRUE(pnanovdb_editor_test::map_shader_defaults(editor, compute, scene_token, name_token, alt_shader()));
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
    ASSERT_TRUE(editor.update_nanovdb_buffer(&editor, scene_token, name_token, replacement));
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
