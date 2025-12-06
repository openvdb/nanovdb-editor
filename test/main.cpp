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
// #define TEST_RASTER_3D
#define TEST_RASTER_2D // editor needs to be have queue and device first
// #define TEST_SVRASTER
// #define TEST_E57
// #define TEST_CAMERA
// #define TEST_IMAGE2D

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
    else if (level == PNANOVDB_COMPUTE_LOG_LEVEL_DEBUG)
    {
        // prefix = "Debug";
        va_end(args);
        return;
    }
    printf("%s: ", prefix);
    vprintf(format, args);
    printf("\n");

    va_end(args);
}

struct NanoVDBEditorArgs : public argparse::Args
{
};

void test_raster_3d(pnanovdb_editor_t* editor,
                    pnanovdb_raster_t* raster,
                    pnanovdb_compute_t* compute,
                    pnanovdb_compute_queue_t* queue,
                    pnanovdb_editor_token_t* scene_token,
                    const char* npy_file,
                    float voxel_size,
                    const char* nanovdb_name,
                    const char* output_file = nullptr)
{
    printf("Testing raster with file: %s\n", npy_file);

    pnanovdb_compute_array_t* data_nanovdb = nullptr;

    raster->raster_file(raster, compute, queue, npy_file, voxel_size, &data_nanovdb, nullptr, nullptr, nullptr, nullptr,
                        nullptr, nullptr);

    if (output_file)
    {
        compute->save_nanovdb(data_nanovdb, output_file);
        printf("Raster result saved to: %s\n", output_file);
    }

    pnanovdb_editor_token_t* name_token = editor->get_token(nanovdb_name);
    editor->add_nanovdb_2(editor, scene_token, name_token, data_nanovdb);
}

void test_raster_2d(pnanovdb_editor_t* editor,
                    pnanovdb_fileformat_t* fileformat,
                    pnanovdb_compute_t* compute,
                    pnanovdb_editor_token_t* scene_token,
                    const char* raster_file,
                    const char* gaussian_name)
{
    printf("Testing raster2d with file: %s\n", raster_file);

    const char* array_names_gaussian[] = { "means", "opacities", "quaternions", "scales", "sh_0", "sh_n" };
    pnanovdb_compute_array_t* arrays[6] = {};

    pnanovdb_bool_t loaded = fileformat->load_file(raster_file, 6, array_names_gaussian, arrays);
    if (loaded == PNANOVDB_TRUE)
    {
        pnanovdb_editor_gaussian_data_desc_t desc = {};
        desc.means = arrays[0];
        desc.opacities = arrays[1];
        desc.quaternions = arrays[2];
        desc.scales = arrays[3];
        desc.sh_0 = arrays[4];
        desc.sh_n = arrays[5];

        pnanovdb_editor_token_t* gaussian_token = editor->get_token(gaussian_name);
        editor->add_gaussian_data_2(editor, scene_token, gaussian_token, &desc);

        printf("Gaussian data '%s' added to scene\n", gaussian_name);

        for (int ai = 0; ai < 6; ++ai)
        {
            if (arrays[ai])
            {
                compute->destroy_array(arrays[ai]);
            }
        }
    }
    else
    {
        printf("Warning: Failed to load Gaussian data from %s\n", raster_file);
    }
}

