// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/compute/Compute.cpp

    \author Petra Hapalova

    \brief
*/

#include "Compute.h"
#include "ComputeShader.h"

#include <nanovdb/io/IO.h>

#include <mutex>
#include <stdio.h>
#include <unordered_map>
#include <optional>

#include "ShaderParamsTypes.h"

// Adding missing NanoVDB load single handle functionality
namespace nanovdb
{ // ==========================================================

template <typename BufferT>
GridHandle<BufferT> readSegment(std::istream& is, int n, const BufferT& pool)
{
    GridHandle<BufferT> gridHandle;
    int counter = -1;
    GridData data;
    is.read((char*)&data, sizeof(GridData));
    if (data.isValid())
    {
        if (data.mGridIndex == 0u)
        {
            ++counter;
        }
        uint64_t sum = data.mGridSize;
        while (counter < n || data.mGridIndex + 1u < data.mGridCount)
        { // loop over remaining raw grids in stream
            is.seekg(data.mGridSize - sizeof(GridData), std::ios::cur); // skip grid
            is.read((char*)&data, sizeof(GridData));
            if (is.eof())
            {
                throw std::logic_error("Hit end of file before finding Nth segment");
            }
            if (data.mGridIndex == 0u)
            {
                ++counter;
                sum = 0u;
            }
            sum += data.mGridSize;
        }
        auto buffer = BufferT::create(sum, &pool);
        is.seekg(-int64_t(sum - data.mGridSize + sizeof(GridData)), std::ios::cur); // rewind to start
        is.read((char*)(buffer.data()), buffer.size());
        gridHandle = std::move(buffer);
    }
    else
    {
        is.seekg(-sizeof(GridData), std::ios::cur); // rewind
        throw std::logic_error("This stream does not contain a valid raw grid buffer");
    }
    return gridHandle;
} // void GridHandle<BufferT>::read(std::istream& is, const BufferT& pool)

namespace io
{ // ===============================================================

template <typename BufferT>
GridHandle<BufferT> readGridSegment(std::istream& is, int n, const BufferT& pool)
{
    GridHandle<BufferT> handle;
    try
    { // first try to read a raw grid buffer
        handle = readSegment(is, n, pool);
        // Disable updateGridCount vs original readGrid
        // tools::updateGridCount((GridData*)handle.data(), 0u, 1u);
    }
    catch (const std::logic_error&)
    {
        Segment seg;
        int counter = -1;
        while (seg.read(is))
        {
            if (++counter == n)
            {
                uint64_t bufferSize = 0;
                for (auto& m : seg.meta)
                    bufferSize += m.gridSize;
                auto buffer = BufferT::create(bufferSize, &pool);
                uint64_t bufferOffset = 0;
                for (uint16_t i = 0; i < seg.header.gridCount; ++i)
                {
                    auto* data = util::PtrAdd<GridData>(buffer.data(), bufferOffset);
                    Internal::read(is, (char*)data, seg.meta[i].gridSize, seg.header.codec);
                    tools::updateGridCount(data, uint32_t(i), uint32_t(seg.header.gridCount));
                    bufferOffset += seg.meta[i].gridSize;
                } // loop over grids in segment
                handle = GridHandle<BufferT>(std::move(buffer));
                break;
            }
        } // loop over segments
        if (n != counter)
            throw std::runtime_error("stream does not contain a #" + std::to_string(n) + " grid");
    }
    return handle;
} // readGridHandle

/// @brief Read the n'th grid
template <typename BufferT>
GridHandle<BufferT> readGridSegment(const std::string& fileName, int n, int verbose, const BufferT& buffer)
{
    std::ifstream is(fileName, std::ios::in | std::ios::binary);
    if (!is.is_open())
        throw std::ios_base::failure("Unable to open file named \"" + fileName + "\" for input");
    auto handle = readGridSegment<BufferT>(is, n, buffer);
    if (verbose)
    {
        if (n < 0)
        {
            std::cout << "Read all NanoGrids from the file named \"" << fileName << "\"" << std::endl;
        }
        else
        {
            std::cout << "Read NanoGrid # " << n << " from the file named \"" << fileName << "\"" << std::endl;
        }
    }
    return handle; // is converted to r-value and return value is move constructed.
} // readGridHandle

}
}

namespace pnanovdb_compute
{
static void compute_profiler_report(void* userdata,
                                    pnanovdb_uint64_t capture_id,
                                    pnanovdb_uint32_t num_entries,
                                    pnanovdb_compute_profiler_entry_t* entries)
{
    const char* name = (const char*)userdata;
    printf("'%s' profiler results capture_id(%llu):\n", name, (unsigned long long int)capture_id);
    for (pnanovdb_uint32_t idx = 0u; idx < num_entries; idx++)
    {
        printf("[%d] name(%s) cpu_ms(%f) gpu_ms(%f)\n", idx, entries[idx].label, 1000.f * entries[idx].cpu_delta_time,
               1000.f * entries[idx].gpu_delta_time);
    }
}

struct shader_context_t
{
    pnanovdb_compute_shader_t* shader;
    pnanovdb_compute_shader_build_t* shader_build;
    pnanovdb_compute_pipeline_t* pipeline;
    pnanovdb_compute_shader_source_t source;

