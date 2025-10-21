// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

#include <nanovdb_editor/putil/Editor.h>
#include <nanovdb_editor/putil/FileFormat.h>

#include <cstdarg>
#include <thread>
#include <chrono>

#define TEST_EDITOR
// #define TEST_RASTER
#define TEST_RASTER_2D
#define TEST_CAMERA
#define TEST_NVDB
// #define TEST_FILE_FORMAT
// #define FORMAT_INGP
#define FORMAT_PLY
// #define TEST_TRAINIG

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

int main(int argc, char* argv[])
{
    pnanovdb_compute_array_t* data_nanovdb = nullptr;

    pnanovdb_compiler_t compiler = {};
    pnanovdb_compiler_load(&compiler);

    pnanovdb_compute_t compute = {};
    pnanovdb_compute_load(&compute, &compiler);

    pnanovdb_compute_device_desc_t device_desc = {};
    // device_desc.log_print = pnanovdb_compute_log_print;

    pnanovdb_compute_device_manager_t* device_manager = compute.device_interface.create_device_manager(PNANOVDB_FALSE);
    pnanovdb_compute_device_t* device = compute.device_interface.create_device(device_manager, &device_desc);

#if defined(TEST_RASTER) || defined(TEST_RASTER_2D)
    pnanovdb_compute_queue_t* queue = compute.device_interface.get_compute_queue(device);

    pnanovdb_raster_t raster = {};
    pnanovdb_raster_load(&raster, &compute);
    pnanovdb_raster_context_t* raster_context = nullptr;
#endif

#ifdef TEST_RASTER
    float voxel_size = 1.f / 128.f;
    const char* raster_to_nvdb_file = "../../data/splats.npz";

    raster.raster_file(&raster, &compute, queue, raster_to_nvdb_file, voxel_size, &data_nanovdb, nullptr,
                       &raster_context, nullptr, nullptr, nullptr, nullptr);

    if (data_nanovdb)
    {
        compute.save_nanovdb(data_nanovdb, "../../data/splats.nvdb");
    }

    pnanovdb_raster_free(&raster);
#endif

#ifdef TEST_FILE_FORMAT

#    if defined(FORMAT_INGP)
    const char* filename = "../../data/ficus-30k.ingp";
    const char* array_names[] = { "mog_positions", "mog_densities", "mog_rotations", "mog_scales", "mog_features" };
#    elif defined(FORMAT_PLY)
    const char* filename = "../../data/ficus.ply";
    const char* array_names[] = { "means", "opacities", "quaternions", "scales", "sh" };
#    else
#        error "No valid format defined"
#    endif

#    define ARRAY_COUNT 5
    pnanovdb_compute_array_t* arrays[ARRAY_COUNT] = {};

    pnanovdb_fileformat_t fileformat = {};
    pnanovdb_fileformat_load(&fileformat, &compute);
    pnanovdb_bool_t loaded = fileformat.load_file(filename, ARRAY_COUNT, array_names, arrays);
    if (loaded == PNANOVDB_TRUE)
    {
        printf("Successfully loaded file with arrays:\n");
        for (int i = 0; i < ARRAY_COUNT; i++)
        {
            if (arrays[i])
            {
                printf("  %s: %llu elements of size %llu\n", array_names[i], arrays[i]->element_count,
                       arrays[i]->element_size);
                compute.destroy_array(arrays[i]);
            }
        }
    }
    else
    {
        printf("Failed to load file '%s'\n", filename);
    }

    pnanovdb_fileformat_free(&fileformat);
    return 0;
#endif

#ifdef TEST_EDITOR
    pnanovdb_editor_t editor = {};
    pnanovdb_editor_load(&editor, &compute, &compiler);

    // ===== New Token-Based Multi-Scene API =====
    printf("\n=== Creating Multiple Scenes with Token API ===\n");

    // Create scene tokens
    pnanovdb_editor_token_t* scene_main = editor.get_token("main_scene");
    pnanovdb_editor_token_t* scene_test = editor.get_token("test_scene");
    printf("Created scenes: '%s' (id=%llu), '%s' (id=%llu)\n", scene_main->str, (unsigned long long)scene_main->id,
           scene_test->str, (unsigned long long)scene_test->id);

    // Add initial data to main scene if available
    if (data_nanovdb)
    {
        pnanovdb_editor_token_t* splats_token = editor.get_token("splats");
        editor.add_nanovdb_2(&editor, scene_main, splats_token, data_nanovdb);
        printf("Added splats to main_scene\n");
    }

    pnanovdb_editor_config_t config = {};
    config.headless = PNANOVDB_TRUE;
    config.streaming = PNANOVDB_TRUE;
    config.ip_address = "192.168.0.6";
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

    runEditorLoop(2);

#    ifdef TEST_CAMERA
    pnanovdb_camera_state_t debug_state = {};
    pnanovdb_camera_state_default(&debug_state, PNANOVDB_FALSE);
    debug_state.position = { 0.632428, 0.930241, -0.005193 };
    debug_state.eye_direction = { -0.012344, 0.959868, -0.280182 };
    debug_state.eye_up = { 0.000000, 1.000000, 0.000000 };
    debug_state.eye_distance_from_position = 41.431084;

    pnanovdb_camera_config_t debug_config = {};
    pnanovdb_camera_config_default(&debug_config);
    debug_config.near_plane = 0.1f;
    debug_config.far_plane = 100.0f;

    pnanovdb_camera_view_t debug_camera;
    pnanovdb_camera_view_default(&debug_camera);
    debug_camera.name = editor.get_token("test_10");
    debug_camera.num_cameras = 10;
    debug_camera.is_visible = PNANOVDB_FALSE;
    debug_camera.states = new pnanovdb_camera_state_t[debug_camera.num_cameras];
    debug_camera.configs = new pnanovdb_camera_config_t[debug_camera.num_cameras];

    for (int i = 0; i < debug_camera.num_cameras; ++i)
    {
        pnanovdb_camera_state_t debug_state_i = debug_state;
        debug_state_i.position.x += 50.f * i;
        debug_state_i.position.z -= 20.f * i;
        debug_camera.states[i] = debug_state_i;
        debug_camera.configs[i] = debug_config;
    }
    editor.add_camera_view(&editor, &debug_camera);

    pnanovdb_camera_config_t default_config = {};
    pnanovdb_camera_config_default(&default_config);
    default_config.near_plane = 0.1f;
    default_config.far_plane = 100.0f;

    pnanovdb_camera_state_t default_state = {};
    pnanovdb_camera_state_default(&default_state, PNANOVDB_FALSE);

    pnanovdb_camera_view_t default_camera;
    pnanovdb_camera_view_default(&default_camera);
    default_camera.name = editor.get_token("default");
    default_camera.num_cameras = 1;
    default_camera.states = new pnanovdb_camera_state_t[default_camera.num_cameras];
    default_camera.states[0] = default_state;
    default_camera.configs = new pnanovdb_camera_config_t[default_camera.num_cameras];
    default_camera.configs[0] = default_config;
    editor.add_camera_view(&editor, &default_camera);
#    endif

#    ifdef TEST_NVDB
    printf("\n=== Adding NanoVDB Volumes to Multiple Scenes ===\n");

    // Load and add dragon to both scenes
    data_nanovdb = compute.load_nanovdb("../../data/dragon.nvdb");
    if (data_nanovdb)
    {
        pnanovdb_editor_token_t* dragon_token = editor.get_token("dragon");
        editor.add_nanovdb_2(&editor, scene_main, dragon_token, data_nanovdb);
        printf("Added dragon to main_scene\n");

        editor.add_nanovdb_2(&editor, scene_test, dragon_token, data_nanovdb);
        printf("Added dragon to test_scene (shared volume)\n");
    }

    // Load and add splats to test scene only
    pnanovdb_compute_array_t* data_nanovdb2 = compute.load_nanovdb("../../data/splats.nvdb");
    if (data_nanovdb2)
    {
        pnanovdb_editor_token_t* splats_token = editor.get_token("splats_volume");
        editor.add_nanovdb_2(&editor, scene_test, splats_token, data_nanovdb2);
        printf("Added splats to test_scene only\n");
    }

    printf("Scene Summary:\n");
    printf("  main_scene: dragon\n");
    printf("  test_scene: dragon + splats_volume\n");
#    endif

#    ifdef TEST_RASTER_2D
    printf("\n=== Adding Gaussian Data to Multiple Scenes ===\n");

    pnanovdb_camera_t camera;
    pnanovdb_camera_init(&camera);

    camera.state.position = { 0.358805, 0.725740, -0.693701 };
    camera.state.eye_direction = { -0.012344, 0.959868, -0.280182 };
    camera.state.eye_up = { 0.000000, 1.000000, 0.000000 };
    camera.state.eye_distance_from_position = -2.111028;
    editor.update_camera(&editor, &camera);

    const char* raster_file = "../../data/ficus.ply";
    const char* raster_file_garden = "../../data/garden.ply";

    // Get tokens for gaussian objects
    pnanovdb_editor_token_t* ficus_token = editor.get_token("ficus");
    pnanovdb_editor_token_t* garden_token = editor.get_token("garden");

    const pnanovdb_reflect_data_type_t* data_type = PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_raster_shader_params_t);
    const pnanovdb_raster_shader_params_t* defaults = (const pnanovdb_raster_shader_params_t*)data_type->default_value;
    pnanovdb_raster_shader_params_t raster_params = *defaults;
    raster_params.data_type = data_type;
    raster_params.name = "ficus";
    pnanovdb_raster_shader_params_t raster_params_garden = *defaults;
    raster_params_garden.data_type = data_type;
    raster_params_garden.name = "garden";

    pnanovdb_compute_array_t* raster2d_shader_params_array = new pnanovdb_compute_array_t();
    raster2d_shader_params_array->element_count = 1u;
    raster2d_shader_params_array->element_size = data_type->element_size;
    raster2d_shader_params_array->data = (void*)&raster_params;

    pnanovdb_compute_array_t* raster2d_shader_params_array_garden = new pnanovdb_compute_array_t();
    raster2d_shader_params_array_garden->element_count = 1u;
    raster2d_shader_params_array_garden->element_size = data_type->element_size;
    raster2d_shader_params_array_garden->data = (void*)&raster_params_garden;

    pnanovdb_raster_gaussian_data_t* gaussian_data = nullptr;
    pnanovdb_raster_gaussian_data_t* gaussian_data_garden = nullptr;
    pnanovdb_raster_context_t* raster_ctx = nullptr;

