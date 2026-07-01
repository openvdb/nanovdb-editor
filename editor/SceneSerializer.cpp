// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   editor/SceneSerializer.cpp

    \author Petra Hapalova

    \brief
*/

#include "SceneSerializer.h"
#include "EditorSceneManager.h"
#include "EditorToken.h"
#include "ParamWidget.h"
#include "PipelineParams.h"
#include "PipelineRegistry.h"
#include "SceneView.h"
#include "ShaderParams.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <set>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#    if !defined(NOMINMAX)
#        define NOMINMAX
#    endif
#    if !defined(WIN32_LEAN_AND_MEAN)
#        define WIN32_LEAN_AND_MEAN
#    endif
#    include <windows.h>
#endif

namespace pnanovdb_editor
{
bool shader_scalar_serialization_supported(ImGuiDataType type, size_t element_size)
{
    if (type == ImGuiDataType_Float)
    {
        return element_size == sizeof(uint16_t) || element_size == sizeof(float) || element_size == sizeof(double);
    }
    if (type == ImGuiDataType_Double)
    {
        return element_size == sizeof(double);
    }
    if (type == ImGuiDataType_Bool || type == ImGuiDataType_S32 || type == ImGuiDataType_U32)
    {
        return element_size == sizeof(uint32_t);
    }
    if (type == ImGuiDataType_S64 || type == ImGuiDataType_U64)
    {
        return element_size == sizeof(uint64_t);
    }
    return false;
}

namespace
{
const char* token_str(pnanovdb_editor_token_t* token)
{
    return (token && token->str) ? token->str : "";
}

const char* scene_object_type_name(SceneObjectType type)
{
    switch (type)
    {
    case SceneObjectType::NanoVDB:
        return "nanovdb";
    case SceneObjectType::GaussianData:
        return "gaussian";
    case SceneObjectType::Array:
        return "array";
    case SceneObjectType::Camera:
        return "camera";
    case SceneObjectType::Uninitialized:
    default:
        return "uninitialized";
    }
}

const pnanovdb_reflect_data_type_t* stage_params_data_type(const PipelineStage& stage)
{
    if (stage.params.type)
    {
        return stage.params.type;
    }
    const pnanovdb_pipeline_descriptor_t* desc = pnanovdb_pipeline_get_descriptor(stage.type);
    return desc ? desc->params_data_type : nullptr;
}

const char* stage_params_hints(const PipelineStage& stage)
{
    const pnanovdb_pipeline_descriptor_t* desc = pnanovdb_pipeline_get_descriptor(stage.type);
    return desc ? desc->params_hints : nullptr;
}

nlohmann::ordered_json stage_to_json(const PipelineStage& stage)
{
    nlohmann::ordered_json j;
    j["type_id"] = pipeline_type_id(stage.type);
    const pnanovdb_reflect_data_type_t* dt = stage_params_data_type(stage);
    if (dt && stage.params.data && stage.params.size > 0)
    {
        nlohmann::ordered_json params =
            reflect_params_to_json(dt, stage.params.data, (size_t)stage.params.size, stage_params_hints(stage));
        if (!params.empty())
        {
            j["params"] = std::move(params);
        }
    }
    return j;
}

nlohmann::ordered_json object_to_json(const SceneObject& obj, ShaderParams& shader_params)
{
    nlohmann::ordered_json j;
    j["scene"] = token_str(obj.scene_token);
    j["name"] = token_str(obj.name_token);
    j["type"] = scene_object_type_name(obj.type);
    j["visible"] = obj.visible;
    j["source_filepath"] = obj.resources.source_filepath;
    j["shader_name"] = token_str(obj.shader_name());

    nlohmann::ordered_json pipeline;
    pipeline["load"] = stage_to_json(obj.pipeline.load());

    nlohmann::ordered_json process = nlohmann::ordered_json::array();
    for (size_t i = 0; i < obj.pipeline.process_count(); ++i)
    {
        process.push_back(stage_to_json(obj.pipeline.process_step(i)));
    }
    pipeline["process"] = std::move(process);

    pipeline["render"] = stage_to_json(obj.pipeline.render());
    pipeline["drop_intermediate"] = obj.pipeline.drop_intermediate;
    j["pipeline"] = std::move(pipeline);

    const std::string shader_name = token_str(obj.shader_name());
    if (!shader_name.empty())
    {
        pnanovdb_compute_array_t* sp = obj.params.shader_params_array;
        if (sp && sp->data && sp->element_size > 0 && sp->element_count > 0)
        {
            const size_t bytes = (size_t)sp->element_size * (size_t)sp->element_count;
            nlohmann::ordered_json params = shader_params_to_json(shader_params, shader_name, sp->data, bytes);
            if (!params.empty())
            {
                j["shader_params"] = std::move(params);
            }
        }
    }

    return j;
}

nlohmann::ordered_json vec3_to_json(const pnanovdb_vec3_t& v)
{
    return nlohmann::ordered_json::array({ v.x, v.y, v.z });
}

nlohmann::ordered_json camera_to_json(pnanovdb_editor_token_t* name_token,
                                      const pnanovdb_camera_view_t& camera,
                                      bool is_viewport)
{
    nlohmann::ordered_json j;
    j["name"] = token_str(name_token);
    j["viewport"] = is_viewport;
    j["visible"] = camera.is_visible != PNANOVDB_FALSE;
    j["axis_length"] = camera.axis_length;
    j["axis_thickness"] = camera.axis_thickness;
    j["frustum_line_width"] = camera.frustum_line_width;
    j["frustum_scale"] = camera.frustum_scale;
    j["frustum_color"] = vec3_to_json(camera.frustum_color);

    nlohmann::ordered_json entries = nlohmann::ordered_json::array();
    if (camera.states && camera.configs)
    {
        for (pnanovdb_uint32_t i = 0; i < camera.num_cameras; ++i)
        {
            const pnanovdb_camera_state_t& state = camera.states[i];
            const pnanovdb_camera_config_t& config = camera.configs[i];
            nlohmann::ordered_json entry;
            entry["state"] = {
                { "position", vec3_to_json(state.position) },
                { "eye_direction", vec3_to_json(state.eye_direction) },
                { "eye_up", vec3_to_json(state.eye_up) },
                { "eye_distance", state.eye_distance_from_position },
                { "orthographic_scale", state.orthographic_scale },
            };
            entry["config"] = {
                { "projection_rh", config.is_projection_rh != PNANOVDB_FALSE },
                { "orthographic", config.is_orthographic != PNANOVDB_FALSE },
                { "reverse_z", config.is_reverse_z != PNANOVDB_FALSE },
                { "near_plane", config.near_plane },
                { "far_plane", config.far_plane },
                { "fov_y", config.fov_angle_y },
                { "orthographic_y", config.orthographic_y },
                { "aspect_ratio", config.aspect_ratio },
                { "pan_rate", config.pan_rate },
                { "tilt_rate", config.tilt_rate },
                { "zoom_rate", config.zoom_rate },
                { "key_translation_rate", config.key_translation_rate },
                { "scroll_zoom_rate", config.scroll_zoom_rate },
            };
            entries.push_back(std::move(entry));
        }
    }
    j["entries"] = std::move(entries);
    return j;
}

bool object_has_restorable_source(const SceneObject& obj)
{
    if (obj.type == SceneObjectType::Camera)
    {
        return true;
    }
    if (obj.type == SceneObjectType::Uninitialized)
    {
        return false;
    }

    const bool has_data = obj.resources.nanovdb_array || obj.resources.converted_nanovdb ||
                          obj.resources.gaussian_data || !obj.resources.named_arrays.empty();
    if (!obj.resources.source_filepath.empty() && !scene_source_path_is_available(obj.resources.source_filepath))
    {
        return false;
    }
    if (!has_data)
    {
        return true; // Explicit empty objects are represented by their pipeline configuration.
    }
    if (obj.resources.source_filepath.empty())
    {
        return false;
    }
    if (!obj.resources.named_arrays.empty())
    {
        const pnanovdb_pipeline_type_t load = obj.pipeline.load().type;
        if (load != pnanovdb_pipeline_type_mesh_load && load != pnanovdb_pipeline_type_gaussian_load)
        {
            return false;
        }
        return obj.resources.named_arrays == obj.resources.file_backed_named_arrays;
    }
    return true;
}

std::filesystem::path unique_scene_temp_path(const std::filesystem::path& destination)
{
    static std::atomic<uint64_t> counter{ 0u };
    const std::filesystem::path directory =
        destination.parent_path().empty() ? std::filesystem::path(".") : destination.parent_path();
    const std::string filename = destination.filename().string();

    for (unsigned int attempt = 0; attempt < 128u; ++attempt)
    {
        const uint64_t serial = counter.fetch_add(1u, std::memory_order_relaxed);
        const uint64_t tick = static_cast<uint64_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count());
        std::ostringstream suffix;
        suffix << filename << ".tmp." << std::hex << tick << '.' << serial;
        const std::filesystem::path candidate = directory / suffix.str();
        std::error_code ec;
        if (!std::filesystem::exists(candidate, ec) && !ec)
        {
            return candidate;
        }
    }
    return {};
}

bool replace_scene_file(const std::filesystem::path& temporary,
                        const std::filesystem::path& destination,
                        std::string* error_message)
{
#if defined(_WIN32)
    if (MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0)
    {
        return true;
    }
    if (error_message)
    {
        *error_message =
            "Failed to replace '" + destination.string() + "' (Windows error " + std::to_string(GetLastError()) + ")";
    }
    return false;
#else
    std::error_code ec;
    std::filesystem::rename(temporary, destination, ec);
    if (!ec)
    {
        return true;
    }
    if (error_message)
    {
        *error_message = "Failed to replace '" + destination.string() + "': " + ec.message();
    }
    return false;
#endif
}

nlohmann::ordered_json read_shader_scalar(ImGuiDataType type, size_t elem_size, const unsigned char* p)
{
    switch (type)
    {
    case ImGuiDataType_Bool:
    {
        uint32_t v = 0u;
        std::memcpy(&v, p, sizeof(v));
        return (v != 0u);
    }
    case ImGuiDataType_Float:
    {
        if (elem_size == sizeof(uint16_t))
        {
            uint16_t bits;
            std::memcpy(&bits, p, sizeof(bits));
            return bits;
        }
        if (elem_size == sizeof(double))
        {
            double v;
            std::memcpy(&v, p, sizeof(v));
            return v;
        }
        float v;
        std::memcpy(&v, p, sizeof(v));
        return v;
    }
    case ImGuiDataType_Double:
    {
        double v;
        std::memcpy(&v, p, sizeof(v));
        return v;
    }
    case ImGuiDataType_S32:
    {
        int32_t v;
        std::memcpy(&v, p, sizeof(v));
        return v;
    }
    case ImGuiDataType_U32:
    {
        uint32_t v;
        std::memcpy(&v, p, sizeof(v));
        return v;
    }
    case ImGuiDataType_S64:
    {
        int64_t v;
        std::memcpy(&v, p, sizeof(v));
        return v;
    }
    case ImGuiDataType_U64:
    {
        uint64_t v;
        std::memcpy(&v, p, sizeof(v));
        return v;
    }
    default:
    {
        float v;
        std::memcpy(&v, p, sizeof(v));
        return v;
    }
    }
}

bool write_shader_scalar(ImGuiDataType type, size_t elem_size, unsigned char* p, const nlohmann::json& in)
{
    switch (type)
    {
    case ImGuiDataType_Bool:
        return reflect_write_scalar_json(PNANOVDB_REFLECT_TYPE_BOOL32, p, in);
    case ImGuiDataType_Float:
        if (elem_size == sizeof(uint16_t))
            return reflect_write_scalar_json(PNANOVDB_REFLECT_TYPE_UINT16, p, in);
        if (elem_size == sizeof(double))
            return reflect_write_scalar_json(PNANOVDB_REFLECT_TYPE_DOUBLE, p, in);
        return elem_size == sizeof(float) && reflect_write_scalar_json(PNANOVDB_REFLECT_TYPE_FLOAT, p, in);
    case ImGuiDataType_Double:
        return reflect_write_scalar_json(PNANOVDB_REFLECT_TYPE_DOUBLE, p, in);
    case ImGuiDataType_S32:
        return reflect_write_scalar_json(PNANOVDB_REFLECT_TYPE_INT32, p, in);
    case ImGuiDataType_U32:
        return reflect_write_scalar_json(PNANOVDB_REFLECT_TYPE_UINT32, p, in);
    case ImGuiDataType_S64:
        return reflect_write_scalar_json(PNANOVDB_REFLECT_TYPE_INT64, p, in);
    case ImGuiDataType_U64:
        return reflect_write_scalar_json(PNANOVDB_REFLECT_TYPE_UINT64, p, in);
    default:
        return false;
    }
}

} // namespace