    pnanovdb_reflect_data_type_t shader_params_data_type = {};
    pnanovdb_reflect_data_type_t shader_params_metadata_type = {};
    std::vector<pnanovdb_reflect_data_t> reflect_datas;
    std::vector<pnanovdb_reflect_data_t> reflect_metadatas;
    std::vector<uint8_t> shader_params_default;
    std::vector<uint8_t> shader_params_metadefault;
    std::vector<std::unique_ptr<std::string>> owned_strings;
};

PNANOVDB_CAST_PAIR(pnanovdb_shader_context_t, shader_context_t)

pnanovdb_shader_context_t* create_shader_context(const char* filename)
{
    shader_context_t* ctx = new shader_context_t();

    ctx->source.source_filename = filename;

    return cast(ctx);
}

void destroy_shader_context(const pnanovdb_compute_t* compute,
                            pnanovdb_compute_queue_t* queue,
                            pnanovdb_shader_context_t* context)
{
    shader_context_t* ctx = cast(context);
    if (ctx)
    {
        compute->destroy_shader(compute->device_interface.get_compute_interface(queue), &compute->shader_interface,
                                compute->device_interface.get_compute_context(queue), context);
        assert(ctx->shader == nullptr);
        delete ctx;
    }
}

pnanovdb_compute_array_t* create_array(size_t element_size, pnanovdb_uint64_t element_count, const void* data);

pnanovdb_compute_array_t* load_nanovdb(const char* filepath)
{
    nanovdb::GridHandle<nanovdb::HostBuffer> gridHandle;
    nanovdb::HostBuffer hostBuffer;
    try
    {
        gridHandle = nanovdb::io::readGridSegment(filepath, 0, 0, hostBuffer);
    }
    catch (const std::ios_base::failure& e)
    {
        printf("Error: Could not open nanovdb '%s'\n", filepath);
        return nullptr;
    }
    catch (...)
    {
        printf("Error: Could not open nanovdb '%s'\n", filepath);
        return nullptr;
    }

    pnanovdb_compute_array_t* array =
        create_array(sizeof(pnanovdb_uint32_t), gridHandle.bufferSize() / sizeof(pnanovdb_uint32_t), gridHandle.data());
    array->filepath = filepath;

    return array;
}

pnanovdb_bool_t save_nanovdb(pnanovdb_compute_array_t* array, const char* filepath)
{
    if (!array || !array->data || array->element_size == 0u || array->element_count == 0u)
    {
        printf("Error: Could not save nanovdb '%s' (invalid array)\n", filepath);
        return PNANOVDB_FALSE;
    }
    nanovdb::HostBuffer buffer(array->element_count * array->element_size);
    memcpy(buffer.data(), array->data, array->element_count * array->element_size);

    nanovdb::GridHandle<nanovdb::HostBuffer> gridHandle(std::move(buffer));
    try
    {
        nanovdb::io::writeGrid(filepath, gridHandle);
    }
    catch (const std::ios_base::failure& e)
    {
        printf("Error: Could not save nanovdb '%s' (%s)\n", filepath, e.what());
        return PNANOVDB_FALSE;
    }
    return PNANOVDB_TRUE;
}

pnanovdb_bool_t init_shader(const pnanovdb_compute_t* compute,
                            pnanovdb_compute_queue_t* queue,
                            pnanovdb_shader_context_t* shaderContext,
                            pnanovdb_compiler_settings_t* compileSettings)
{
    const pnanovdb_compute_shader_interface_t* shader_interface = &compute->shader_interface;
    pnanovdb_compute_interface_t* compute_interface = compute->device_interface.get_compute_interface(queue);
    pnanovdb_compute_context_t* context = compute->device_interface.get_compute_context(queue);

    auto log_print = compute_interface->get_log_print(context);

    shader_context_t* shader_ctx = cast(shaderContext);

    pnanovdb_bool_t shader_updated = PNANOVDB_FALSE;
    if (compute->compiler)
    {
        // shader will be recompiled only if the source has changed
        pnanovdb_bool_t result = compute->compiler->compile_shader_from_file(
            nullptr, shader_ctx->source.source_filename, compileSettings, &shader_updated);
        if (shader_updated && log_print)
        {
            log_print(PNANOVDB_COMPUTE_LOG_LEVEL_DEBUG, "shader '%s' updated", shader_ctx->source.source_filename);
        }
    }
    if (shader_updated == PNANOVDB_TRUE)
    {
        compute->destroy_shader(compute_interface, shader_interface, context, shaderContext);
    }

    shader_ctx->shader = shader_interface->create_shader(&shader_ctx->source);
    shader_ctx->shader_build = nullptr;
    bool result = shader_interface->map_shader_build(shader_ctx->shader, &shader_ctx->shader_build);
    if (!result && log_print)
    {
        log_print(
            PNANOVDB_COMPUTE_LOG_LEVEL_ERROR, "mapping shader build failed, check the shader exists and is compiled");
        return PNANOVDB_FALSE;
    }

    // Can be enabled when debugging shader/resource binding problems
#if 0
    if (log_print)
    {
        log_print(PNANOVDB_COMPUTE_LOG_LEVEL_DEBUG, "shader(%s) descriptor_write_count(%d) byte_code_size(%d)",
                  shader_ctx->shader_build->debug_label, shader_ctx->shader_build->descriptor_write_count,
                  (pnanovdb_uint32_t)shader_ctx->shader_build->pipeline_desc.bytecode.size_in_bytes);
        for (uint32_t idx = 0u; idx < shader_ctx->shader_build->descriptor_write_count; idx++)
        {
            log_print(PNANOVDB_COMPUTE_LOG_LEVEL_DEBUG, "resource[%d] name(%s)", idx,
                      shader_ctx->shader_build->resource_names[idx]);
        }
    }
#endif

    if (shader_ctx->shader_build->pipeline_desc.bytecode.size_in_bytes == 0)
    {
        log_print(PNANOVDB_COMPUTE_LOG_LEVEL_ERROR, "shader bytecode is empty. Compilation may have failed for '%s'",
                  shader_ctx->source.source_filename);
        return PNANOVDB_FALSE;
    }

    shader_ctx->pipeline = compute_interface->create_compute_pipeline(context, &shader_ctx->shader_build->pipeline_desc);
    if (!shader_ctx->pipeline)
    {
        log_print(PNANOVDB_COMPUTE_LOG_LEVEL_ERROR, "failed to create compute pipeline.");
        return PNANOVDB_FALSE;
    }

    return PNANOVDB_TRUE;
}

void destroy_shader(pnanovdb_compute_interface_t* computeInterface,
                    const pnanovdb_compute_shader_interface_t* shaderInterface,
                    pnanovdb_compute_context_t* computeContext,
                    pnanovdb_shader_context_t* shaderContext)
{
    if (!shaderContext)
    {
        return;
    }

    shader_context_t* shader = cast(shaderContext);
    if (shader->pipeline)
    {
        computeInterface->destroy_compute_pipeline(computeContext, shader->pipeline);
        shader->pipeline = nullptr;
    }
    if (shader->shader)
    {
        shaderInterface->destroy_shader(shader->shader);
        shader->shader = nullptr;
    }
}

static nlohmann::ordered_json* getCompiledShaderParamsObject(nlohmann::ordered_json& json)
{
    if (json.contains(pnanovdb_shader::SHADER_PARAM_JSON))
    {
        return &json[pnanovdb_shader::SHADER_PARAM_JSON];
    }
    if (json.contains(pnanovdb_shader::GENERATED_SHADER_PARAM_JSON))
    {
        return &json[pnanovdb_shader::GENERATED_SHADER_PARAM_JSON];
    }
    return nullptr;
}

static const uint64_t json_type_name_count = 10u;
static const char* json_type_names[json_type_name_count] = {
    "", // SLANG_SCALAR_TYPE_NONE
    "void", // SLANG_SCALAR_TYPE_VOID
    "bool", // SLANG_SCALAR_TYPE_BOOL
    "int", // SLANG_SCALAR_TYPE_INT32
    "uint", // SLANG_SCALAR_TYPE_UINT32
    "int64", // SLANG_SCALAR_TYPE_INT64
    "uint64", // SLANG_SCALAR_TYPE_UINT64
    "float16", // SLANG_SCALAR_TYPE_FLOAT16
    "float", // SLANG_SCALAR_TYPE_FLOAT32
    "double" // SLANG_SCALAR_TYPE_FLOAT64
};
static const pnanovdb_reflect_data_type_t* reflect_data_types[json_type_name_count] = {
    nullptr, // SLANG_SCALAR_TYPE_NONE
    PNANOVDB_REFLECT_DATA_TYPE(void), // SLANG_SCALAR_TYPE_VOID
    PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_uint32_t), // SLANG_SCALAR_TYPE_BOOL
    PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_int32_t), // SLANG_SCALAR_TYPE_INT32
    PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_uint32_t), // SLANG_SCALAR_TYPE_UINT32
    PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_int64_t), // SLANG_SCALAR_TYPE_INT64
    PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_uint64_t), // SLANG_SCALAR_TYPE_UINT64
    PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_uint16_t), // SLANG_SCALAR_TYPE_FLOAT16
    PNANOVDB_REFLECT_DATA_TYPE(float), // SLANG_SCALAR_TYPE_FLOAT32
    PNANOVDB_REFLECT_DATA_TYPE(double) // SLANG_SCALAR_TYPE_FLOAT64
};

static std::optional<nlohmann::ordered_json> loadJsonFileFromShaderPath(const std::string& relFilePath)
{
    std::string json_filePath = pnanovdb_shader::getShaderParamsFilePath(relFilePath.c_str());
    std::ifstream json_file(json_filePath);
    if (!json_file)
    {
        return std::nullopt;
    }
    if (!nlohmann::json::accept(json_file))
    {
        return std::nullopt;
    }
    json_file.clear();
    json_file.seekg(0);
    nlohmann::ordered_json json;
    try
    {
        json_file >> json;
    }
    catch (const nlohmann::json::parse_error& e)
    {
        printf("Error parsing file '%s': %s\n", json_filePath.c_str(), e.what());
        return std::nullopt;
    }
    return json;
}

