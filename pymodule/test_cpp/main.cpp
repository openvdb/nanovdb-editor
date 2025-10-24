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
    pnanovdb_compute_array_t* data_nanovdb2 = nullptr;

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

    // Create scene tokens
    pnanovdb_editor_token_t* scene_main = editor.get_token("main_scene");
    pnanovdb_editor_token_t* scene_secondary = editor.get_token("secondary_scene");

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
    editor.add_camera_view_2(&editor, scene_secondary, &debug_camera);
    editor.add_camera_view_2(&editor, scene_main, &debug_camera);

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
    editor.add_camera_view_2(&editor, scene_main, &default_camera);
    editor.add_camera_view_2(&editor, scene_secondary, &default_camera);
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

        editor.add_nanovdb_2(&editor, scene_main, dragon_token, data_nanovdb);
        printf("Added dragon to test_scene (shared volume)\n");
    }

    // Load and add flow volume to secondary scene only
    data_nanovdb2 = compute.load_nanovdb("../../data/hexagon_flow_test2.nvdb");
    if (data_nanovdb2)
    {
        pnanovdb_editor_token_t* flow_token = editor.get_token("flow_volume");
        editor.add_nanovdb_2(&editor, scene_secondary, flow_token, data_nanovdb2);
    }

    printf("Scene Summary:\n");
    printf("  main_scene: dragon\n");
    printf("  secondary_scene: dragon + flow_volume\n");
#    endif

#    ifdef TEST_RASTER_2D
    printf("\n=== Adding Gaussian Data to Multiple Scenes ===\n");

    pnanovdb_camera_t camera;
    pnanovdb_camera_init(&camera);

    camera.state.position = { 0.358805, 0.725740, -0.693701 };
    camera.state.eye_direction = { -0.012344, 0.959868, -0.280182 };
    camera.state.eye_up = { 0.000000, 1.000000, 0.000000 };
    camera.state.eye_distance_from_position = -2.111028;
    editor.update_camera_2(&editor, scene_main, &camera);

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

    pnanovdb_raster_gaussian_data_t* gaussian_data = nullptr;
    pnanovdb_raster_gaussian_data_t* gaussian_data_garden = nullptr;
    pnanovdb_raster_context_t* raster_ctx = nullptr;

    pnanovdb_compute_array_t* arrays[6] = {};
    pnanovdb_fileformat_t fileformat = {};
    pnanovdb_fileformat_load(&fileformat, &compute);

#        ifdef TEST_TRAINIG
    int N = 100;
#        else
    int N = 1;