void test_image_2d(pnanovdb_editor_t* editor,
                   pnanovdb_compute_t* compute,
                   pnanovdb_editor_token_t* scene_token,
                   const char* image_name)
{
    printf("Testing image2d creation: %s\n", image_name);

    pnanovdb_uint32_t width = 1440;
    pnanovdb_uint32_t height = 720;
    pnanovdb_compute_array_t* image_rgba = compute->create_array(4u, width * height, nullptr);
    pnanovdb_uint32_t* mapped = (pnanovdb_uint32_t*)compute->map_array(image_rgba);
    for (pnanovdb_uint32_t j = 0u; j < height; j++)
    {
        for (pnanovdb_uint32_t i = 0u; i < width; i++)
        {
            pnanovdb_uint32_t val = 0u;
            val |= ((255 * i) / (width - 1));
            val |= (((255 * j) / (height - 1)) << 8u);
            val |= (255 << 24u);
            mapped[j * width + i] = val;
        }
    }
    compute->unmap_array(image_rgba);
    pnanovdb_compute_array_t* image_nanovdb = compute->nanovdb_from_image_rgba8(image_rgba, width, height);
    compute->destroy_array(image_rgba);

    pnanovdb_editor_token_t* image_token = editor->get_token(image_name);
    editor->add_nanovdb_2(editor, scene_token, image_token, image_nanovdb);
    compute->destroy_array(image_nanovdb);

    pnanovdb_editor_shader_name_t* mapped_shader = (pnanovdb_editor_shader_name_t*)editor->map_params(
        editor, scene_token, image_token, PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_editor_shader_name_t));
    if (mapped_shader)
    {
        mapped_shader->shader_name = editor->get_token("editor/image2d.slang");
        editor->unmap_params(editor, scene_token, image_token);
    }

    printf("Image2d '%s' added to scene\n", image_name);
}

#define TEST_LARGE_BUFFER

void test_large_buffer(pnanovdb_compute_t* compute,
                      pnanovdb_compute_device_t* device)
{
    pnanovdb_raster_t raster = {};
    pnanovdb_raster_load(&raster, compute);

    pnanovdb_uint64_t element_count = 1024llu * 1024llu * 1024llu;

    pnanovdb_compute_array_t* arr = compute->create_array(8u, element_count, nullptr);
    pnanovdb_uint64_t* mapped = (pnanovdb_uint64_t*)compute->map_array(arr);
    for (pnanovdb_uint64_t idx = 0u; idx < element_count; idx++)
    {
        mapped[idx] = idx;
    }
    compute->unmap_array(arr);

    pnanovdb_compute_queue_t* queue = compute->device_interface.get_device_queue(device);

    pnanovdb_uint64_t mismatch_count = 0u;

    pnanovdb_compute_array_t* dst = raster.upload_and_readback_array(compute, queue, arr);
    mapped = (pnanovdb_uint64_t*)compute->map_array(dst);
    for (pnanovdb_uint64_t idx = 0u; idx < element_count; idx++)
    {
        if (mapped[idx] != idx)
        {
            mismatch_count++;
        }
        if (element_count - idx < 16u)
        {
            printf("mapped[%zu] = %zu\n", idx, mapped[idx]);
        }
    }
    compute->unmap_array(dst);

    printf("test_larger_buffer mismatch_count(%zu) of count(%zu)\n", mismatch_count, element_count);

    compute->destroy_array(arr);
    compute->destroy_array(dst);

    pnanovdb_raster_free(&raster);
}

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
    device_desc.log_print = pnanovdb_compute_log_print;
    device_desc.device_index = 1u;

    pnanovdb_compute_device_manager_t* device_manager = compute.device_interface.create_device_manager(PNANOVDB_FALSE);
    pnanovdb_compute_device_t* device = compute.device_interface.create_device(device_manager, &device_desc);

    pnanovdb_fileformat_t fileformat = {};
    pnanovdb_fileformat_load(&fileformat, &compute);

#ifdef TEST_LARGE_BUFFER
    test_large_buffer(&compute, device);
    return 0;
#endif

#if defined(TEST_EDITOR) || defined(TEST_EDITOR_START_STOP)
    pnanovdb_compute_array_t* data_nanovdb = compute.load_nanovdb(nvdb_filepath);
    if (!data_nanovdb)
    {
        printf("Error: Could not load file '%s'\n", nvdb_filepath);
        // return 1;
    }
#else
    pnanovdb_compute_array_t* data_nanovdb = nullptr;
#endif

    float voxel_size = 1.f / 128.f;

#ifdef TEST_SVRASTER
    const char* npy_file = "./data/svraster.npz";
    pnanovdb_raster_t raster = {};
    pnanovdb_raster_load(&raster, &compute);

    pnanovdb_compute_queue_t* queue = compute.device_interface.get_compute_queue(device);

    raster.raster_file
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
    return 0;
#endif

