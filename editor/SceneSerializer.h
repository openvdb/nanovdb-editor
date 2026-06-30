// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   editor/SceneSerializer.h

    \author Petra Hapalova

    \brief
*/

#pragma once

#include "PipelineTypes.h"
#include "nanovdb_editor/putil/Reflect.h"

#include <nlohmann/json.hpp>
#include <imgui.h>

#include <string>
#include <vector>

namespace pnanovdb_editor
{
#if defined(_WIN32)
#    define PNANOVDB_SCENE_SERIALIZER_EXPORT_CXX __declspec(dllexport)
#else
#    define PNANOVDB_SCENE_SERIALIZER_EXPORT_CXX __attribute__((visibility("default")))
#endif
class EditorSceneManager;
class SceneView;
class ShaderParams;
struct ShaderParam;

inline constexpr int k_scene_file_version = 1;

bool scene_source_path_is_available(const std::string& path) noexcept;

bool parse_scene_object_record(const nlohmann::json& object,
                               std::string& scene_name,
                               std::string& object_name,
                               std::string& object_type,
                               std::string& source_filepath,
                               std::string* error_message = nullptr);

bool restore_empty_scene_object(EditorSceneManager& scene_manager,
                                SceneView& scene_view,
                                pnanovdb_editor_token_t* scene_token,
                                pnanovdb_editor_token_t* name_token,
                                const std::string& object_type);

nlohmann::ordered_json reflect_params_to_json(const pnanovdb_reflect_data_type_t* data_type, const void* data, size_t size);

void json_to_reflect_params(const nlohmann::json& j, const pnanovdb_reflect_data_type_t* data_type, void* data, size_t size);

std::string pipeline_type_name(pnanovdb_pipeline_type_t type);

pnanovdb_pipeline_type_t pipeline_type_from_name(const std::string& name);

bool pipeline_type_from_json(const nlohmann::json& stage,
                             pnanovdb_pipeline_type_t& type,
                             std::string* error_message = nullptr);

nlohmann::ordered_json shader_params_to_json(ShaderParams& shader_params,
                                             const std::string& shader_name,
                                             const void* data,
                                             size_t size);

bool json_to_shader_params(ShaderParams& shader_params,
                           const std::string& shader_name,
                           const nlohmann::json& j,
                           std::vector<unsigned char>& out_bytes);

bool apply_shader_params_json(const std::vector<ShaderParam>& params,
                              const nlohmann::json& j,
                              std::vector<unsigned char>& bytes);

bool shader_scalar_serialization_supported(ImGuiDataType type, size_t element_size);

PNANOVDB_SCENE_SERIALIZER_EXPORT_CXX nlohmann::ordered_json serialize_scenes(EditorSceneManager& scene_manager,
                                                                             const SceneView& scene_view);

bool save_scenes_to_file(EditorSceneManager& scene_manager,
                         const SceneView& scene_view,
                         const std::string& path,
                         std::string* error_message = nullptr);

} // namespace pnanovdb_editor