bool scene_source_path_is_available(const std::string& path) noexcept
{
    if (path.empty())
    {
        return false;
    }
    try
    {
        std::error_code ec;
        const bool regular_file = std::filesystem::is_regular_file(std::filesystem::path(path), ec);
        return regular_file && !ec;
    }
    catch (const std::exception&)
    {
        return false;
    }
}

bool validate_scene_file_for_load(const std::string& path, std::string* error_message)
{
    const auto fail = [error_message](const std::string& message)
    {
        if (error_message)
            *error_message = message;
        return false;
    };

    std::ifstream file(path);
    if (!file)
        return fail("cannot open file");

    nlohmann::json doc;
    try
    {
        file >> doc;
    }
    catch (const nlohmann::json::exception& e)
    {
        return fail(std::string("invalid JSON: ") + e.what());
    }

    int serialized_version = k_scene_file_version;
    const auto version = doc.find("version");
    if (version != doc.end() && version->is_number_integer())
    {
        try
        {
            serialized_version = version->get<int>();
        }
        catch (const nlohmann::json::exception&)
        {
            serialized_version = k_scene_file_version;
        }
    }
    if (serialized_version != k_scene_file_version)
    {
        return fail("unsupported file version " + std::to_string(serialized_version) + " (expected " +
                    std::to_string(k_scene_file_version) + ")");
    }

    const auto objects = doc.find("objects");
    if (objects == doc.end() || !objects->is_array())
        return fail("file has no 'objects' array");

    bool has_valid_scene = false;
    const auto scenes = doc.find("scenes");
    if (scenes != doc.end() && scenes->is_array())
    {
        for (const auto& scene : *scenes)
        {
            if (!scene.is_object())
                continue;
            const auto name = scene.find("name");
            if (name != scene.end() && name->is_string() && !name->get_ref<const std::string&>().empty())
                has_valid_scene = true;
        }
    }

    if (objects->empty())
        return has_valid_scene ? true : fail("file contains no loadable scene or object records");

    for (const auto& object : *objects)
    {
        std::string scene_name;
        std::string object_name;
        std::string object_type;
        std::string source_filepath;
        if (parse_scene_object_record(object, scene_name, object_name, object_type, source_filepath, nullptr))
        {
            return true;
        }
    }

    return fail("file contains no loadable scene or object records");
}

