// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/test/main.cpp

    \author Petra Hapalova, Andrew Reidmeyer

    \brief
*/

#include <nanovdb_editor/putil/Raster.h>
#include <nanovdb_editor/putil/Editor.h>
#include <nanovdb_editor/putil/FileFormat.h>
#include <nanovdb_editor/putil/Raster.hpp>
#include <nanovdb_editor/putil/Reflect.h>

#include <nanovdb/io/IO.h>

#include <cstdarg>

#include <slang.h>

#define SLANG_PRELUDE_NAMESPACE CPPPrelude
#include <slang-cpp-types.h>

#include <argparse/argparse.hpp>

// #define TEST_COMPILER
// #define TEST_CPU_COMPILER         // TODO: test needs to check if slang-llvm is in the slang package
// #define TEST_EMPTY_COMPILER
// #define TEST_COMPUTE
#define TEST_EDITOR
// #define TEST_EDITOR_START_STOP
// #define TEST_RASTER
#define TEST_RASTER_2D
// #define TEST_SVRASTER
// #define TEST_E57

struct constants_t
{
    int magic_number;
    int pad1;
    int pad2;
    int pad3;
};

void pnanovdb_compute_log_print(pnanovdb_compute_log_level_t level, const char* format, ...)
{
    va_list args;
    va_start(args, format);

    const char* prefix = "Unknown";
    if (level == PNANOVDB_COMPUTE_LOG_LEVEL_ERROR)
    {
        prefix = "Error";
    }
    else if (level == PNANOVDB_COMPUTE_LOG_LEVEL_WARNING)
    {
        prefix = "Warning";
    }
    else if (level == PNANOVDB_COMPUTE_LOG_LEVEL_INFO)
    {
        prefix = "Info";
    }
    printf("%s: ", prefix);
    vprintf(format, args);
    printf("\n");

    va_end(args);
}

struct NanoVDBEditorArgs : public argparse::Args
{
};