static void copyReflectParameter(uint8_t* mapped_data, const pnanovdb_reflect_data_t* reflect_data, nlohmann::basic_json<nlohmann::ordered_map> val)
{
    printf("copyReflectParameter(%s) offset(%zu)\n", reflect_data->name, reflect_data->data_offset);
    if (!val.is_number())
    {
        return;
    }
    uint8_t* data = mapped_data + reflect_data->data_offset;
    if (reflect_data->data_type->data_type == PNANOVDB_REFLECT_TYPE_INT32)
    {
        int v = val.get<int>();
        memcpy(data, &v, sizeof(int));
    }
    else if (reflect_data->data_type->data_type == PNANOVDB_REFLECT_TYPE_UINT32)
    {
        uint32_t v = val.get<uint32_t>();
        memcpy(data, &v, sizeof(uint32_t));
    }
    else if (reflect_data->data_type->data_type == PNANOVDB_REFLECT_TYPE_FLOAT)
    {
        float v = val.get<float>();
        memcpy(data, &v, sizeof(float));
    }
    else if (reflect_data->data_type->data_type == PNANOVDB_REFLECT_TYPE_BOOL32)
    {
        uint32_t v = val.get<uint32_t>();
        memcpy(data, &v, sizeof(uint32_t));
    }
    else if (reflect_data->data_type->data_type == PNANOVDB_REFLECT_TYPE_UINT8)
    {
        uint8_t v = val.get<uint8_t>();
        memcpy(data, &v, sizeof(uint8_t));
    }
    else if (reflect_data->data_type->data_type == PNANOVDB_REFLECT_TYPE_UINT16)
    {
        uint16_t v = val.get<uint16_t>();
        memcpy(data, &v, sizeof(uint16_t));
    }
    else if (reflect_data->data_type->data_type == PNANOVDB_REFLECT_TYPE_UINT64)
    {
        uint64_t v = val.get<uint64_t>();
        memcpy(data, &v, sizeof(uint64_t));
    }
    else if (reflect_data->data_type->data_type == PNANOVDB_REFLECT_TYPE_DOUBLE)
    {
        double v = val.get<double>();
        memcpy(data, &v, sizeof(double));
    }
    else if (reflect_data->data_type->data_type == PNANOVDB_REFLECT_TYPE_INT64)
    {
        int64_t v = val.get<int64_t>();
        memcpy(data, &v, sizeof(int64_t));
    }
}

typedef struct pnanovdb_shader_params_metadata_t
{
    double min_double;
    pnanovdb_int64_t min_int64;
    double max_double;
    pnanovdb_int64_t max_int64;
    double step;
    pnanovdb_bool_t is_bool;
    pnanovdb_bool_t hidden;
    pnanovdb_bool_t use_slider;
} pnanovdb_shader_params_metadata_t;

#define PNANOVDB_REFLECT_TYPE pnanovdb_shader_params_metadata_t
PNANOVDB_REFLECT_BEGIN()
PNANOVDB_REFLECT_VALUE(double, min_double, 0, 0)
PNANOVDB_REFLECT_VALUE(pnanovdb_int64_t, min_int64, 0, 0)
PNANOVDB_REFLECT_VALUE(double, max_double, 0, 0)
PNANOVDB_REFLECT_VALUE(pnanovdb_int64_t, max_int64, 0, 0)
PNANOVDB_REFLECT_VALUE(double, step, 0, 0)
PNANOVDB_REFLECT_VALUE(pnanovdb_bool_t, is_bool, 0, 0)
PNANOVDB_REFLECT_VALUE(pnanovdb_bool_t, hidden, 0, 0)
PNANOVDB_REFLECT_VALUE(pnanovdb_bool_t, use_slider, 0, 0)
PNANOVDB_REFLECT_END(0)
#undef PNANOVDB_REFLECT_TYPE