bool parse_scene_object_record(const nlohmann::json& object,
                               std::string& scene_name,
                               std::string& object_name,
                               std::string& object_type,
                               std::string& source_filepath,
                               std::string* error_message)
{
    const auto fail = [error_message](const std::string& message)
    {
        if (error_message)
            *error_message = message;
        return false;
    };
    if (!object.is_object())
        return fail("record is not an object");

    const auto scene = object.find("scene");
    if (scene == object.end() || !scene->is_string() || scene->get_ref<const std::string&>().empty())
        return fail("record has no valid scene name");
    scene_name = scene->get<std::string>();

    const auto type = object.find("type");
    if (type == object.end() || !type->is_string())
        return fail("record has no valid object type");
    object_type = type->get<std::string>();
    if (object_type != "array" && object_type != "nanovdb" && object_type != "gaussian")
        return fail("unsupported object type '" + object_type + "'");

    const auto source = object.find("source_filepath");
    if (source != object.end() && !source->is_string())
        return fail("source_filepath is not a string");
    source_filepath = source == object.end() ? std::string() : source->get<std::string>();
    if (!source_filepath.empty() && !scene_source_path_is_available(source_filepath))
        return fail("source file '" + source_filepath + "' is unavailable");

    const auto name = object.find("name");
    if (name != object.end() && !name->is_string())
        return fail("object name is not a string");
    object_name = name == object.end() ? std::string() : name->get<std::string>();
    if (object_name.empty() && !source_filepath.empty())
        object_name = std::filesystem::path(source_filepath).stem().string();
    if (object_name.empty())
        return fail("record has no valid object name");
    return true;
}

