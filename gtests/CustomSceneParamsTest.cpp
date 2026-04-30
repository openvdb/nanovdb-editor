// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include "editor/CustomSceneParams.h"
#include "nanovdb_editor/putil/Reflect.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

TEST(NanoVDBEditor, CustomSceneParamsLoadsJsonFile)
{
    EXPECT_EQ(pnanovdb_reflect_type_from_string("int64"), PNANOVDB_REFLECT_TYPE_INT64);
    EXPECT_STREQ(pnanovdb_reflect_type_to_string(PNANOVDB_REFLECT_TYPE_INT64), "int64");

    const std::filesystem::path json_path =
        std::filesystem::temp_directory_path() / "nanovdb_editor_scene_params_test.json";

    {
        std::ofstream out(json_path);
        ASSERT_TRUE(out.good());
        out << R"json(
{
  "SceneParams": {
    "gain": {
      "type": "float",
      "value": 1.5,
      "min": 0.0,
      "max": 5.0,
      "step": 0.25
    },
    "toggle": {
      "type": "bool",
      "value": true
    },
    "offset": {
      "type": "int",
      "value": [1, 2, 3],
      "elementCount": 3,
      "useSlider": true
    }
  }
}
)json";
    }

    pnanovdb_editor::CustomSceneParams params;
    std::string error_message;
    ASSERT_TRUE(params.loadFromJsonFile(json_path.string(), &error_message)) << error_message;

    pnanovdb_shader_params_desc_t desc = {};
    ASSERT_TRUE(params.fillDesc("editor/test.slang", &desc));
    ASSERT_NE(desc.data, nullptr);
    ASSERT_EQ(desc.element_count, 3u);
    ASSERT_STREQ(desc.shader_name, "editor/test.slang");
    ASSERT_NE(desc.element_names, nullptr);
    ASSERT_NE(desc.element_typenames, nullptr);
    ASSERT_NE(desc.element_offsets, nullptr);

    EXPECT_STREQ(desc.element_names[0], "gain");
    EXPECT_STREQ(desc.element_typenames[0], "float");
    EXPECT_STREQ(desc.element_names[1], "toggle");
    EXPECT_STREQ(desc.element_typenames[1], "bool");
    EXPECT_STREQ(desc.element_names[2], "offset");
    EXPECT_STREQ(desc.element_typenames[2], "int[3]");

    const auto* raw = static_cast<const char*>(desc.data);
    EXPECT_FLOAT_EQ(*reinterpret_cast<const float*>(raw + desc.element_offsets[0]), 1.5f);
    EXPECT_EQ(*reinterpret_cast<const pnanovdb_bool_t*>(raw + desc.element_offsets[1]), PNANOVDB_TRUE);

    const auto* offset_values = reinterpret_cast<const pnanovdb_int32_t*>(raw + desc.element_offsets[2]);
    EXPECT_EQ(offset_values[0], 1);
    EXPECT_EQ(offset_values[1], 2);
    EXPECT_EQ(offset_values[2], 3);

    std::filesystem::remove(json_path);
}

TEST(NanoVDBEditor, CustomSceneParamsRejectsUnsupportedType)
{
    const std::filesystem::path json_path =
        std::filesystem::temp_directory_path() / "nanovdb_editor_scene_params_invalid_type_test.json";

    {
        std::ofstream out(json_path);
        ASSERT_TRUE(out.good());
        out << R"json(
{
  "SceneParams": {
    "bad_field": {
      "type": "vec3",
      "value": [1.0, 2.0, 3.0]
    }
  }
}
)json";
    }

    pnanovdb_editor::CustomSceneParams params;
    std::string error_message;
    EXPECT_FALSE(params.loadFromJsonFile(json_path.string(), &error_message));
    EXPECT_NE(error_message.find("unsupported type 'vec3'"), std::string::npos);

    std::filesystem::remove(json_path);
}