int main(int argc, char* argv[])
{
    auto args = argparse::parse<NanoVDBEditorArgs>(argc, argv);

#if TEST_NODE2
    const char* nvdb_filepath = "./data/dragon_node2.nvdb";
#else
    const char* nvdb_filepath = "./data/dragon.nvdb";
#endif

    // uses dlopen to load compiler and get symbols
    pnanovdb_compiler_t compiler = {};
    pnanovdb_compiler_load(&compiler);

    // uses dlopen to load compute and get symbols
    pnanovdb_compute_t compute = {};
    pnanovdb_compute_load(&compute, &compiler);

    pnanovdb_compute_device_desc_t device_desc = {};
    // device_desc.log_print = pnanovdb_compute_log_print;

    pnanovdb_compute_device_manager_t* device_manager = compute.device_interface.create_device_manager(PNANOVDB_FALSE);
    pnanovdb_compute_device_t* device = compute.device_interface.create_device(device_manager, &device_desc);

#if defined(TEST_EDITOR) || defined(TEST_EDITOR_START_STOP)
    pnanovdb_compute_array_t* data_nanovdb = compute.load_nanovdb(nvdb_filepath);
    if (!data_nanovdb)
    {
        printf("Error: Could not load file '%s'\n", nvdb_filepath);
        return 1;
    }
#else
    pnanovdb_compute_array_t* data_nanovdb = nullptr;
#endif

    float voxel_size = 1.f / 128.f;

#ifdef TEST_RASTER
    const char* npy_file = "./data/splats.npz";
    pnanovdb_raster_t raster = {};
    pnanovdb_raster_load(&raster, &compute);

    pnanovdb_compute_queue_t* queue = compute.device_interface.get_compute_queue(device);

    pnanovdb_raster::raster_file(
        &raster, &compute, queue, npy_file, voxel_size, &data_nanovdb, nullptr, nullptr, nullptr, nullptr, nullptr);

    compute.save_nanovdb(data_nanovdb, "./data/splats.nvdb");

    pnanovdb_raster_free(&raster);
#endif

#ifdef TEST_SVRASTER
    const char* npy_file = "./data/svraster.npz";
    pnanovdb_raster_t raster = {};
    pnanovdb_raster_load(&raster, &compute);

    pnanovdb_compute_queue_t* queue = compute.device_interface.get_compute_queue(device);

    pnanovdb_raster::raster_file(
        &raster, &compute, queue, npy_file, voxel_size, &data_nanovdb, nullptr, nullptr, nullptr, nullptr, nullptr);

    compute.save_nanovdb(data_nanovdb, "./data/splats.nvdb");

    pnanovdb_raster_free(&raster);
#endif

#ifdef TEST_E57
#    define ARRAY_COUNT 3
    size_t runs = 10u;
    size_t totalTime = 0u;
    size_t array_size = 0u;

    const char* e57_file = "./data/kantonalschule/20042020-kantonalschule-_Setip_001.e57";
    const char* array_names[] = { "positions", "colors", "normals" };

    pnanovdb_fileformat_t fileformat = {};
    pnanovdb_fileformat_load(&fileformat, &compute);

    for (int i = 0; i < runs; ++i)
    {
        auto start = std::chrono::high_resolution_clock::now();

        pnanovdb_compute_array_t* arrays[ARRAY_COUNT] = {};
        pnanovdb_bool_t loaded = fileformat.load_file(e57_file, ARRAY_COUNT, array_names, arrays);

        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

        totalTime += duration.count();
        array_size = arrays[0]->element_count;

        float* positions = (float*)compute.map_array(arrays[0]);
        float* colors = (float*)compute.map_array(arrays[1]);

        for (int i = 0; i < 10; i++)
        {
            printf("[%d] position: %f, %f, %f\t", i, positions[i * 3], positions[i * 3 + 1], positions[i * 3 + 2]);
            printf("color: %f, %f, %f\n", colors[i * 3], colors[i * 3 + 1], colors[i * 3 + 2]);
        }
        compute.unmap_array(arrays[0]);
        compute.unmap_array(arrays[1]);

        for (int i = 0; i < ARRAY_COUNT; i++)
        {
            compute.destroy_array(arrays[i]);
        }

        // printf("Load time: %zu ms (%zu points)\n", (size_t)duration.count(), array_size / 3);
    }
    printf("E57 average load: %zu ms (%zu points)\n", totalTime / runs, array_size / 3);

    pnanovdb_fileformat_free(&fileformat);
    return 0;
#endif

#ifdef TEST_EDITOR
    // add editing optionally and late
    pnanovdb_editor_t editor = {};
    pnanovdb_editor_load(&editor, &compute, &compiler);

    pnanovdb_camera_state_t debug_state = {};
    pnanovdb_camera_state_default(&debug_state, PNANOVDB_FALSE);
    debug_state.position = { 0.632428, 0.930241, -0.005193 };
    debug_state.eye_direction = { -0.012344, 0.959868, -0.280182 };
    debug_state.eye_up = { 0.000000, 1.000000, 0.000000 };
    debug_state.eye_distance_from_position = -41.431084;

    pnanovdb_camera_config_t debug_config = {};
    pnanovdb_camera_config_default(&debug_config);
    debug_config.near_plane = 0.1f;
    debug_config.far_plane = 100.0f;
    debug_config.is_reverse_z = PNANOVDB_TRUE;

    pnanovdb_debug_camera_t debug_camera;
    pnanovdb_debug_camera_default(&debug_camera);
    debug_camera.name = "test";
    debug_camera.state = debug_state;
    debug_camera.config = debug_config;
    editor.add_debug_camera(&editor, &debug_camera);

#    ifdef TEST_RASTER
    pnanovdb_camera_t camera;
    pnanovdb_camera_init(&camera);
    // values are copy pasted from imgui.ini
    camera.state.position = { 3.663805, 3.287321, -0.607114 };
    camera.state.eye_direction = { -0.022936, 0.760122, -0.649375 };
    camera.state.eye_up = { 0.000000, 1.000000, 0.000000 };
    camera.state.eye_distance_from_position = -0.072589;
    editor.add_camera(&editor, &camera);
#    endif

#    ifdef TEST_RASTER_2D
    pnanovdb_camera_t camera;
    pnanovdb_camera_init(&camera);

    camera.state.position = { 0.358805, 0.725740, -0.693701 };
    camera.state.eye_direction = { -0.012344, 0.959868, -0.280182 };
    camera.state.eye_up = { 0.000000, 1.000000, 0.000000 };
    camera.state.eye_distance_from_position = -2.111028;
    editor.add_camera(&editor, &camera);

    const char* raster_file = "./data/splats.npz";
    pnanovdb_compute_queue_t* queue = compute.device_interface.get_compute_queue(device);

    pnanovdb_raster_t raster = {};
    pnanovdb_raster_load(&raster, &compute);

    const pnanovdb_reflect_data_type_t* data_type = PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_raster_shader_params_t);
    const pnanovdb_raster_shader_params_t* defaults = (const pnanovdb_raster_shader_params_t*)data_type->default_value;
    pnanovdb_raster_shader_params_t raster_params = *defaults;