bool restore_empty_scene_object(EditorSceneManager& scene_manager,
                                SceneView& scene_view,
                                pnanovdb_editor_token_t* scene_token,
                                pnanovdb_editor_token_t* name_token,
                                const std::string& object_type)
{
    if (!scene_token || !name_token)
    {
        return false;
    }

    SceneObjectType type;
    if (object_type == "array")
        type = SceneObjectType::Array;
    else if (object_type == "nanovdb")
        type = SceneObjectType::NanoVDB;
    else if (object_type == "gaussian")
        type = SceneObjectType::GaussianData;
    else
        return false;

    scene_manager.with_object_or_create(scene_token, name_token,
                                        [type](SceneObject* obj)
                                        {
                                            if (!obj)
                                                return;
                                            obj->type = type;
                                            obj->load_pipeline() = pnanovdb_pipeline_type_noop;
                                            obj->process_pipeline() = pnanovdb_pipeline_type_noop;
                                            obj->render_pipeline() = pnanovdb_pipeline_type_noop;
                                            obj->pipeline.load().configured = true;
                                            obj->pipeline.process().configured = true;
                                            obj->pipeline.render().configured = true;
                                        });

    if (type == SceneObjectType::GaussianData)
        scene_view.add_gaussian(scene_token, name_token, GaussianDataContext{});
    else
        scene_view.add_nanovdb(scene_token, name_token, NanoVDBContext{});
    return true;
}