void shader_context_map_shader_params_type(pnanovdb_shader_context_t* shaderContext,
                                           const pnanovdb_reflect_data_type_t** out_data_type,
                                           const pnanovdb_reflect_data_type_t** out_metadata_type)
{
    if (!shaderContext)
    {
        return;
    }
    shader_context_t* ptr = cast(shaderContext);

    ptr->reflect_datas.clear();
    ptr->reflect_metadatas.clear();
    ptr->owned_strings.clear();
    ptr->shader_params_default.clear();
    ptr->shader_params_metadefault.clear();

    // load parameters from compiled shader
    std::string shader_json_path = pnanovdb_shader::getCompiledShaderParamsFilePath(ptr->source.source_filename);
    std::ifstream shader_json_file(shader_json_path);
    if (shader_json_file)
    {
        nlohmann::ordered_json shader_json;
        shader_json_file >> shader_json;
        pnanovdb_uint64_t global_offset = 0llu;
        pnanovdb_uint64_t global_metaoffset = 0llu;
        if (nlohmann::ordered_json* shader_params = getCompiledShaderParamsObject(shader_json))
        {
            for (auto& [key, value] : shader_params->items())
            {
                // this breaks offset math
                //if (key.find("_pad") != std::string::npos)
                //{
                //    continue;
                //}
                assert(value.contains("type"));

                ptr->owned_strings.push_back(std::make_unique<std::string>(value["type"]));
                std::string* type_str = ptr->owned_strings.back().get();
                ptr->owned_strings.push_back(std::make_unique<std::string>(key));
                std::string* key_str = ptr->owned_strings.back().get();

                size_t num_elements = value["elementCount"];
                printf("type(%s) name(%s) elementCount(%zu)\n", type_str->c_str(), key_str->c_str(), num_elements);

                pnanovdb_reflect_data_t reflect_data = {};
                reflect_data.reflect_mode = PNANOVDB_REFLECT_MODE_VALUE;
                for (uint64_t type_idx = 0u; type_idx < json_type_name_count; type_idx++)
                {
                    if (*type_str == json_type_names[type_idx])
                    {
                        reflect_data.data_type = reflect_data_types[type_idx];
                    }
                }
                if (num_elements > 1u)
                {
                    reflect_data.data_type = get_vector_type(num_elements, reflect_data.data_type->data_type);
                    if (!reflect_data.data_type)
                    {
                        printf("Failed to resolve vector or matrix type!!!\n");
                    }
                }
                reflect_data.name = key_str->c_str();
                reflect_data.data_offset = global_offset;

                global_offset += reflect_data.data_type->element_size;

                ptr->reflect_datas.push_back(reflect_data);
            }
        }
        shader_json_file.close();

        ptr->shader_params_data_type.data_type = PNANOVDB_REFLECT_TYPE_STRUCT;
        ptr->shader_params_data_type.element_size = global_offset;
        ptr->shader_params_data_type.struct_typename = "shader_params_t";
        ptr->shader_params_data_type.child_reflect_datas = ptr->reflect_datas.data();
        ptr->shader_params_data_type.child_reflect_data_count = ptr->reflect_datas.size();
        ptr->shader_params_data_type.default_value = nullptr;

        ptr->shader_params_default.resize(global_offset);
        const std::optional<nlohmann::ordered_json> json = loadJsonFileFromShaderPath(ptr->source.source_filename);
        if (json && json->contains(pnanovdb_shader::SHADER_PARAM_JSON))
        {
            uint8_t* params_data = ptr->shader_params_default.data();
            ptr->shader_params_data_type.default_value = params_data;

            auto section = (*json)[pnanovdb_shader::SHADER_PARAM_JSON];
            for (auto& [name, value] : section.items())
            {
                // create owned version of name
                ptr->owned_strings.push_back(std::make_unique<std::string>(name));
                std::string* key_str = ptr->owned_strings.back().get();

                auto val = value["value"];
                // scatter default value
                for(pnanovdb_uint64_t reflect_idx = 0u;
                    reflect_idx < ptr->shader_params_data_type.child_reflect_data_count;
                    reflect_idx++)
                {
                    auto reflect_data = ptr->shader_params_data_type.child_reflect_datas[reflect_idx];
                    if (pnanovdb_reflect_string_compare(
                            reflect_data.name,
                            name.c_str()) == 0)
                    {
                        if (val.is_array())
                        {
                            if (reflect_data.data_type->data_type == PNANOVDB_REFLECT_TYPE_STRUCT)
                            {
                                const pnanovdb_reflect_data_type_t* data_type2 = reflect_data.data_type;
                                uint8_t* params_data2 = params_data + reflect_data.data_offset;
                                auto it = val.begin();
                                for (pnanovdb_uint64_t idx2 = 0u; idx2 < data_type2->child_reflect_data_count; idx2++)
                                {
                                    auto reflect_data2 = &data_type2->child_reflect_datas[idx2];
                                    copyReflectParameter(params_data2, reflect_data2, *it);
                                    it++;
                                }
                            }
                            else
                            {
                                printf("Json metadata has array of values, but data_type is not a struct\n");
                            }
                        }
                        else
                        {
                            copyReflectParameter(params_data, &reflect_data, val);
                        }
                    }
                }
                // add to metadata
                pnanovdb_shader_params_metadata_t meta = {};
                if (value.contains("min"))
                {
                    meta.min_double = value["min"].get<double>();
                    meta.min_int64 = value["min"].get<int64_t>();
                }
                if (value.contains("max"))
                {
                    meta.max_double = value["max"].get<double>();
                    meta.max_int64 = value["max"].get<int64_t>();
                }
                if (value.contains("step"))
                {
                    meta.step = value["step"].get<double>();
                }
                if (value.contains("isBool"))
                {
                    meta.is_bool = value["isBool"].get<bool>() ? PNANOVDB_TRUE : PNANOVDB_FALSE;
                }
                if (value.contains("hidden"))
                {
                    meta.hidden = value["hidden"].get<bool>() ? PNANOVDB_TRUE : PNANOVDB_FALSE;
                }
                if (value.contains("useSlider"))
                {
                    meta.use_slider = value["useSlider"].get<bool>() ? PNANOVDB_TRUE : PNANOVDB_FALSE;
                }

                pnanovdb_reflect_data_t reflect_data = {};
                reflect_data.reflect_mode = PNANOVDB_REFLECT_MODE_VALUE;
                reflect_data.data_type = PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_shader_params_metadata_t);
                reflect_data.name = key_str->c_str();
                reflect_data.data_offset = global_metaoffset;

                // commit
                ptr->shader_params_metadefault.insert(
                    ptr->shader_params_metadefault.end(),
                    (uint8_t*)&meta,
                    (uint8_t*)&meta + sizeof(meta));
                ptr->reflect_metadatas.push_back(reflect_data);
                global_metaoffset += sizeof(pnanovdb_shader_params_metadata_t);
            }
        }

        ptr->shader_params_metadata_type.data_type = PNANOVDB_REFLECT_TYPE_STRUCT;
        ptr->shader_params_metadata_type.element_size = global_metaoffset;
        ptr->shader_params_metadata_type.struct_typename = "shader_params_meta_t";
        ptr->shader_params_metadata_type.child_reflect_datas = ptr->reflect_metadatas.data();
        ptr->shader_params_metadata_type.child_reflect_data_count = ptr->reflect_metadatas.size();
        ptr->shader_params_metadata_type.default_value = ptr->shader_params_metadefault.data();
    }
    if (out_data_type)
    {
        *out_data_type = &ptr->shader_params_data_type;
    }
    if (out_metadata_type)
    {
        *out_metadata_type = &ptr->shader_params_metadata_type;
    }
}

void dispatch_shader(pnanovdb_compute_interface_t* contextInterface,
                     pnanovdb_compute_context_t* computeContext,
                     const pnanovdb_shader_context_t* shaderContext,
                     pnanovdb_compute_resource_t* resources,
                     pnanovdb_uint32_t grid_dim_x,
                     pnanovdb_uint32_t grid_dim_y,
                     pnanovdb_uint32_t grid_dim_z,
                     const char* debug_label)
{
    auto shader = cast(shaderContext);

    pnanovdb_compute_dispatch_params_t dispatch_params = {};
    dispatch_params.pipeline = shader->pipeline;
    dispatch_params.grid_dim_x = grid_dim_x;
    dispatch_params.grid_dim_y = grid_dim_y;
    dispatch_params.grid_dim_z = grid_dim_z;

    dispatch_params.descriptor_writes = shader->shader_build->descriptor_writes;
    dispatch_params.resources = resources;
    dispatch_params.descriptor_write_count = shader->shader_build->descriptor_write_count;

    dispatch_params.debug_label = debug_label ? debug_label : shader->shader_build->debug_label;

    contextInterface->dispatch(computeContext, &dispatch_params);
}