#ifdef TEST_EDITOR
    // add editing optionally and late
    pnanovdb_editor_t editor = {};
    pnanovdb_editor_load(&editor, &compute, &compiler);

    pnanovdb_editor_token_t* scene_main = editor.get_token("main");
    pnanovdb_editor_token_t* scene_secondary = editor.get_token("secondary_scene");

    pnanovdb_editor_token_t* volume_token = editor.get_token("dragon");
    editor.add_nanovdb_2(&editor, scene_main, volume_token, data_nanovdb);
    editor.add_nanovdb_2(&editor, scene_secondary, volume_token, data_nanovdb);
    printf("Added dragon volume to main scene using add_nanovdb()\n");

    pnanovdb_compute_array_t* data_nanovdb2 = compute.load_nanovdb("./data/hexagon_flow_test2.nvdb");
    if (data_nanovdb2)
    {
        pnanovdb_editor_token_t* flow_token = editor.get_token("flow_volume");
        editor.add_nanovdb_2(&editor, scene_main, flow_token, data_nanovdb2);
        printf("Added flow volume to main using add_nanovdb()\n");
    }

#    ifdef TEST_CAMERA
    pnanovdb_camera_config_t default_config = {};
    pnanovdb_camera_config_default(&default_config);
    default_config.near_plane = 0.1f;
    default_config.far_plane = 100.0f;

    pnanovdb_camera_state_t default_state = {};
    pnanovdb_camera_state_default(&default_state, PNANOVDB_FALSE);

    pnanovdb_camera_state_t debug_state = {};
    pnanovdb_camera_state_default(&debug_state, PNANOVDB_FALSE);
    debug_state.position = { 0.632428, -0.930241, -0.005193 };
    debug_state.eye_direction = { -0.012344, 0.959868, -0.280182 };
    debug_state.eye_up = { 0.000000, 1.000000, 0.000000 };
    debug_state.eye_distance_from_position = 41.431084;

    pnanovdb_camera_config_t debug_config = {};
    pnanovdb_camera_config_default(&debug_config);
    debug_config.near_plane = 0.1f;
    debug_config.far_plane = 100.0f;
    debug_config.aspect_ratio = 2.0f;

    pnanovdb_camera_state_t test_state = default_state;
    test_state.eye_distance_from_position = 3.f;
    test_state.eye_up = { 0.000000, 0.000000, -1.000000 };
    pnanovdb_camera_config_t test_config = default_config;

    pnanovdb_camera_view_t test_camera;
    pnanovdb_camera_view_default(&test_camera);
    test_camera.name = editor.get_token("test");
    test_camera.num_cameras = 1;
    test_camera.states = new pnanovdb_camera_state_t[test_camera.num_cameras];
    test_camera.states[0] = test_state;
    test_camera.configs = new pnanovdb_camera_config_t[test_camera.num_cameras];
    test_camera.configs[0] = test_config;
    test_camera.is_visible = PNANOVDB_FALSE;
    editor.add_camera_view_2(&editor, scene_main, &test_camera);

    pnanovdb_camera_view_t debug_camera;
    pnanovdb_camera_view_default(&debug_camera);
    debug_camera.name = editor.get_token("test_10");
    debug_camera.num_cameras = 10;
    debug_camera.states = new pnanovdb_camera_state_t[debug_camera.num_cameras];
    debug_camera.configs = new pnanovdb_camera_config_t[debug_camera.num_cameras];
    debug_camera.is_visible = PNANOVDB_FALSE;

    for (int i = 0; i < debug_camera.num_cameras; ++i)
    {
        pnanovdb_camera_state_t debug_state_i = debug_state;
        debug_state_i.position.x += 50.f * i;
        debug_state_i.position.z -= 20.f * i;
        debug_camera.states[i] = debug_state_i;
        debug_camera.configs[i] = debug_config;
    }
    editor.add_camera_view_2(&editor, scene_main, &debug_camera);

    pnanovdb_camera_view_t default_camera;
    pnanovdb_camera_view_default(&default_camera);
    default_camera.name = editor.get_token("test_default");
    default_camera.num_cameras = 1;
    default_camera.states = new pnanovdb_camera_state_t[default_camera.num_cameras];
    default_camera.states[0] = default_state;
    default_camera.configs = new pnanovdb_camera_config_t[default_camera.num_cameras];
    default_camera.configs[0] = default_config;
    default_camera.is_visible = PNANOVDB_FALSE;
    editor.add_camera_view_2(&editor, scene_main, &default_camera);