#        ifdef TEST_TRAINIG
    int N = 100;
#        else
    int N = 1;
#        endif
    for (int i = 0; i < N; i++)
    {
        pnanovdb_raster_gaussian_data_t* gaussian_data_old = gaussian_data;
        raster.raster_file(&raster, &compute, queue, raster_file, 0.f, nullptr, &gaussian_data, &raster_ctx, nullptr,
                           &raster_params, nullptr, nullptr);

        // Using old API - will be added to default scene
        // For multi-scene: could use add_gaussian_data_2 with descriptor API when fully implemented
        editor.add_gaussian_data(&editor, raster_ctx, queue, gaussian_data);
        printf("Added ficus gaussian data (iteration %d)\n", i + 1);
        raster.destroy_gaussian_data(raster.compute, queue, gaussian_data_old);

        runEditorLoop(5);

        gaussian_data_old = gaussian_data_garden;
        raster.raster_file(&raster, &compute, queue, raster_file_garden, 0.f, nullptr, &gaussian_data_garden,
                           &raster_ctx, nullptr, &raster_params_garden, nullptr, nullptr);

        editor.add_gaussian_data(&editor, raster_ctx, queue, gaussian_data_garden);
        printf("Added garden gaussian data (iteration %d)\n", i + 1);
        raster.destroy_gaussian_data(raster.compute, queue, gaussian_data_old);

        runEditorLoop(5);
    }

    printf("\n=== Gaussian Data Summary ===\n");
    printf("Note: Gaussian data uses old API (add_gaussian_data) which tracks in default scene\n");
    printf("For explicit multi-scene gaussian support, use add_gaussian_data_2 with descriptor API\n");

    raster_params.eps2d = 0.5f;
    printf("Updating shader param eps2d to %f\n", raster_params.eps2d);
    editor.sync_shader_params(&editor, &raster_params, PNANOVDB_TRUE);

    raster_params_garden.sh_degree_override = 3;
    editor.sync_shader_params(&editor, &raster_params_garden, PNANOVDB_TRUE);
    printf("Updating shader param sh_degree to %d\n", raster_params_garden.sh_degree_override);

    default_camera.is_visible = PNANOVDB_FALSE;

    runEditorLoop(5);

    editor.sync_shader_params(&editor, &raster_params, PNANOVDB_FALSE);

    printf("Updated shader params:\n");
    printf("eps2d: %f\n", raster_params.eps2d);

    editor.sync_shader_params(&editor, &raster_params_garden, PNANOVDB_FALSE);

    printf("Updated shader params:\n");
    printf("sh_degree: %d\n", raster_params_garden.sh_degree_override);