pnanovdb_bool_t dispatch_shader_on_nanovdb_array(const pnanovdb_compute_t* compute,
                                                 const pnanovdb_compute_device_t* device,
                                                 const pnanovdb_shader_context_t* shader_context,
                                                 pnanovdb_compute_array_t* nanovdb_array,
                                                 pnanovdb_int32_t image_width,
                                                 pnanovdb_int32_t image_height,
                                                 pnanovdb_compute_texture_t* background_image,
                                                 pnanovdb_compute_buffer_transient_t* upload_buffer,
                                                 pnanovdb_compute_buffer_transient_t* user_upload_buffer,
                                                 pnanovdb_compute_buffer_t** nanovdb_buffer,
                                                 pnanovdb_compute_buffer_transient_t** readback_buffer)
{
    if (!compute || !device || !shader_context)
    {
        return PNANOVDB_FALSE;
    }
    if (!nanovdb_array)
    {
        return PNANOVDB_FALSE;
    }
    if (image_width <= 0 || image_height <= 0 || !background_image)
    {
        return PNANOVDB_FALSE;
    }
    const pnanovdb_uint64_t size_in_bytes = nanovdb_array->element_count * nanovdb_array->element_size;
    if (size_in_bytes == 0)
    {
        return PNANOVDB_FALSE;
    }
    auto shader = cast(shader_context);
    if (!shader->shader_build)
    {
        return PNANOVDB_FALSE;
    }

    pnanovdb_compute_queue_t* queue = compute->device_interface.get_device_queue(device);
    pnanovdb_compute_interface_t* compute_interface = compute->device_interface.get_compute_interface(queue);
    pnanovdb_compute_context_t* compute_context = compute->device_interface.get_compute_context(queue);
    if (!queue || !compute_interface || !compute_context)
    {
        return PNANOVDB_FALSE;
    }

    if (nanovdb_array && *nanovdb_buffer == nullptr)
    {
        // nanovdb buffer to upload
        pnanovdb_compute_buffer_desc_t upload_desc = {};
        upload_desc.size_in_bytes = size_in_bytes;
        upload_desc.usage = PNANOVDB_COMPUTE_BUFFER_USAGE_COPY_SRC;

        pnanovdb_compute_buffer_t* nanovdb_upload_buffer =
            compute_interface->create_buffer(compute_context, PNANOVDB_COMPUTE_MEMORY_TYPE_UPLOAD, &upload_desc);
        if (!nanovdb_upload_buffer)
        {
            return PNANOVDB_FALSE;
        }
        void* mapped_upload = compute_interface->map_buffer(compute_context, nanovdb_upload_buffer);
        memcpy(mapped_upload, nanovdb_array->data, size_in_bytes);
        compute_interface->unmap_buffer(compute_context, nanovdb_upload_buffer);

        // uploaded nanovdb buffer
        pnanovdb_compute_buffer_desc_t buf_desc = {};
        buf_desc.size_in_bytes = size_in_bytes;
        buf_desc.usage = PNANOVDB_COMPUTE_BUFFER_USAGE_STRUCTURED | PNANOVDB_COMPUTE_BUFFER_USAGE_COPY_DST;
        buf_desc.structure_stride = nanovdb_array->element_size;

        *nanovdb_buffer =
            compute_interface->create_buffer(compute_context, PNANOVDB_COMPUTE_MEMORY_TYPE_DEVICE, &buf_desc);
        if (*nanovdb_buffer == nullptr)
        {
            return PNANOVDB_FALSE;
        }

        // upload nanovdb
        pnanovdb_compute_copy_buffer_params_t upload_params = {};
        upload_params.num_bytes = size_in_bytes;
        upload_params.src = compute_interface->register_buffer_as_transient(compute_context, nanovdb_upload_buffer);
        upload_params.dst = compute_interface->register_buffer_as_transient(compute_context, *nanovdb_buffer);
        upload_params.debug_label = "dispatch_shader_on_nanovdb_array_upload";
        compute_interface->copy_buffer(compute_context, &upload_params);

        // free upload buffer
        compute_interface->destroy_buffer(compute_context, nanovdb_upload_buffer);
    }

    pnanovdb_compute_buffer_desc_t image_buf_desc = {};
    image_buf_desc.size_in_bytes = pnanovdb_uint64_t(image_width * image_height * 4u);
    image_buf_desc.usage = PNANOVDB_COMPUTE_BUFFER_USAGE_RW_STRUCTURED | PNANOVDB_COMPUTE_BUFFER_USAGE_COPY_SRC;
    image_buf_desc.structure_stride = 4u;
    auto* image_buffer = compute_interface->get_buffer_transient(compute_context, &image_buf_desc);

    pnanovdb_compute_resource_t resources[5u] = {};
    resources[0u].buffer_transient = compute_interface->register_buffer_as_transient(compute_context, *nanovdb_buffer);
    resources[1u].buffer_transient = image_buffer;
    resources[2u].texture_transient = compute_interface->register_texture_as_transient(compute_context, background_image);
    resources[3u].buffer_transient = upload_buffer;
    resources[4u].buffer_transient = user_upload_buffer;

    compute->dispatch_shader(compute_interface, compute_context, shader_context, resources, (image_width + 15u) / 16u,
                             (image_height + 7u) / 8u, 1u, "dispatch_shader_on_nanovdb_array");

    if (*readback_buffer)
    {
        // reading final image back to CPU
        pnanovdb_compute_copy_buffer_params_t readback_params = {};
        readback_params.num_bytes = pnanovdb_uint64_t(image_width * image_height * 4u);
        readback_params.src = image_buffer;
        readback_params.dst = *readback_buffer;
        readback_params.debug_label = "dispatch_shader_on_nanovdb_array_readback";
        compute_interface->copy_buffer(compute_context, &readback_params);
    }

    return PNANOVDB_TRUE;
}