#    endif

#    ifdef TEST_RASTER_3D
    pnanovdb_camera_t camera;
    pnanovdb_camera_init(&camera);
    // values are copy pasted from imgui.ini
    camera.state.position = { 3.663805, 3.287321, -0.607114 };
    camera.state.eye_direction = { -0.022936, 0.760122, -0.649375 };
    camera.state.eye_up = { 0.000000, 1.000000, 0.000000 };
    camera.state.eye_distance_from_position = -0.072589;
    editor.update_camera_2(&editor, &camera);

    pnanovdb_camera_t camera;
    pnanovdb_camera_init(&camera);

    const char* raster_file = "./data/ficus.ply";
    const char* raster_file_garden = "./data/garden.ply";

    // Test Ficus
    test_raster_2d(&editor, &fileformat, &compute, scene_main, raster_file, "ficus");

    // Test Garden
    test_raster_2d(&editor, &fileformat, &compute, scene_secondary, raster_file_garden, "garden");
#    endif

#    ifdef TEST_RASTER_3D
    const char* npy_file = "./data/splats.npz";
    pnanovdb_raster_t raster = {};
    pnanovdb_raster_load(&raster, &compute);

    pnanovdb_compute_queue_t* queue = compute.device_interface.get_compute_queue(device);

    test_raster_3d(&editor, &raster, &compute, queue, scene_token, npy_file, voxel_size, npy_file);

    pnanovdb_raster_free(&raster);
#    endif

#    ifdef TEST_IMAGE2D
    test_image_2d(&editor, &compute, scene_main, "image2d");
#    endif

    pnanovdb_editor_config_t config = {};
    config.headless = PNANOVDB_FALSE;
    config.streaming = PNANOVDB_FALSE;
    config.ip_address = "127.0.0.1";
    config.port = 8080;
    // config.ui_profile_name = "viewer";
    editor.show(&editor, device, &config);

    // if (editor.camera)
    // {
    //     pnanovdb_vec3_t position = editor.camera->state.position;
    //     printf("Camera position: %f, %f, %f\n", position.x, position.y, position.z);
    // }

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

    pnanovdb_int32_t port = editor.get_resolved_port(&editor, PNANOVDB_TRUE);
    printf("Editor starting on port: %d\n", port);

    auto runEditorLoop = [](int iterations = 5)
    {
        for (int i = 0; i < iterations; ++i)
        {
            printf("Editor running... (%d/%d)\n", i + 1, iterations);
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    };

    runEditorLoop();

    pnanovdb_editor_token_t* scene_token = editor.get_token("scene");
    pnanovdb_editor_token_t* main_token = editor.get_token("main");
    pnanovdb_editor_token_t* volume_token = editor.get_token("dragon");

    editor.add_nanovdb_2(&editor, main_token, volume_token, data_nanovdb);

    compute.destroy_array(data_nanovdb);

#    ifdef TEST_IMAGE2D
    test_image_2d(&editor, &compute, scene_token, "image2d");
#    endif

#    ifdef TEST_RASTER_2D
    const char* raster_file = "./data/ficus.ply";
    test_raster_2d(&editor, &fileformat, &compute, scene_token, raster_file, "ficus");
#    endif

#    ifdef TEST_RASTER_3D
    const char* npy_file = "./data/splats.npz";
    pnanovdb_raster_t raster = {};
    pnanovdb_raster_load(&raster, &compute);

    pnanovdb_compute_queue_t* queue = compute.device_interface.get_compute_queue(device);

    test_raster_3d(&editor, &raster, &compute, queue, scene_token, npy_file, voxel_size, npy_file, "splats");

    pnanovdb_raster_free(&raster);
#    endif

    editor.wait_for_interrupt(&editor);

    editor.stop(&editor);

    pnanovdb_editor_free(&editor);
#endif

    compute.device_interface.destroy_device(device_manager, device);
    compute.device_interface.destroy_device_manager(device_manager);

    pnanovdb_fileformat_free(&fileformat);
    pnanovdb_compute_free(&compute);
    pnanovdb_compiler_free(&compiler);

    return 0;
}