namespace
{
struct VectorParamGroup
{
    std::string key;
    std::vector<std::string> components;
};

std::vector<VectorParamGroup> load_vector_param_groups(const char* hints_name)
{
    std::vector<VectorParamGroup> groups;
    if (!hints_name || !hints_name[0])
    {
        return groups;
    }
    const std::optional<nlohmann::ordered_json> section = loadParamHintsJson(hints_name, PIPELINE_PARAM_JSON);
    if (!section || !section->is_object())
    {
        return groups;
    }
    for (const auto& [name, value] : section->items())
    {
        if (!value.is_object() || !value.contains("components") || !value["components"].is_array())
        {
            continue;
        }
        VectorParamGroup group;
        group.key = name;
        for (const auto& c : value["components"])
        {
            if (c.is_string())
            {
                group.components.push_back(c.get<std::string>());
            }
        }
        if (group.components.size() >= 2)
        {
            groups.push_back(std::move(group));
        }
    }
    return groups;
}
} // namespace

nlohmann::ordered_json reflect_params_to_json(const pnanovdb_reflect_data_type_t* data_type,
                                              const void* data,
                                              size_t size,
                                              const char* hints_name)
{
    nlohmann::ordered_json j = nlohmann::ordered_json::object();
    if (!data_type || !data)
    {
        return j;
    }
    const unsigned char* base = static_cast<const unsigned char*>(data);
    for (pnanovdb_uint64_t i = 0; i < data_type->child_reflect_data_count; ++i)
    {
        const pnanovdb_reflect_data_t& f = data_type->child_reflect_datas[i];
        if (!f.data_type || f.reflect_mode != PNANOVDB_REFLECT_MODE_VALUE || !f.name)
        {
            continue;
        }
        const pnanovdb_reflect_type_t rt = f.data_type->data_type;
        const size_t fsize = reflect_scalar_field_size(rt);
        if (fsize == 0 || (size_t)f.data_offset + fsize > size)
        {
            continue;
        }
        nlohmann::ordered_json value;
        if (reflect_read_scalar_json(rt, base + f.data_offset, value))
        {
            j[f.name] = std::move(value);
        }
    }

    for (const VectorParamGroup& group : load_vector_param_groups(hints_name))
    {
        bool all_present = true;
        for (const std::string& comp : group.components)
        {
            const auto it = j.find(comp);
            if (it == j.end() || !it->is_number())
            {
                all_present = false;
                break;
            }
        }
        if (!all_present)
        {
            continue;
        }
        nlohmann::ordered_json arr = nlohmann::ordered_json::array();
        for (const std::string& comp : group.components)
        {
            arr.push_back(j[comp]);
        }
        nlohmann::ordered_json grouped = nlohmann::ordered_json::object();
        for (auto it = j.begin(); it != j.end(); ++it)
        {
            if (it.key() == group.components.front())
            {
                grouped[group.key] = arr;
            }
            else if (std::find(group.components.begin(), group.components.end(), it.key()) != group.components.end())
            {
                continue;
            }
            else
            {
                grouped[it.key()] = it.value();
            }
        }
        j = std::move(grouped);
    }
    return j;
}

void json_to_reflect_params(const nlohmann::json& j,
                            const pnanovdb_reflect_data_type_t* data_type,
                            void* data,
                            size_t size,
                            const char* hints_name)
{
    if (!data_type || !data || !j.is_object())
    {
        return;
    }
    unsigned char* base = static_cast<unsigned char*>(data);

    std::map<std::string, const nlohmann::json*> component_values;
    for (const VectorParamGroup& group : load_vector_param_groups(hints_name))
    {
        const auto it = j.find(group.key);
        if (it == j.end() || !it->is_array() || it->size() < group.components.size())
        {
            continue;
        }
        for (size_t c = 0; c < group.components.size(); ++c)
        {
            if ((*it)[c].is_number())
            {
                component_values[group.components[c]] = &(*it)[c];
            }
        }
    }

    for (pnanovdb_uint64_t i = 0; i < data_type->child_reflect_data_count; ++i)
    {
        const pnanovdb_reflect_data_t& f = data_type->child_reflect_datas[i];
        if (!f.data_type || f.reflect_mode != PNANOVDB_REFLECT_MODE_VALUE || !f.name)
        {
            continue;
        }
        auto it = j.find(f.name);
        const nlohmann::json* value = (it != j.end()) ? &(*it) : nullptr;
        if (!value)
        {
            const auto comp_it = component_values.find(f.name);
            if (comp_it != component_values.end())
            {
                value = comp_it->second;
            }
        }
        if (!value)
        {
            continue;
        }
        const pnanovdb_reflect_type_t rt = f.data_type->data_type;
        const size_t fsize = reflect_scalar_field_size(rt);
        if (fsize == 0 || (size_t)f.data_offset + fsize > size)
        {
            continue;
        }
        try
        {
            reflect_write_scalar_json(rt, base + f.data_offset, *value);
        }
        catch (const nlohmann::json::exception&)
        {
            // A malformed field must not invalidate the rest of the stage.
        }
    }
}