pnanovdb_bool_t dispatch_shader_on_array(const pnanovdb_compute_t* compute,
                                         const pnanovdb_compute_device_t* device,
                                         const char* shader_path,
                                         pnanovdb_uint32_t grid_dim_x,
                                         pnanovdb_uint32_t grid_dim_y,
                                         pnanovdb_uint32_t grid_dim_z,
                                         pnanovdb_compute_array_t* data_in,
                                         pnanovdb_compute_array_t* constants,
                                         pnanovdb_compute_array_t* data_out,
                                         pnanovdb_uint32_t dispatch_count,
                                         pnanovdb_uint64_t scratch_size,
                                         pnanovdb_uint64_t scratch_clear_size)
{
    if (!compute || !device)
    {
        printf("Error: shader dispatch failed, null compute or device\n");
        return PNANOVDB_FALSE;
    }
    if (!data_in->data)
    {
        printf("Error: shader dispatch failed, null data_in\n");
        return PNANOVDB_FALSE;
    }
    if (!constants->data)
    {
        printf("Error: shader dispatch failed, null constants\n");
        return PNANOVDB_FALSE;
    }

    pnanovdb_compiler_settings_t compile_settings = {};
    pnanovdb_compiler_settings_init(&compile_settings);

    pnanovdb_compute_queue_t* queue = compute->device_interface.get_device_queue(device);
    pnanovdb_compute_interface_t* compute_interface = compute->device_interface.get_compute_interface(queue);
    pnanovdb_compute_context_t* compute_context = compute->device_interface.get_compute_context(queue);

    pnanovdb_shader_context_t* shader_context = compute->create_shader_context(shader_path);
    if (compute->init_shader(compute, queue, shader_context, &compile_settings) == PNANOVDB_FALSE)
    {
        compute->destroy_shader_context(compute, queue, shader_context);
        return PNANOVDB_FALSE;
    }

    pnanovdb_compiler_settings_t clear_compile_settings = {};
    pnanovdb_compiler_settings_init(&clear_compile_settings);

    pnanovdb_shader_context_t* clear_shader_context = compute->create_shader_context("compute/clear_buffer.slang");
    if (compute->init_shader(compute, queue, clear_shader_context, &clear_compile_settings) == PNANOVDB_FALSE)
    {
        compute->destroy_shader_context(compute, queue, clear_shader_context);
        return PNANOVDB_FALSE;
    }

    auto* shader = cast(shader_context);
    auto* clear_shader = cast(clear_shader_context);

    compute->device_interface.enable_profiler(
        compute_context, (void*)"dispatch_shader_on_array", compute_profiler_report);

    pnanovdb_compute_buffer_desc_t buf_desc = {};

    // data_in upload and device
    buf_desc.usage = PNANOVDB_COMPUTE_BUFFER_USAGE_COPY_SRC;
    buf_desc.format = PNANOVDB_COMPUTE_FORMAT_UNKNOWN;
    buf_desc.structure_stride = 0u;
    buf_desc.size_in_bytes = data_in->element_count * data_in->element_size;
    pnanovdb_compute_buffer_t* data_in_upload =
        compute_interface->create_buffer(compute_context, PNANOVDB_COMPUTE_MEMORY_TYPE_UPLOAD, &buf_desc);
    buf_desc.usage = PNANOVDB_COMPUTE_BUFFER_USAGE_STRUCTURED | PNANOVDB_COMPUTE_BUFFER_USAGE_RW_STRUCTURED |
                     PNANOVDB_COMPUTE_BUFFER_USAGE_COPY_DST;
    buf_desc.structure_stride = 4u;
    pnanovdb_compute_buffer_t* data_in_device =
        compute_interface->create_buffer(compute_context, PNANOVDB_COMPUTE_MEMORY_TYPE_DEVICE, &buf_desc);

    // constants
    buf_desc.usage = PNANOVDB_COMPUTE_BUFFER_USAGE_CONSTANT;
    buf_desc.format = PNANOVDB_COMPUTE_FORMAT_UNKNOWN;
    buf_desc.structure_stride = 0u;
    buf_desc.size_in_bytes = constants->element_count * constants->element_size;
    pnanovdb_compute_buffer_t* constant_buffer =
        compute_interface->create_buffer(compute_context, PNANOVDB_COMPUTE_MEMORY_TYPE_UPLOAD, &buf_desc);

    // data_out device and readback
    buf_desc.usage = PNANOVDB_COMPUTE_BUFFER_USAGE_RW_STRUCTURED | PNANOVDB_COMPUTE_BUFFER_USAGE_COPY_SRC;
    buf_desc.format = PNANOVDB_COMPUTE_FORMAT_UNKNOWN;
    buf_desc.structure_stride = 4u;
    buf_desc.size_in_bytes = data_out->element_count == 0u ? 65536u : data_out->element_count * data_out->element_size;
    pnanovdb_compute_buffer_t* data_out_device =
        compute_interface->create_buffer(compute_context, PNANOVDB_COMPUTE_MEMORY_TYPE_DEVICE, &buf_desc);
    buf_desc.usage = PNANOVDB_COMPUTE_BUFFER_USAGE_COPY_DST;
    buf_desc.format = PNANOVDB_COMPUTE_FORMAT_UNKNOWN;
    buf_desc.structure_stride = 0u;
    buf_desc.size_in_bytes = data_out->element_count == 0u ? 65536u : data_out->element_count * data_out->element_size;
    pnanovdb_compute_buffer_t* data_out_readback =
        compute_interface->create_buffer(compute_context, PNANOVDB_COMPUTE_MEMORY_TYPE_READBACK, &buf_desc);

    // scratch buffer
    buf_desc.usage = PNANOVDB_COMPUTE_BUFFER_USAGE_RW_STRUCTURED;
    buf_desc.format = PNANOVDB_COMPUTE_FORMAT_UNKNOWN;
    buf_desc.structure_stride = 4u;
    buf_desc.size_in_bytes = scratch_size == 0u ? 65536u : scratch_size;
    pnanovdb_compute_buffer_t* scratch_device =
        compute_interface->create_buffer(compute_context, PNANOVDB_COMPUTE_MEMORY_TYPE_DEVICE, &buf_desc);

    // copy data_in
    void* mapped_data_in = compute_interface->map_buffer(compute_context, data_in_upload);
    memcpy(mapped_data_in, data_in->data, data_in->element_count * data_in->element_size);
    compute_interface->unmap_buffer(compute_context, data_in_upload);

    // copy constants
    void* mapped_constants = compute_interface->map_buffer(compute_context, constant_buffer);
    memcpy(mapped_constants, constants->data, constants->element_count * constants->element_size);
    compute_interface->unmap_buffer(compute_context, constant_buffer);

    // upload data_in
    pnanovdb_compute_copy_buffer_params_t copy_params = {};
    copy_params.num_bytes = data_in->element_count * data_in->element_size;
    copy_params.src = compute_interface->register_buffer_as_transient(compute_context, data_in_upload);
    copy_params.dst = compute_interface->register_buffer_as_transient(compute_context, data_in_device);
    copy_params.debug_label = "dispatch_shader_on_array_upload";
    compute_interface->copy_buffer(compute_context, &copy_params);

    pnanovdb_compute_buffer_transient_t* scratch_transient =
        compute_interface->register_buffer_as_transient(compute_context, scratch_device);

    // ------------------ clear dispatch params --------------------
    pnanovdb_compute_resource_t clear_resources[1u] = {};
    clear_resources[0u].buffer_transient = scratch_transient;

    // note: clear size in bytes, but compute shader writes words, so 4x workgroup size
    pnanovdb_uint32_t clear_grid_dim_x = (scratch_clear_size + 1023u) / 1024u;

    pnanovdb_compute_dispatch_params_t clear_dispatch_params = {};
    clear_dispatch_params.pipeline = clear_shader->pipeline;
    clear_dispatch_params.grid_dim_x = clear_grid_dim_x;
    clear_dispatch_params.grid_dim_y = 1u;
    clear_dispatch_params.grid_dim_z = 1u;

    clear_dispatch_params.descriptor_writes = clear_shader->shader_build->descriptor_writes;
    clear_dispatch_params.resources = clear_resources;
    clear_dispatch_params.descriptor_write_count = clear_shader->shader_build->descriptor_write_count;

    clear_dispatch_params.debug_label = "dispatch_shader_on_array_clear";

    // ------------------ compute dispatch params --------------------
    pnanovdb_compute_resource_t resources[4u] = {};
    resources[0u].buffer_transient = compute_interface->register_buffer_as_transient(compute_context, data_in_device);
    resources[1u].buffer_transient = compute_interface->register_buffer_as_transient(compute_context, constant_buffer);
    resources[2u].buffer_transient = compute_interface->register_buffer_as_transient(compute_context, data_out_device);
    resources[3u].buffer_transient = scratch_transient;

    pnanovdb_compute_dispatch_params_t dispatch_params = {};
    dispatch_params.pipeline = shader->pipeline;
    dispatch_params.grid_dim_x = grid_dim_x;
    dispatch_params.grid_dim_y = grid_dim_y;
    dispatch_params.grid_dim_z = grid_dim_z;

    dispatch_params.descriptor_writes = shader->shader_build->descriptor_writes;
    dispatch_params.resources = resources;
    dispatch_params.descriptor_write_count = shader->shader_build->descriptor_write_count;

    dispatch_params.debug_label = "dispatch_shader_on_array_dispatch";

    // ------------------ dispatch --------------------
    for (pnanovdb_uint32_t dispatch_idx = 0u; dispatch_idx < dispatch_count; dispatch_idx++)
    {
        if (scratch_clear_size != 0llu)
        {
            compute_interface->dispatch(compute_context, &clear_dispatch_params);
        }
        compute_interface->dispatch(compute_context, &dispatch_params);
    }

    // readback data_out
    copy_params.num_bytes = data_out->element_count * data_out->element_size;
    copy_params.src = compute_interface->register_buffer_as_transient(compute_context, data_out_device);
    copy_params.dst = compute_interface->register_buffer_as_transient(compute_context, data_out_readback);
    copy_params.debug_label = "dispatch_shader_on_array_readback";
    compute_interface->copy_buffer(compute_context, &copy_params);

    pnanovdb_uint64_t flushed_frame = 0llu;
    compute->device_interface.flush(queue, &flushed_frame, nullptr, nullptr);

    compute->device_interface.wait_idle(queue);

    // to flush profile
    compute->device_interface.flush(queue, &flushed_frame, nullptr, nullptr);

    // copy data_out
    void* mapped_data_out = compute_interface->map_buffer(compute_context, data_out_readback);
    memcpy(data_out->data, mapped_data_out, data_out->element_count * data_out->element_size);
    compute_interface->unmap_buffer(compute_context, data_out_readback);

    compute_interface->destroy_buffer(compute_context, data_in_upload);
    compute_interface->destroy_buffer(compute_context, data_in_device);
    compute_interface->destroy_buffer(compute_context, constant_buffer);
    compute_interface->destroy_buffer(compute_context, data_out_device);
    compute_interface->destroy_buffer(compute_context, data_out_readback);
    compute_interface->destroy_buffer(compute_context, scratch_device);

    compute->device_interface.disable_profiler(compute_context);

    compute->destroy_shader(compute_interface, &compute->shader_interface, compute_context, shader_context);
    compute->destroy_shader(compute_interface, &compute->shader_interface, compute_context, clear_shader_context);

    compute->destroy_shader_context(compute, queue, shader_context);
    compute->destroy_shader_context(compute, queue, clear_shader_context);

    return PNANOVDB_TRUE;
}