#        ifdef TEST_RASTER_SHADER_PARAMS
    pnanovdb_compute_array_t* params_array =
        compute.create_array(raster_params.data_type->element_size, 1, (void*)&raster_params);
    pnanovdb_compute_array_t** shader_params_arrays = new pnanovdb_compute_array_t*[pnanovdb_raster::shader_param_count];
    for (pnanovdb_uint32_t idx = 0; idx < pnanovdb_raster::shader_param_count; idx++)
    {
        shader_params_arrays[idx] = nullptr;
    }

    shader_params_arrays[pnanovdb_raster::shader_param_count] = params_array;

    pnanovdb_raster::raster_file(&raster, &compute, queue, raster_file, 0.f, nullptr, &editor.gaussian_data,
                                 &editor.raster_ctx, shader_params_arrays, nullptr, nullptr);
#        else
    pnanovdb_raster::raster_file(&raster, &compute, queue, raster_file, 0.f, nullptr, &editor.gaussian_data,
                                 &editor.raster_ctx, nullptr, nullptr, nullptr);
#        endif

    editor.setup_shader_params(&editor, &raster_params, data_type);

    raster_params.eps2d = 0.5f;
    raster_params.near_plane_override = 1.f;

#        ifdef TEST_RASTER_SHADER_PARAMS
    compute.destroy_array(params_array);
    delete[] shader_params_arrays;
#        endif
#    endif

#    ifdef TEST_RASTER
    editor.add_nanovdb(&editor, data_nanovdb);
#    endif

    pnanovdb_editor_config_t config = {};
    config.headless = PNANOVDB_FALSE;
    config.streaming = PNANOVDB_FALSE;
    config.ip_address = "127.0.0.1";
    config.port = 8080;
    editor.show(&editor, device, &config);

    if (editor.camera)
    {
        pnanovdb_vec3_t position = editor.camera->state.position;
        printf("Camera position: %f, %f, %f\n", position.x, position.y, position.z);
    }

#    ifdef TEST_RASTER_2D
    pnanovdb_raster_free(&raster);
#    endif

    pnanovdb_editor_free(&editor);
#endif

#ifdef TEST_EDITOR_START_STOP
    pnanovdb_editor_t editor = {};
    pnanovdb_editor_load(&editor, &compute, &compiler);

    pnanovdb_editor_config_t config = {};
    config.headless = PNANOVDB_TRUE;
    config.streaming = PNANOVDB_TRUE;
    config.ip_address = "127.0.0.1";
    config.port = 8080;

    editor.start(&editor, device, &config);

    auto runEditorLoop = [](int iterations = 5)
    {
        for (int i = 0; i < iterations; ++i)
        {
            printf("Editor running... (%d/%d)\n", i + 1, iterations);
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    };

    runEditorLoop();

    editor.add_nanovdb(&editor, data_nanovdb);

    pnanovdb_camera_t camera;
    pnanovdb_camera_init(&camera);
    editor.add_camera(&editor, &camera);

    runEditorLoop(10);

    if (editor.camera)
    {
        pnanovdb_vec3_t position = editor.camera->state.position;
        printf("Camera position: %f, %f, %f\n", position.x, position.y, position.z);
    }

    editor.stop(&editor);

    pnanovdb_editor_free(&editor);
#endif

    compute.device_interface.destroy_device(device_manager, device);
    compute.device_interface.destroy_device_manager(device_manager);

    pnanovdb_compute_free(&compute);
    pnanovdb_compiler_free(&compiler);

    return 0;
}