std::string pipeline_type_name(pnanovdb_pipeline_type_t type)
{
    const pnanovdb_pipeline_descriptor_t* desc = pnanovdb_pipeline_get_descriptor(type);
    return (desc && desc->ui_name) ? std::string(desc->ui_name) : std::string();
}

std::string pipeline_type_id(pnanovdb_pipeline_type_t type)
{
    const pnanovdb_pipeline_descriptor_t* desc = pnanovdb_pipeline_get_descriptor(type);
    return (desc && desc->type_id) ? std::string(desc->type_id) : std::string();
}

pnanovdb_pipeline_type_t pipeline_type_from_id(const std::string& type_id)
{
    if (type_id.empty())
    {
        return pnanovdb_pipeline_type_noop;
    }
    for (pnanovdb_uint32_t t = 0; t < pnanovdb_pipeline_type_count; ++t)
    {
        const pnanovdb_pipeline_descriptor_t* desc = pnanovdb_pipeline_get_descriptor(t);
        if (desc && desc->type_id && type_id == desc->type_id)
        {
            return (pnanovdb_pipeline_type_t)t;
        }
    }
    return pnanovdb_pipeline_type_noop;
}

bool pipeline_type_from_json(const nlohmann::json& stage, pnanovdb_pipeline_type_t& type, std::string* error_message)
{
    const auto fail = [error_message](const std::string& message)
    {
        if (error_message)
        {
            *error_message = message;
        }
        return false;
    };

    if (!stage.is_object())
    {
        return fail("stage is not an object");
    }

    const auto id_it = stage.find("type_id");
    if (id_it == stage.end() || !id_it->is_string())
    {
        return fail("stage has no 'type_id'");
    }
    const std::string type_id = id_it->get<std::string>();
    for (pnanovdb_uint32_t t = 0; t < pnanovdb_pipeline_type_count; ++t)
    {
        const pnanovdb_pipeline_descriptor_t* desc = pnanovdb_pipeline_get_descriptor(t);
        if (desc && desc->type_id && type_id == desc->type_id)
        {
            type = static_cast<pnanovdb_pipeline_type_t>(t);
            return true;
        }
    }
    return fail("unknown type_id '" + type_id + "'");
}

nlohmann::ordered_json shader_params_to_json(ShaderParams& shader_params,
                                             const std::string& shader_name,
                                             const void* data,
                                             size_t size)
{
    nlohmann::ordered_json j = nlohmann::ordered_json::object();
    if (!data || shader_name.empty())
    {
        return j;
    }
    if (!shader_params.load(shader_name, false))
    {
        return j;
    }
    const std::vector<ShaderParam>* params = shader_params.get(shader_name);
    if (!params)
    {
        return j;
    }
    const unsigned char* base = static_cast<const unsigned char*>(data);
    size_t offset = 0;
    for (const ShaderParam& p : *params)
    {
        if (p.size == 0 || p.num_elements == 0)
        {
            continue;
        }
        const size_t field_bytes = p.size * p.num_elements;
        if (offset + field_bytes > size)
        {
            break;
        }
        if (!shader_scalar_serialization_supported(p.type, p.size))
        {
            offset += field_bytes;
            continue;
        }
        if (p.num_elements == 1)
        {
            j[p.name] = read_shader_scalar(p.type, p.size, base + offset);
        }
        else
        {
            nlohmann::ordered_json arr = nlohmann::ordered_json::array();
            for (size_t e = 0; e < p.num_elements; ++e)
            {
                arr.push_back(read_shader_scalar(p.type, p.size, base + offset + e * p.size));
            }
            j[p.name] = std::move(arr);
        }
        offset += field_bytes;
    }
    return j;
}