// #define LEAK_TRACKER
#ifdef LEAK_TRACKER
std::mutex g_leak_tracker_mutex;
struct leak_tracker_t
{
    std::unordered_map<pnanovdb_compute_array_t*, bool> active_arrays;
    leak_tracker_t()
    {
    }
    ~leak_tracker_t()
    {
        std::lock_guard<std::mutex> lock(g_leak_tracker_mutex);
        for (auto&& it : active_arrays)
        {
            if (it.second)
            {
                printf("Leak! data(%p) element_size(%zu) element_count(%zu) filepath(%s)\n", it.first->data,
                       it.first->element_size, it.first->element_count,
                       it.first->filepath ? it.first->filepath : "invalid");
            }
        }
    }
    void set(pnanovdb_compute_array_t* array, bool is_active)
    {
        std::lock_guard<std::mutex> lock(g_leak_tracker_mutex);
        active_arrays[array] = is_active;
    }
};
leak_tracker_t g_leak_tracker;
#endif

pnanovdb_compute_array_t* create_array(size_t element_size, pnanovdb_uint64_t element_count, const void* data)
{
    pnanovdb_compute_array_t* array = new pnanovdb_compute_array_t();
    array->element_count = element_count;
    array->element_size = element_size;
    size_t alloc_size = array->element_size * array->element_count;
    if (alloc_size < 65536u)
    {
        alloc_size = 65536u;
    }
    // new char[alloc_size];
    array->data = aligned_alloc(32u, alloc_size);
    memset(array->data, 0, 65536u);
    if (data)
    {
        memcpy(array->data, data, array->element_size * array->element_count);
    }
    else
    {
        memset(array->data, 0, array->element_size * array->element_count);
    }

#ifdef LEAK_TRACKER
    g_leak_tracker.set(array, true);
#endif

    return array;
}

pnanovdb_compute_array_t* duplicate_array(pnanovdb_compute_array_t* array)
{
    return create_array(array->element_size, array->element_count, array->data);
}

void destroy_array(pnanovdb_compute_array_t* array)
{
    if (!array)
    {
        return;
    }

    if (!array->data)
    {
        delete array;
        return;
    }

#ifdef LEAK_TRACKER
    g_leak_tracker.set(array, false);
#endif

    //delete[] (char*)array->data;
    free(array->data);
    array->data = nullptr;
    delete array;
}

void* map_array(pnanovdb_compute_array_t* array)
{
    if (!array)
    {
        printf("Error: map_array failed, null array\n");
        return nullptr;
    }
    return array->data;
}

void unmap_array(pnanovdb_compute_array_t* array)
{
    if (!array)
    {
        printf("Error: unmap_array failed, null array\n");
        return;
    }
}


void compute_array_print_range(const pnanovdb_compute_t* compute,
                               pnanovdb_compute_log_print_t log_print,
                               const char* name,
                               pnanovdb_compute_array_t* arr,
                               pnanovdb_uint32_t channel_count)
{
    if (channel_count > 4u)
    {
        channel_count = 4u;
    }
    int print_count = 0;
    float val_min[4] = { INFINITY, INFINITY, INFINITY, INFINITY };
    float val_max[4] = { -INFINITY, -INFINITY, -INFINITY, -INFINITY };
    float val_min_anisotropy = INFINITY;
    float val_max_anisotropy = -INFINITY;
    double val_ave_anisotropy = 0.0;
    double val_ave[4] = { 0.0, 0.0, 0.0, 0.0 };
    float* mapped_val = (float*)compute->map_array(arr);
    int aniso_type[4] = {};
    for (pnanovdb_uint64_t idx = 0u; idx < arr->element_count; idx++)
    {
        pnanovdb_uint64_t channel_idx = idx % channel_count;
        val_min[channel_idx] = fminf(val_min[channel_idx], mapped_val[idx]);
        val_max[channel_idx] = fmaxf(val_max[channel_idx], mapped_val[idx]);
        val_ave[channel_idx] = val_ave[channel_idx] + (double)mapped_val[idx];
        if (channel_idx == 0u)
        {
            float abs_val_min = INFINITY;
            float abs_val_max = -INFINITY;
            for (pnanovdb_uint32_t channel_idx_off = 0u; channel_idx_off < channel_count; channel_idx_off++)
            {
                float abs_val = fabsf(mapped_val[idx + channel_idx_off]);
                abs_val_min = fminf(abs_val_min, abs_val);
                abs_val_max = fmaxf(abs_val_max, abs_val);
            }
            float val_anisotropy = abs_val_max / abs_val_min;
            val_min_anisotropy = fminf(val_min_anisotropy, val_anisotropy);
            val_max_anisotropy = fmaxf(val_max_anisotropy, val_anisotropy);
            val_ave_anisotropy = val_ave_anisotropy + (double)val_anisotropy;
            if (print_count < 4u && channel_count == 3u && val_anisotropy > 30000000.f)
            {
                print_count++;
                if (log_print)
                {
                    log_print(PNANOVDB_COMPUTE_LOG_LEVEL_DEBUG, "array(%s) val(%e,%e,%e)", name, mapped_val[idx],
                              mapped_val[idx + 1], mapped_val[idx + 2]);
                }
            }
            if (channel_count == 3u)
            {
                float sx = fabsf(mapped_val[idx]);
                float sy = fabsf(mapped_val[idx + 1]);
                float sz = fabsf(mapped_val[idx + 2]);
                float smax = fmaxf(fmaxf(sx, sy), sz);
                int s_type = 0;
                s_type += (sx >= 0.5f * smax) ? 1 : 0;
                s_type += (sy >= 0.5f * smax) ? 1 : 0;
                s_type += (sz >= 0.5f * smax) ? 1 : 0;
                aniso_type[s_type]++;
            }
        }
    }
    compute->unmap_array(arr);
    for (pnanovdb_uint32_t channel_idx = 0u; channel_idx < channel_count; channel_idx++)
    {
        val_ave[channel_idx] /= (double)(arr->element_count / channel_count);
        if (log_print)
        {
            log_print(PNANOVDB_COMPUTE_LOG_LEVEL_DEBUG, "array(%s) channel(%d) min(%f) max(%f) ave(%f)", name,
                      channel_idx, val_min[channel_idx], val_max[channel_idx], val_ave[channel_idx]);
        }
    }
    val_ave_anisotropy /= (double)(arr->element_count / channel_count);
    if (log_print)
    {
        log_print(PNANOVDB_COMPUTE_LOG_LEVEL_DEBUG, "array(%s) min_anisotropy(%f) max_anisotropy(%f) ave_anisotropy(%f)",
                  name, val_min_anisotropy, val_max_anisotropy, val_ave_anisotropy);
        log_print(PNANOVDB_COMPUTE_LOG_LEVEL_DEBUG, "array(%s) aniso_type_count(%d,%d,%d,%d)", name, aniso_type[0],
                  aniso_type[1], aniso_type[2], aniso_type[3]);
    }
}