#    endif

    printf("\n=== Multi-Scene Test Complete ===\n");
    printf("Scenes created:\n");
    printf("  1. main_scene (id=%llu)\n", (unsigned long long)scene_main->id);
    printf("  2. test_scene (id=%llu)\n", (unsigned long long)scene_test->id);
    printf("\nUse the Scene dropdown in the streaming UI to switch between scenes!\n");
    printf("Running editor loop for 1000 iterations...\n\n");

    runEditorLoop(1000);

    // editor.show(&editor, device, &config);

    pnanovdb_editor_free(&editor);
#else
    if (data_nanovdb)
    {
        compute.destroy_array(data_nanovdb);
    }
#endif

#ifdef TEST_RASTER_2D
    raster.destroy_gaussian_data(raster.compute, queue, gaussian_data);
    gaussian_data = nullptr;
    raster.destroy_gaussian_data(raster.compute, queue, gaussian_data_garden);
    gaussian_data_garden = nullptr;
    raster.destroy_context(raster.compute, queue, raster_ctx);
    raster_ctx = nullptr;
#endif

#if defined(TEST_RASTER) || defined(TEST_RASTER_2D)
    pnanovdb_raster_free(&raster);
#endif

    compute.device_interface.destroy_device(device_manager, device);
    compute.device_interface.destroy_device_manager(device_manager);

    pnanovdb_compute_free(&compute);

    return 0;
}