bool json_to_shader_params(ShaderParams& shader_params,
                           const std::string& shader_name,
                           const nlohmann::json& j,
                           std::vector<unsigned char>& out_bytes)
{
    if (shader_name.empty() || !j.is_object())
    {
        return false;
    }
    if (!shader_params.load(shader_name, false))
    {
        return false;
    }
    const std::vector<ShaderParam>* params = shader_params.get(shader_name);
    if (!params)
    {
        return false;
    }

    size_t total = 0;
    for (const ShaderParam& p : *params)
    {
        total += p.size * p.num_elements;
    }
    if (total == 0)
    {
        return false;
    }

    const size_t buffer_size = (total > (size_t)PNANOVDB_COMPUTE_CONSTANT_BUFFER_MAX_SIZE) ?
                                   total :
                                   (size_t)PNANOVDB_COMPUTE_CONSTANT_BUFFER_MAX_SIZE;
    out_bytes.assign(buffer_size, 0u);
    shader_params.copy_default_params_to_buffer(shader_name, out_bytes.data(), out_bytes.size());

    return apply_shader_params_json(*params, j, out_bytes);
}

bool apply_shader_params_json(const std::vector<ShaderParam>& params,
                              const nlohmann::json& j,
                              std::vector<unsigned char>& bytes)
{
    if (!j.is_object())
    {
        return false;
    }
    size_t offset = 0;
    for (const ShaderParam& p : params)
    {
        if (p.size == 0 || p.num_elements == 0)
        {
            continue;
        }
        if (offset > bytes.size() || p.num_elements > (bytes.size() - offset) / p.size)
        {
            return false;
        }
        const size_t field_bytes = p.size * p.num_elements;
        if (!shader_scalar_serialization_supported(p.type, p.size))
        {
            offset += field_bytes;
            continue;
        }
        auto it = j.find(p.name);
        if (it != j.end())
        {
            if (p.num_elements == 1)
            {
                write_shader_scalar(p.type, p.size, bytes.data() + offset, *it);
            }
            else if (it->is_array())
            {
                const size_t count = std::min(p.num_elements, it->size());
                for (size_t e = 0; e < count; ++e)
                {
                    write_shader_scalar(p.type, p.size, bytes.data() + offset + e * p.size, (*it)[e]);
                }
            }
        }
        offset += field_bytes;
    }
    return true;
}

nlohmann::ordered_json serialize_scenes(EditorSceneManager& scene_manager, const SceneView& scene_view)
{
    nlohmann::ordered_json doc;
    doc["version"] = k_scene_file_version;

    nlohmann::ordered_json objects = nlohmann::ordered_json::array();
    ShaderParams& shader_params = scene_manager.shader_params;
    scene_manager.for_each_object(
        [&objects, &shader_params](SceneObject* obj)
        {
            if (obj && obj->type != SceneObjectType::Uninitialized && obj->type != SceneObjectType::Camera)
            {
                objects.push_back(object_to_json(*obj, shader_params));
            }
            return true;
        });
    doc["objects"] = std::move(objects);

    nlohmann::ordered_json scenes = nlohmann::ordered_json::array();
    for (pnanovdb_editor_token_t* scene_token : scene_view.get_all_scene_tokens())
    {
        if (!scene_token)
        {
            continue;
        }
        nlohmann::ordered_json scene;
        scene["name"] = token_str(scene_token);
        nlohmann::ordered_json cameras = nlohmann::ordered_json::array();
        const SceneViewData* scene_data = scene_view.get_scene_data(scene_token);
        if (scene_data)
        {
            for (const auto& [camera_id, context] : scene_data->cameras)
            {
                if (!context.camera_view)
                {
                    continue;
                }
                pnanovdb_editor_token_t* camera_token = EditorToken::getInstance().getTokenById(camera_id);
                cameras.push_back(camera_to_json(
                    camera_token, *context.camera_view, scene_data->viewport_camera_token_id == camera_id));
            }
        }
        scene["cameras"] = std::move(cameras);
        scenes.push_back(std::move(scene));
    }
    doc["scenes"] = std::move(scenes);
    return doc;
}