pnanovdb_compute_array_t* nanovdb_from_image_rgba8(pnanovdb_compute_array_t* image_data,
                                                   pnanovdb_uint32_t width,
                                                   pnanovdb_uint32_t height)
{
    // for now, make empty grid with blindmetadata
    pnanovdb_uint64_t header_size = PNANOVDB_GRID_SIZE + PNANOVDB_TREE_SIZE +
                                    PNANOVDB_GRID_TYPE_GET(PNANOVDB_GRID_TYPE_ONINDEX, root_size) +
                                    PNANOVDB_GRIDBLINDMETADATA_SIZE;
    pnanovdb_uint64_t data_size = image_data->element_count * image_data->element_size;
    pnanovdb_uint64_t size = header_size + data_size;

    pnanovdb_compute_array_t* nvdb_array = create_array(4u, size / 4u, nullptr);

    uint8_t* nvdb_data = (uint8_t*)nvdb_array->data;

    memcpy(nvdb_data + header_size, image_data->data, data_size);

    pnanovdb_buf_t buf = pnanovdb_make_buf((pnanovdb_uint32_t*)nvdb_data, size / 4u);

    float voxel_size = 1.f;
    float voxel_size_inv = 1.f / voxel_size;

    pnanovdb_grid_handle_t grid = { pnanovdb_address_null() };
    pnanovdb_grid_set_magic(buf, grid, PNANOVDB_MAGIC_GRID);
    pnanovdb_grid_set_version(buf, grid,
                              pnanovdb_make_version(PNANOVDB_MAJOR_VERSION_NUMBER, PNANOVDB_MINOR_VERSION_NUMBER,
                                                    PNANOVDB_PATCH_VERSION_NUMBER));
    pnanovdb_grid_set_flags(buf, grid, 0u);
    pnanovdb_grid_set_grid_index(buf, grid, 0u);
    pnanovdb_grid_set_grid_count(buf, grid, 1u);
    pnanovdb_grid_set_grid_size(buf, grid, size);
    pnanovdb_grid_set_grid_name(buf, grid, 0u, 0x61626772); // "rgba"
    pnanovdb_grid_set_grid_name(buf, grid, 1u, 0x00000000);
    pnanovdb_grid_set_voxel_size(buf, grid, 0u, voxel_size);
    pnanovdb_grid_set_voxel_size(buf, grid, 1u, voxel_size);
    pnanovdb_grid_set_voxel_size(buf, grid, 2u, voxel_size);
    pnanovdb_grid_set_grid_class(buf, grid, PNANOVDB_GRID_CLASS_UNKNOWN);
    pnanovdb_grid_set_grid_type(buf, grid, PNANOVDB_GRID_TYPE_ONINDEX);

    pnanovdb_map_handle_t map = pnanovdb_grid_get_map(buf, grid);
    pnanovdb_map_set_matf(buf, map, 0u, voxel_size);
    pnanovdb_map_set_matf(buf, map, 4u, voxel_size);
    pnanovdb_map_set_matf(buf, map, 8u, voxel_size);
    pnanovdb_map_set_invmatf(buf, map, 0u, voxel_size_inv);
    pnanovdb_map_set_invmatf(buf, map, 4u, voxel_size_inv);
    pnanovdb_map_set_invmatf(buf, map, 8u, voxel_size_inv);
    pnanovdb_map_set_matd(buf, map, 0u, voxel_size);
    pnanovdb_map_set_matd(buf, map, 4u, voxel_size);
    pnanovdb_map_set_matd(buf, map, 8u, voxel_size);
    pnanovdb_map_set_invmatd(buf, map, 0u, voxel_size_inv);
    pnanovdb_map_set_invmatd(buf, map, 4u, voxel_size_inv);
    pnanovdb_map_set_invmatd(buf, map, 8u, voxel_size_inv);
    pnanovdb_map_set_vecf(buf, map, 0u, 0.f);
    pnanovdb_map_set_vecf(buf, map, 1u, 0.f);
    pnanovdb_map_set_vecf(buf, map, 2u, 0.f);
    pnanovdb_map_set_vecd(buf, map, 0u, 0.0);
    pnanovdb_map_set_vecd(buf, map, 1u, 0.0);
    pnanovdb_map_set_vecd(buf, map, 2u, 0.0);

    pnanovdb_tree_handle_t tree = pnanovdb_grid_get_tree(buf, grid);
    pnanovdb_root_handle_t root = { pnanovdb_address_offset(tree.address, PNANOVDB_TREE_SIZE) };

    pnanovdb_tree_set_first_root(buf, tree, root);

    pnanovdb_coord_t ijk_min = { 0, 0, 0 };
    pnanovdb_coord_t ijk_max = { int(width) - 1, int(height) - 1, 0 };

    pnanovdb_root_set_bbox_min(buf, root, PNANOVDB_REF(ijk_min));
    pnanovdb_root_set_bbox_max(buf, root, PNANOVDB_REF(ijk_max));

    pnanovdb_gridblindmetadata_handle_t meta = { pnanovdb_address_offset(
        root.address, PNANOVDB_GRID_TYPE_GET(PNANOVDB_GRID_TYPE_ONINDEX, root_size)) };

    pnanovdb_grid_set_first_gridblindmetadata(buf, grid, meta);
    pnanovdb_grid_set_blind_metadata_count(buf, grid, 1u);

    pnanovdb_gridblindmetadata_set_data_offset(buf, meta, PNANOVDB_GRIDBLINDMETADATA_SIZE);
    pnanovdb_gridblindmetadata_set_value_count(buf, meta, data_size / 4u);
    pnanovdb_gridblindmetadata_set_value_size(buf, meta, 4u);
    pnanovdb_gridblindmetadata_set_data_class(buf, meta, 0u);
    pnanovdb_gridblindmetadata_set_data_type(buf, meta, PNANOVDB_GRID_TYPE_RGBA8);
    pnanovdb_gridblindmetadata_set_name(buf, meta, 0u, 0u);

    return nvdb_array;
}

std::mutex token_mutex;
std::unordered_map<std::string, std::unique_ptr<pnanovdb_compute_token_t>> tokens;
std::vector<std::unique_ptr<std::string>> strings;

pnanovdb_compute_token_t* get_token(const char* name)
{
    std::lock_guard<std::mutex> token_lock(token_mutex);

    std::string key(name);
    auto it = tokens.find(key);
    if (it != tokens.end())
    {
        return it->second.get();
    }

    strings.push_back(std::make_unique<std::string>(std::move(key)));

    std::unique_ptr<pnanovdb_compute_token_t> token = std::make_unique<pnanovdb_compute_token_t>();

    pnanovdb_compute_token_t* ret = token.get();
    ret->id = strings.size();
    ret->str = strings.back().get()->c_str();

    tokens[*(strings.back().get())] = std::move(token);

    return ret;
}

PNANOVDB_API pnanovdb_compute_t* pnanovdb_get_compute()
{
    static pnanovdb_compute_t compute = { PNANOVDB_REFLECT_INTERFACE_INIT(pnanovdb_compute_t) };

    compute.load_nanovdb = load_nanovdb;
    compute.save_nanovdb = save_nanovdb;
    compute.create_shader_context = create_shader_context;
    compute.destroy_shader_context = destroy_shader_context;
    compute.init_shader = init_shader;
    compute.destroy_shader = destroy_shader;
    compute.dispatch_shader = dispatch_shader;
    compute.dispatch_shader_on_array = dispatch_shader_on_array;
    compute.dispatch_shader_on_nanovdb_array = dispatch_shader_on_nanovdb_array;
    compute.create_array = create_array;
    compute.destroy_array = destroy_array;
    compute.duplicate_array = duplicate_array;
    compute.map_array = map_array;
    compute.unmap_array = unmap_array;
    compute.compute_array_print_range = compute_array_print_range;
    compute.nanovdb_from_image_rgba8 = nanovdb_from_image_rgba8;
    compute.shader_context_map_shader_params_type = shader_context_map_shader_params_type;
    compute.get_token = get_token;

    return &compute;
}
}