TEST(NanoVDBEditor, CustomSceneParamsLoadsStringField)
{
    const std::string json = R"json(
{
  "SceneParams": {
    "scale": {
      "type": "float",
      "value": 0.5,
      "min": 0.0,
      "max": 1.0,
      "useSlider": true
    },
    "prompt": {
      "type": "string",
      "length": 32,
      "value": "a red chair"
    },
    "default_prompt": {
      "type": "string"
    }
  }
}
)json";

    pnanovdb_editor::CustomSceneParams params;
    std::string error_message;
    ASSERT_TRUE(params.loadFromJsonString(json, "stringTest", &error_message)) << error_message;

    pnanovdb_shader_params_desc_t desc = {};
    ASSERT_TRUE(params.fillDesc("editor/test.slang", &desc));
    ASSERT_EQ(desc.element_count, 3u);

    EXPECT_STREQ(desc.element_names[0], "scale");
    EXPECT_STREQ(desc.element_names[1], "prompt");
    EXPECT_STREQ(desc.element_names[2], "default_prompt");
    EXPECT_STREQ(desc.element_typenames[1], "char[32]");
    EXPECT_STREQ(desc.element_typenames[2], "char[256]");

    // scale is float at offset 0, prompt (char[32]) starts right after at offset 4
    ASSERT_EQ(desc.element_offsets[0], 0u);
    ASSERT_EQ(desc.element_offsets[1], 4u);
    ASSERT_EQ(desc.element_offsets[2], 4u + 32u);

    const auto* raw = static_cast<const char*>(desc.data);
    const char* prompt = raw + desc.element_offsets[1];
    EXPECT_STREQ(prompt, "a red chair");
    EXPECT_EQ(std::strlen(prompt), std::strlen("a red chair"));

    // default_prompt has no initial value so the buffer is zero-initialised; first byte is
    // the null terminator and reading it as a C string yields an empty string.
    const char* default_prompt = raw + desc.element_offsets[2];
    EXPECT_EQ(default_prompt[0], '\0');

    // Verify the reflected data type exposes char[32] via the cached child reflect datas.
    const pnanovdb_reflect_data_type_t* dt = params.dataType();
    ASSERT_NE(dt, nullptr);
    ASSERT_EQ(dt->child_reflect_data_count, 3u);
    const pnanovdb_reflect_data_t& prompt_child = dt->child_reflect_datas[1];
    EXPECT_STREQ(prompt_child.name, "prompt");
    ASSERT_NE(prompt_child.data_type, nullptr);
    EXPECT_EQ(prompt_child.data_type->data_type, PNANOVDB_REFLECT_TYPE_CHAR);
    EXPECT_EQ(prompt_child.data_type->element_size, 32u);
}

TEST(NanoVDBEditor, CustomSceneParamsTruncatesStringValueToCapacity)
{
    const std::string json = R"json(
{
  "SceneParams": {
    "prompt": {
      "type": "string",
      "length": 8,
      "value": "abcdefghijkl"
    }
  }
}
)json";

    pnanovdb_editor::CustomSceneParams params;
    std::string error_message;
    ASSERT_TRUE(params.loadFromJsonString(json, "truncateTest", &error_message)) << error_message;

    pnanovdb_shader_params_desc_t desc = {};
    ASSERT_TRUE(params.fillDesc("shader", &desc));
    const auto* raw = static_cast<const char*>(desc.data);
    const char* prompt = raw + desc.element_offsets[0];
    EXPECT_EQ(std::strlen(prompt), 7u); // capacity - 1 for null terminator
    EXPECT_STREQ(prompt, "abcdefg");
}

TEST(NanoVDBEditor, CustomSceneParamsLoadsMultipleStringFields)
{
    const std::string json = R"json(
{
  "SceneParams": {
    "prompt": {
      "type": "string",
      "length": 16,
      "value": "hello"
    },
    "passive_prompt": {
      "type": "string",
      "length": 16,
      "value": "world"
    }
  }
}
)json";

    pnanovdb_editor::CustomSceneParams params;
    std::string error_message;
    ASSERT_TRUE(params.loadFromJsonString(json, "stringTest", &error_message)) << error_message;

    pnanovdb_shader_params_desc_t desc = {};
    ASSERT_TRUE(params.fillDesc("shader", &desc));
    ASSERT_EQ(desc.element_count, 2u);

    EXPECT_STREQ(desc.element_typenames[0], "char[16]");
    EXPECT_STREQ(desc.element_typenames[1], "char[16]");
    EXPECT_EQ(desc.element_offsets[0], 0u);
    EXPECT_EQ(desc.element_offsets[1], 16u);

    const auto* raw = static_cast<const char*>(desc.data);
    EXPECT_STREQ(raw + desc.element_offsets[0], "hello");
    EXPECT_STREQ(raw + desc.element_offsets[1], "world");
}

TEST(NanoVDBEditor, CustomSceneParamsRejectsStringWithNumericOptions)
{
    const std::string json = R"json(
{
  "SceneParams": {
    "prompt": {
      "type": "string",
      "min": "a",
      "max": "z"
    }
  }
}
)json";

    pnanovdb_editor::CustomSceneParams params;
    std::string error_message;
    EXPECT_FALSE(params.loadFromJsonString(json, "rejectTest", &error_message));
    EXPECT_NE(error_message.find("not supported"), std::string::npos);
}