#        endif
    for (int i = 0; i < N; i++)
    {
        // Load Gaussian arrays from file and add via token-based descriptor API
        const char* array_names_gaussian[] = { "means", "opacities", "quaternions", "scales", "sh_0", "sh_n" };

        // Ficus
        {
            pnanovdb_bool_t loaded = fileformat.load_file(raster_file, 6, array_names_gaussian, arrays);
            if (loaded == PNANOVDB_TRUE)
            {
                pnanovdb_editor_gaussian_data_desc_t desc = {};
                desc.means = arrays[0];
                desc.opacities = arrays[1];
                desc.quaternions = arrays[2];
                desc.scales = arrays[3];
                desc.sh_0 = arrays[4];
                desc.sh_n = arrays[5];

                editor.add_gaussian_data_2(&editor, scene_main, ficus_token, &desc);

                for (int ai = 0; ai < 6; ++ai)
                {
                    if (arrays[ai])
                    {
                        compute.destroy_array(arrays[ai]);
                    }
                }
            }
        }

        pnanovdb_raster_shader_params_t* ficus_params =
            (pnanovdb_raster_shader_params_t*)editor.map_params(&editor, scene_main, ficus_token, data_type);
        if (ficus_params)
        {
            ficus_params->eps2d = 0.5f;
            printf("Updating shader param eps2d to %f\n", ficus_params->eps2d);
            editor.unmap_params(&editor, scene_main, ficus_token);
        }

        // Verify the parameter was updated
        ficus_params = (pnanovdb_raster_shader_params_t*)editor.map_params(&editor, scene_main, ficus_token, data_type);
        if (ficus_params)
        {
            printf("Verifying ficus eps2d parameter: %f (expected 0.5)\n", ficus_params->eps2d);
            if (ficus_params->eps2d != 0.5f)
            {
                printf("ERROR: Parameter verification failed! eps2d = %f, expected 0.5\n", ficus_params->eps2d);
            }
            editor.unmap_params(&editor, scene_main, ficus_token);
        }

        runEditorLoop(5);

        // Garden
        {
            pnanovdb_bool_t loaded = fileformat.load_file(raster_file_garden, 6, array_names_gaussian, arrays);
            if (loaded == PNANOVDB_TRUE)
            {
                pnanovdb_editor_gaussian_data_desc_t desc = {};
                desc.means = arrays[0];
                desc.opacities = arrays[1];
                desc.quaternions = arrays[2];
                desc.scales = arrays[3];
                desc.sh_0 = arrays[4];
                desc.sh_n = arrays[5];

                editor.add_gaussian_data_2(&editor, scene_secondary, garden_token, &desc);
                printf("Added garden gaussian data to secondary_scene (iteration %d)\n", i + 1);

                for (int ai = 0; ai < 6; ++ai)
                {
                    if (arrays[ai])
                    {
                        compute.destroy_array(arrays[ai]);
                    }
                }
            }
        }

        pnanovdb_raster_shader_params_t* garden_params =
            (pnanovdb_raster_shader_params_t*)editor.map_params(&editor, scene_secondary, garden_token, data_type);
        if (garden_params)
        {
            garden_params->sh_degree_override = 3;
            printf("Updating shader param sh_degree to %d\n", garden_params->sh_degree_override);
            editor.unmap_params(&editor, scene_secondary, garden_token);
        }

        // Verify the parameter was updated
        garden_params =
            (pnanovdb_raster_shader_params_t*)editor.map_params(&editor, scene_secondary, garden_token, data_type);
        if (garden_params)
        {
            printf("Verifying garden sh_degree_override parameter: %d (expected 3)\n", garden_params->sh_degree_override);
            if (garden_params->sh_degree_override != 3)
            {
                printf("ERROR: Parameter verification failed! sh_degree_override = %d, expected 3\n",
                       garden_params->sh_degree_override);
            }
            editor.unmap_params(&editor, scene_secondary, garden_token);
        }

        runEditorLoop(5);
    }

    // Update shader params using map/unmap API (no explicit sync required)
    {
        pnanovdb_raster_shader_params_t* ficus_params =
            (pnanovdb_raster_shader_params_t*)editor.map_params(&editor, scene_main, ficus_token, data_type);
        if (ficus_params)
        {
            ficus_params->eps2d = 0.5f;
            printf("Updating shader param eps2d to %f\n", ficus_params->eps2d);
            editor.unmap_params(&editor, scene_main, ficus_token);
        }

        // Verify the parameter was updated
        ficus_params = (pnanovdb_raster_shader_params_t*)editor.map_params(&editor, scene_main, ficus_token, data_type);
        if (ficus_params)
        {
            printf("Verifying ficus eps2d parameter: %f (expected 0.5)\n", ficus_params->eps2d);
            if (ficus_params->eps2d != 0.5f)
            {
                printf("ERROR: Parameter verification failed! eps2d = %f, expected 0.5\n", ficus_params->eps2d);
            }
            editor.unmap_params(&editor, scene_main, ficus_token);
        }

        pnanovdb_raster_shader_params_t* garden_params =
            (pnanovdb_raster_shader_params_t*)editor.map_params(&editor, scene_secondary, garden_token, data_type);
        if (garden_params)
        {
            garden_params->sh_degree_override = 3;
            printf("Updating shader param sh_degree to %d\n", garden_params->sh_degree_override);
            editor.unmap_params(&editor, scene_secondary, garden_token);
        }

        // Verify the parameter was updated
        garden_params =
            (pnanovdb_raster_shader_params_t*)editor.map_params(&editor, scene_secondary, garden_token, data_type);
        if (garden_params)
        {
            printf("Verifying garden sh_degree_override parameter: %d (expected 3)\n", garden_params->sh_degree_override);
            if (garden_params->sh_degree_override != 3)
            {
                printf("ERROR: Parameter verification failed! sh_degree_override = %d, expected 3\n",
                       garden_params->sh_degree_override);
            }
            editor.unmap_params(&editor, scene_secondary, garden_token);
        }
    }

    default_camera.is_visible = PNANOVDB_FALSE;

    runEditorLoop(5);

    // Read back and print current params via map/unmap (demonstrates round-trip and state persistence)
    {
        pnanovdb_raster_shader_params_t* ficus_params =
            (pnanovdb_raster_shader_params_t*)editor.map_params(&editor, scene_main, ficus_token, data_type);
        if (ficus_params)
        {
            printf("Ficus current shader params:\n");
            printf("  eps2d: %f (expected 0.5)\n", ficus_params->eps2d);
            if (ficus_params->eps2d != 0.5f)
            {
                printf("ERROR: Parameter state not persisted! eps2d = %f, expected 0.5\n", ficus_params->eps2d);
            }
            else
            {
                printf("  State verification: PASSED\n");
            }
            editor.unmap_params(&editor, scene_main, ficus_token);
        }

        pnanovdb_raster_shader_params_t* garden_params =
            (pnanovdb_raster_shader_params_t*)editor.map_params(&editor, scene_secondary, garden_token, data_type);
        if (garden_params)
        {
            printf("Garden current shader params:\n");
            printf("  sh_degree_override: %d (expected 3)\n", garden_params->sh_degree_override);
            if (garden_params->sh_degree_override != 3)
            {
                printf("ERROR: Parameter state not persisted! sh_degree_override = %d, expected 3\n",
                       garden_params->sh_degree_override);
            }
            else
            {
                printf("  State verification: PASSED\n");
            }
            editor.unmap_params(&editor, scene_secondary, garden_token);
        }
    }
#    endif

    pnanovdb_camera_t* cameraPtr = editor.get_camera(&editor, scene_main);
    if (cameraPtr)
    {
        printf("Camera position: (%f, %f, %f)\n", cameraPtr->state.position.x, cameraPtr->state.position.y,
               cameraPtr->state.position.z);
    }
    else
    {
        printf("No camera found\n");
    }

    runEditorLoop(5);

    pnanovdb_camera_t camera_default;
    pnanovdb_camera_init(&camera_default);
    editor.update_camera_2(&editor, scene_main, &camera_default);

    runEditorLoop(1);

    cameraPtr = editor.get_camera(&editor, scene_main);
    if (cameraPtr)
    {
        printf("Camera position: (%f, %f, %f)\n", cameraPtr->state.position.x, cameraPtr->state.position.y,
               cameraPtr->state.position.z);
    }
    else
    {
        printf("No camera found\n");
    }

    runEditorLoop(10);

    // editor.remove(&editor, scene_main, nullptr);

    runEditorLoop(1000);

    pnanovdb_editor_free(&editor);
#else
#endif

#if defined(TEST_RASTER) || defined(TEST_RASTER_2D)
    pnanovdb_raster_free(&raster);
    pnanovdb_fileformat_free(&fileformat);
#endif

    compute.device_interface.destroy_device(device_manager, device);
    compute.device_interface.destroy_device_manager(device_manager);

    pnanovdb_compute_free(&compute);

    return 0;
}
