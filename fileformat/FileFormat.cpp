// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/fileformat/FileFormat.cpp

    \author Petra Hapalova

    \brief
*/

#include "nanovdb_editor/putil/FileFormat.h"
#ifdef NANOVDB_EDITOR_E57_FORMAT
#    include "nanovdb_editor/fileformat/ReaderE57.h"
#endif
#include "nanovdb_editor/putil/Raster.h"

#include <cnpy.h>
#include <fstream>
#include <nlohmann/json.hpp>
#include <zstr.hpp>

#include <string>
#include <cstring>
#include <vector>
#include <memory>

namespace pnanovdb_fileformat
{

static const char* s_supportedExtensions[] = { ".ply", ".npz",
#ifdef NANOVDB_EDITOR_E57_FORMAT
                                               ".e57",
#endif
                                               ".ingp" };

static std::string get_file_extension(const char* filename)
{
    std::string path(filename);
    size_t pos = path.find_last_of('.');
    if (pos != std::string::npos && pos < path.length() - 1)
    {
        return path.substr(pos);
    }
    return "";
}

pnanovdb_bool_t can_load_file(const char* filename)
{
    if (!filename)
    {
        return PNANOVDB_FALSE;
    }

    std::string extension = get_file_extension(filename);
    for (const auto& supportedExt : s_supportedExtensions)
    {
        if (extension == supportedExt)
        {
            return PNANOVDB_TRUE;
        }
    }

    return PNANOVDB_FALSE;
}

static pnanovdb_bool_t load_npz_file(const char* filename,
                                     pnanovdb_uint32_t array_count,
                                     const char** array_names,
                                     pnanovdb_compute_array_t** out_arrays)
{
    if (!filename || !array_names || !out_arrays)
    {
        return PNANOVDB_FALSE;
    }

    cnpy::npz_t npz_dict = {};
    try
    {
        npz_dict = cnpy::npz_load(filename);
    }
    catch (const std::exception& e)
    {
        printf("Error loading npz file: %s\n", e.what());
        return PNANOVDB_FALSE;
    }

    static const uint32_t name_alias_count = 2u;
    const char* name_aliases[name_alias_count][2] = {
        {"sh_0", "sh"},
        {"sh_n", "sh"},
    };

    // early exit if not all arrays are found
    for (pnanovdb_uint32_t i = 0; i < array_count; i++)
    {
        const char* array_name = array_names[i];
        bool found_match = npz_dict.count(array_name) > 0;
        for (uint32_t alias_idx = 0u; !found_match && alias_idx < name_alias_count; alias_idx++)
        {
            found_match = strcmp(array_name, name_aliases[alias_idx][0]) == 0 &&
                npz_dict.count(name_aliases[alias_idx][1]) > 0;
        }
        if (!found_match)
        {
            printf("Warning: Array '%s' not found in npz file\n", array_name);
            return PNANOVDB_FALSE;
        }
    }

    for (pnanovdb_uint32_t i = 0; i < array_count; i++)
    {
        const char* array_name = array_names[i];
        const char* array_name_aliased = array_name;
        bool found_match = npz_dict.count(array_name) > 0;
        for (uint32_t alias_idx = 0u; !found_match && alias_idx < name_alias_count; alias_idx++)
        {
            array_name_aliased = name_aliases[alias_idx][1];
            found_match = strcmp(array_name, name_aliases[alias_idx][0]) == 0 &&
                npz_dict.count(name_aliases[alias_idx][1]) > 0;
        }

        if (found_match)
        {
            cnpy::NpyArray npz_array = npz_dict[array_name_aliased];

            out_arrays[i] = new pnanovdb_compute_array_t();

            size_t total_size = 1;
            size_t vector_stride = 1u;
            size_t vector_width = 1u;
            for (size_t shape_idx = 0u; shape_idx < npz_array.shape.size(); shape_idx++)
            {
                total_size *= npz_array.shape[shape_idx];
                if (shape_idx == 1u)
                {
                    vector_stride = npz_array.shape[shape_idx];
                }
                if (shape_idx == 2u)
                {
                    vector_width = npz_array.shape[shape_idx];
                }
            }

            if (array_name == array_name_aliased)
            {
                out_arrays[i]->element_count = total_size;
                out_arrays[i]->element_size = npz_array.word_size;
                out_arrays[i]->data = new char[total_size * npz_array.word_size];
                memcpy(out_arrays[i]->data, npz_array.data<char>(), total_size * npz_array.word_size);
            }
            else if (strcmp(array_name, "sh_0") == 0)
            {
                size_t dst_size = total_size / vector_stride;
                out_arrays[i]->element_count = dst_size;
                out_arrays[i]->element_size = npz_array.word_size;
                out_arrays[i]->data = new char[dst_size * npz_array.word_size];
                size_t sh_count = total_size / (vector_stride * vector_width);
                for (size_t sh_idx = 0u; sh_idx < sh_count; sh_idx++)
                {
                    memcpy(static_cast<char*>(out_arrays[i]->data) + sh_idx * vector_width * npz_array.word_size,
                        npz_array.data<char>() + sh_idx * vector_stride * vector_width * npz_array.word_size,
                        vector_width * npz_array.word_size);
                }
            }
            else if (strcmp(array_name, "sh_n") == 0)
            {
                size_t dst_size = (total_size * (vector_stride - 1u)) / vector_stride;
                out_arrays[i]->element_count = dst_size;
                out_arrays[i]->element_size = npz_array.word_size;
                out_arrays[i]->data = new char[dst_size * npz_array.word_size];
                size_t sh_count = total_size / (vector_stride * vector_width);
                for (size_t sh_idx = 0u; sh_idx < sh_count; sh_idx++)
                {
                    memcpy(static_cast<char*>(out_arrays[i]->data) + sh_idx * (vector_stride - 1u) * vector_width * npz_array.word_size,
                        npz_array.data<char>() + sh_idx * vector_stride * vector_width * npz_array.word_size + vector_width * npz_array.word_size,
                        (vector_stride - 1u) * vector_width * npz_array.word_size);
                }
            }
        }
        else
        {
            out_arrays[i] = nullptr;
        }
    }

    return PNANOVDB_TRUE;
}

static pnanovdb_bool_t load_ply_file(const char* filename,
                                     pnanovdb_uint32_t array_count,
                                     const char** array_names,
                                     pnanovdb_compute_array_t** out_arrays)
{
    if (!filename || !array_names || !out_arrays)
    {
        return PNANOVDB_FALSE;
    }

    FILE* file = fopen(filename, "rb");
    if (!file)
    {
        printf("Error loading ply file\n");
        return PNANOVDB_FALSE;
    }

    std::vector<std::string> properties;

    uint64_t vertex_count = 0llu;
    bool vertex_push_enabled = false;
    bool is_fvdb_gs = false;
    char buf[256u] = {};
    while (fgets(buf, 255u, file))
    {
        if (strcmp("end_header\n", buf) == 0)
        {
            break;
        }
        std::string line(buf);
        if (line.find("property") != std::string::npos)
        {
            if (vertex_push_enabled)
            {
                properties.push_back(line);
            }
        }
        if (line.find("element vertex") != std::string::npos)
        {
            std::string count_str = line.substr(sizeof("element vertex"));
            vertex_count = (uint64_t)std::stoll(count_str);
            vertex_push_enabled = true;
        }
        else if (line.find("element") != std::string::npos)
        {
            vertex_push_enabled = false;
        }
        if (line.find("comment fvdb_gs_ply_version fvdb_ply") != std::string::npos)
        {
            is_fvdb_gs = true;
        }
    }
    // printf("vertex_count(%llu)\n", (unsigned long long int)vertex_count);

    std::vector<float> arr_means;
    std::vector<float> arr_opacities;
    std::vector<float> arr_quaternions;
    std::vector<float> arr_scales;
    std::vector<float> arr_sh_0;
    std::vector<float> arr_sh_n;

    auto resolve_prop = [&](const char* str)
    {
        for (uint32_t idx = 0u; idx < properties.size(); idx++)
        {
            if (properties[idx] == str)
            {
                return idx;
            }
        }
        printf("Error: Failed to resolve: %s", str);
        return uint32_t(~0u);
    };

    uint32_t prop_x = resolve_prop("property float x\n");
    uint32_t prop_y = resolve_prop("property float y\n");
    uint32_t prop_z = resolve_prop("property float z\n");
    uint32_t prop_scale_0 = resolve_prop("property float scale_0\n");
    uint32_t prop_scale_1 = resolve_prop("property float scale_1\n");
    uint32_t prop_scale_2 = resolve_prop("property float scale_2\n");
    uint32_t prop_rot_0 = resolve_prop("property float rot_0\n");
    uint32_t prop_rot_1 = resolve_prop("property float rot_1\n");
    uint32_t prop_rot_2 = resolve_prop("property float rot_2\n");
    uint32_t prop_rot_3 = resolve_prop("property float rot_3\n");
    uint32_t prop_opacity = resolve_prop("property float opacity\n");
    uint32_t prop_f_dc_0 = resolve_prop("property float f_dc_0\n");
    uint32_t prop_f_dc_1 = resolve_prop("property float f_dc_1\n");
    uint32_t prop_f_dc_2 = resolve_prop("property float f_dc_2\n");
    uint32_t prop_f_rest_0 = resolve_prop("property float f_rest_0\n");

    std::vector<float> element;
    element.resize(properties.size());
    size_t element_size = element.size() * sizeof(float);

    while (fread(element.data(), 1u, element_size, file) == element_size && arr_opacities.size() < vertex_count)
    {
        arr_means.push_back(element[prop_x]);
        arr_means.push_back(element[prop_y]);
        arr_means.push_back(element[prop_z]);
        arr_opacities.push_back(element[prop_opacity]);
        arr_quaternions.push_back(element[prop_rot_0]);
        arr_quaternions.push_back(element[prop_rot_1]);
        arr_quaternions.push_back(element[prop_rot_2]);
        arr_quaternions.push_back(element[prop_rot_3]);
        arr_scales.push_back(element[prop_scale_0]);
        arr_scales.push_back(element[prop_scale_1]);
        arr_scales.push_back(element[prop_scale_2]);
        // put in rrrgggbbb convention
        arr_sh_0.push_back(element[prop_f_dc_0]);
        arr_sh_0.push_back(element[prop_f_dc_1]);
        arr_sh_0.push_back(element[prop_f_dc_2]);

        if (prop_f_rest_0 != ~0u)
        {
            for (unsigned int idx = 0u; idx < 15u; idx++)
            {
                arr_sh_n.push_back(element[prop_f_rest_0 + idx]);
            }
        }
        if (prop_f_rest_0 != ~0u)
        {
            for (unsigned int idx = 0u; idx < 15u; idx++)
            {
                arr_sh_n.push_back(element[prop_f_rest_0 + 15u + idx]);
            }
        }
        if (prop_f_rest_0 != ~0u)
        {
            for (unsigned int idx = 0u; idx < 15u; idx++)
            {
                arr_sh_n.push_back(element[prop_f_rest_0 + 30u + idx]);
            }
        }
    }

    fclose(file);

    // Map the arrays to the requested names
    for (pnanovdb_uint32_t i = 0; i < array_count; i++)
    {
        const char* array_name = array_names[i];
        std::vector<float>* source_array = nullptr;

        if (strcmp(array_name, "means") == 0)
        {
            source_array = &arr_means;
        }
        else if (strcmp(array_name, "opacities") == 0)
        {
            source_array = &arr_opacities;
        }
        else if (strcmp(array_name, "quaternions") == 0)
        {
            source_array = &arr_quaternions;
        }
        else if (strcmp(array_name, "scales") == 0)
        {
            source_array = &arr_scales;
        }
        else if (strcmp(array_name, "sh_0") == 0)
        {
            source_array = &arr_sh_0;
        }
        else if (strcmp(array_name, "sh_n") == 0)
        {
            source_array = &arr_sh_n;
        }

        if (source_array && !source_array->empty())
        {
            out_arrays[i] = new pnanovdb_compute_array_t();
            out_arrays[i]->element_count = source_array->size();
            out_arrays[i]->element_size = sizeof(float);
            out_arrays[i]->data = new float[source_array->size()];
            memcpy(out_arrays[i]->data, source_array->data(), source_array->size() * sizeof(float));
        }
        else
        {
            printf("Warning: Array '%s' not found in ply file\n", array_name);
            out_arrays[i] = nullptr;
        }
    }

    return PNANOVDB_TRUE;
}

static pnanovdb_bool_t load_e57_file(const char* filename,
                                     pnanovdb_uint32_t array_count,
                                     const char** array_names,
                                     pnanovdb_compute_array_t** out_arrays)
{
    if (!filename || !array_names || !out_arrays)
    {
        return PNANOVDB_FALSE;
    }

    float* positions_array = nullptr;
    float* colors_array = nullptr;
    float* normals_array = nullptr;
    size_t array_size = 0u;
#ifdef NANOVDB_EDITOR_E57_FORMAT
    e57_to_float(filename, &array_size, &positions_array, &colors_array, &normals_array);
#endif
    if (array_size == 0u)
    {
        return PNANOVDB_FALSE;
    }

    // Map the arrays to the requested names
    for (pnanovdb_uint32_t i = 0; i < array_count; i++)
    {
        const char* array_name = array_names[i];
        float* source_array = nullptr;
        size_t element_count = 0;

        if (strcmp(array_name, "positions") == 0)
        {
            source_array = positions_array;
            element_count = array_size;
        }
        else if (strcmp(array_name, "colors") == 0)
        {
            source_array = colors_array;
            element_count = array_size;
        }
        else if (strcmp(array_name, "normals") == 0)
        {
            source_array = normals_array;
            element_count = array_size;
        }

        if (source_array && element_count > 0)
        {
            out_arrays[i] = new pnanovdb_compute_array_t();
            out_arrays[i]->element_count = element_count;
            out_arrays[i]->element_size = sizeof(float);
            out_arrays[i]->data = new float[element_count];
            memcpy(out_arrays[i]->data, source_array, element_count * sizeof(float));
        }
        else
        {
            printf("Warning: Array '%s' not found in e57 file\n", array_name);
            out_arrays[i] = nullptr;
        }
    }

    // Clean up temporary arrays
    delete[] positions_array;
    if (colors_array)
    {
        delete[] colors_array;
    }
    if (normals_array)
    {
        delete[] normals_array;
    }

    return PNANOVDB_TRUE;
}

template <class T>
static void loadJsonArray(nlohmann::json& j, const char* key, pnanovdb_compute_array_t* array, const char* filename)
{
    if (j.contains(key))
    {
        const nlohmann::json::binary_t& buf = j[key];
        array->element_count = (buf.size() + sizeof(T) - 1u) / sizeof(T);
        array->element_size = sizeof(T);
        array->data = new T[array->element_count];
        array->filepath = filename;
        if (array->element_count > 0u)
        {
            ((T*)array->data)[array->element_count - 1u] = T();
        }
        memcpy(array->data, buf.data(), buf.size());
    }
    else
    {
        printf("Warning: Array '%s' not found in %s\n", key, filename);
    }
}

static pnanovdb_bool_t load_ingp_file(const char* filename,
                                      pnanovdb_uint32_t array_count,
                                      const char** array_names,
                                      pnanovdb_compute_array_t** out_arrays)
{
    if (!filename || !array_names || !out_arrays)
    {
        return PNANOVDB_FALSE;
    }

    std::ifstream file(filename, std::ios::binary);
    if (!file.is_open())
    {
        printf("Error: Could not open file: %s\n", filename);
        return PNANOVDB_FALSE;
    }

    zstr::istream zstream(file);
    try
    {
        nlohmann::json j = nlohmann::json::from_msgpack(zstream);

        printf("Parsed %s, contains:\n", filename);
        for (auto& [key, value] : j.items())
        {
            printf("%s\n", key.c_str());
        }

        size_t point_count = j["mog_num"];
        int sph_degree = j["mog_sph_degree"];
        bool halfPrecision = false;
        if (j.contains("precision") && std::string(j["precision"]) == std::string("half"))
        {
            halfPrecision = true;
        }

        for (int i = 0; i < array_count; i++)
        {
            out_arrays[i] = new pnanovdb_compute_array_t();
        }

        // Expect the arrays to be in order: means, opacities, quaternions, scales, features
        {
            int i = 0;
            {
                loadJsonArray<pnanovdb_vec3_t>(j, array_names[i], out_arrays[i], filename);
                i++;
            }
            {
                loadJsonArray<float>(j, array_names[i], out_arrays[i], filename);
                i++;
            }
            {
                loadJsonArray<pnanovdb_vec4_t>(j, array_names[i], out_arrays[i], filename);
                i++;
            }
            {
                loadJsonArray<pnanovdb_vec3_t>(j, array_names[i], out_arrays[i], filename);
                i++;
            }
            {
                loadJsonArray<float>(j, array_names[i], out_arrays[i], filename);
                i++;
            }
        }

        // "checksum"
        for (int i = 0; i < array_count - 1; i++)
        {
            if (out_arrays[i] && out_arrays[i]->element_count != point_count)
            {
                printf("Warning: Array '%s' has wrong element count (%zu != %zu)\n", array_names[i],
                       out_arrays[i]->element_count, point_count);
            }
        }
    }
    catch (const nlohmann::json::parse_error& e)
    {
        printf("Parse error: %s\n", e.what());
        return PNANOVDB_FALSE;
    }

    return PNANOVDB_TRUE;
}

pnanovdb_bool_t load_file(const char* filename,
                          pnanovdb_uint32_t array_count,
                          const char** array_names,
                          pnanovdb_compute_array_t** out_arrays)
{
    if (!filename || !array_names || !out_arrays)
    {
        return PNANOVDB_FALSE;
    }

    if (!can_load_file(filename))
    {
        printf("Error: File format not supported: %s\n", filename);
        return PNANOVDB_FALSE;
    }

    std::string extension = get_file_extension(filename);

    pnanovdb_bool_t loaded = PNANOVDB_FALSE;
    if (extension == ".npz")
    {
        loaded = load_npz_file(filename, array_count, array_names, out_arrays);
    }
    else if (extension == ".ply")
    {
        loaded = load_ply_file(filename, array_count, array_names, out_arrays);
    }
#ifdef NANOVDB_EDITOR_E57_FORMAT
    else if (extension == ".e57")
    {
        loaded = load_e57_file(filename, array_count, array_names, out_arrays);
    }
#endif
    else if (extension == ".ingp")
    {
        loaded = load_ingp_file(filename, array_count, array_names, out_arrays);
    }

    if (loaded == PNANOVDB_FALSE)
    {
        printf("Error: Failed to load file: %s\n", filename);
        return PNANOVDB_FALSE;
    }

    return PNANOVDB_TRUE;
}

} // namespace pnanovdb_fileformat

PNANOVDB_API pnanovdb_fileformat_t* pnanovdb_get_fileformat()
{
    static pnanovdb_fileformat_t fileformat = { PNANOVDB_REFLECT_INTERFACE_INIT(pnanovdb_fileformat_t) };

    fileformat.can_load_file = pnanovdb_fileformat::can_load_file;
    fileformat.load_file = pnanovdb_fileformat::load_file;
#ifdef NANOVDB_EDITOR_E57_FORMAT
    fileformat.e57_to_float = pnanovdb_fileformat::e57_to_float;
#endif

    return &fileformat;
}