bool save_scenes_to_file(EditorSceneManager& scene_manager,
                         const SceneView& scene_view,
                         const std::string& path,
                         std::string* error_message)
{
    if (scene_manager.has_file_object_replacement_in_progress())
    {
        if (error_message)
        {
            *error_message = "Cannot save while a file-backed object replacement is still in progress";
        }
        return false;
    }

    std::string unsupported;
    std::set<std::pair<uint64_t, uint64_t>> camera_names;
    for (pnanovdb_editor_token_t* scene_token : scene_view.get_all_scene_tokens())
    {
        const SceneViewData* scene_data = scene_view.get_scene_data(scene_token);
        if (!scene_token || !scene_data)
        {
            continue;
        }
        for (const auto& [camera_id, context] : scene_data->cameras)
        {
            if (context.camera_view)
            {
                camera_names.emplace(scene_token->id, camera_id);
            }
        }
    }
    scene_manager.for_each_object(
        [&unsupported, &camera_names](SceneObject* obj)
        {
            if (!obj || !unsupported.empty())
            {
                return unsupported.empty();
            }
            if (obj->type == SceneObjectType::Uninitialized)
            {
                unsupported = "Cannot save configured placeholder '" + std::string(token_str(obj->name_token)) +
                              "' in scene '" + std::string(token_str(obj->scene_token)) +
                              "': its object type is not defined";
            }
            else if (obj->type != SceneObjectType::Camera && obj->scene_token && obj->name_token &&
                     camera_names.count({ obj->scene_token->id, obj->name_token->id }) != 0)
            {
                unsupported = "Cannot save object '" + std::string(token_str(obj->name_token)) + "' in scene '" +
                              std::string(token_str(obj->scene_token)) + "': a camera already uses the same name";
            }
            else if (!object_has_restorable_source(*obj))
            {
                unsupported = "Cannot save object '" + std::string(token_str(obj->name_token)) + "' in scene '" +
                              std::string(token_str(obj->scene_token)) +
                              "': its source data has no available file-backed representation; export it first";
            }
            return unsupported.empty();
        });
    if (!unsupported.empty())
    {
        if (error_message)
        {
            *error_message = unsupported;
        }
        return false;
    }

    nlohmann::ordered_json doc = serialize_scenes(scene_manager, scene_view);
    const std::filesystem::path destination(path);
#if !defined(_WIN32)
    bool preserve_destination_permissions = false;
    std::filesystem::perms destination_permissions = std::filesystem::perms::unknown;
    std::error_code status_ec;
    const std::filesystem::file_status destination_status = std::filesystem::status(destination, status_ec);
    if (!status_ec && std::filesystem::exists(destination_status) &&
        destination_status.permissions() != std::filesystem::perms::unknown)
    {
        preserve_destination_permissions = true;
        destination_permissions = destination_status.permissions();
    }
#endif
    const std::filesystem::path temporary = unique_scene_temp_path(destination);
    if (temporary.empty())
    {
        if (error_message)
        {
            *error_message = "Failed to create a unique temporary file for '" + path + "'";
        }
        return false;
    }

    std::ofstream file(temporary, std::ios::out | std::ios::trunc);
    if (!file)
    {
        if (error_message)
        {
            *error_message = "Failed to open '" + path + "' for writing";
        }
        return false;
    }
    try
    {
        file << doc.dump(4) << '\n';
    }
    catch (const nlohmann::json::exception& e)
    {
        file.close();
        std::error_code cleanup_ec;
        std::filesystem::remove(temporary, cleanup_ec);
        if (error_message)
        {
            *error_message = "Failed to serialize scene for '" + path + "': " + e.what();
        }
        return false;
    }
    file.flush();
    if (!file)
    {
        file.close();
        std::error_code cleanup_ec;
        std::filesystem::remove(temporary, cleanup_ec);
        if (error_message)
        {
            *error_message = "Failed to write to '" + path + "'";
        }
        return false;
    }
    file.close();
    if (file.fail())
    {
        std::error_code cleanup_ec;
        std::filesystem::remove(temporary, cleanup_ec);
        if (error_message)
        {
            *error_message = "Failed to close '" + path + "' after writing";
        }
        return false;
    }

#if !defined(_WIN32)
    if (preserve_destination_permissions)
    {
        std::error_code permissions_ec;
        std::filesystem::permissions(
            temporary, destination_permissions, std::filesystem::perm_options::replace, permissions_ec);
        if (permissions_ec)
        {
            std::error_code cleanup_ec;
            std::filesystem::remove(temporary, cleanup_ec);
            if (error_message)
            {
                *error_message = "Failed to preserve permissions for '" + path + "': " + permissions_ec.message();
            }
            return false;
        }
    }
#endif

    if (!replace_scene_file(temporary, destination, error_message))
    {
        std::error_code cleanup_ec;
        std::filesystem::remove(temporary, cleanup_ec);
        return false;
    }
    return true;
}

} // namespace pnanovdb_editor
