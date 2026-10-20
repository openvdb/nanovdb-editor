// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/editor/Editor.cpp

    \author Petra Hapalova

    \brief
*/

#include "Editor.h"

PNANOVDB_CAST_PAIR(pnanovdb_editor_token_t, pnanovdb_compute_token_t)

#include <mutex>
#include <thread>

// signal handling
#include <atomic>
#if defined(_WIN32)
#    define WIN32_LEAN_AND_MEAN
#    include <windows.h>
#else
#    include <signal.h>
#    include <unistd.h>
#    include <stdio.h>
#endif

static const pnanovdb_uint32_t s_default_width = 1440u;
static const pnanovdb_uint32_t s_default_height = 720u;

namespace pnanovdb_editor
{

static pnanovdb_bool_t init_impl(pnanovdb_editor_t* editor,
                                 const pnanovdb_compute_t* compute,
                                 const pnanovdb_compiler_t* compiler)
{
    editor->impl = new pnanovdb_editor_impl_t();
    if (!editor->impl)
    {
        return PNANOVDB_FALSE;
    }

    editor->impl->compute = compute;
    editor->impl->compiler = compiler;
    editor->impl->pipeline_registry = nullptr;
    editor->impl->pipeline_config = nullptr;
    editor->impl->resolved_port = PNANOVDB_EDITOR_RESOLVED_PORT_PENDING;

    return PNANOVDB_TRUE;
}

void init(pnanovdb_editor_t* editor)
{
    pnanovdb_pipeline_load(&editor->impl->pipeline, editor->impl->compute);

    editor->impl->pipeline_registry = editor->impl->pipeline.create_registry(
        editor->impl->compute, editor->impl->compiler);
    editor->impl->pipeline_config = editor->impl->pipeline.create_config(
        editor->impl->pipeline_registry);
}

void shutdown(pnanovdb_editor_t* editor)
{
    // cleanup pipeline system
    if (editor->impl->pipeline_config)
    {
        editor->impl->pipeline.destroy_config(
            editor->impl->pipeline_registry, editor->impl->pipeline_config);
        editor->impl->pipeline_config = nullptr;
    }
    if (editor->impl->pipeline_registry)
    {
        editor->impl->pipeline.destroy_registry(
            editor->impl->compute,
            editor->impl->compiler,
            editor->impl->pipeline_registry);
        editor->impl->pipeline_registry = nullptr;
    }
    if (editor->impl->pipeline.create_registry)
    {
        pnanovdb_pipeline_free(&editor->impl->pipeline);
        pnanovdb_pipeline_t null_iface = {};
        editor->impl->pipeline = null_iface;
    }

    // invalidate, passed in with start()
    if (editor->impl->start_params)
    {
        editor->impl->start_params.reset();
    }

    if (editor->impl)
    {
        delete editor->impl;
        editor->impl = nullptr;
    }
}

void add_nanovdb_2(pnanovdb_editor_t* editor,
                   pnanovdb_editor_token_t* scene,
                   pnanovdb_editor_token_t* name,
                   pnanovdb_compute_array_t* array_in);
void add_nanovdb(pnanovdb_editor_t* editor, pnanovdb_compute_array_t* nanovdb_array)
{
    add_nanovdb_2(editor, editor->get_token("main"), editor->get_token("default"), nanovdb_array);
}

void add_named_array(pnanovdb_editor_t* editor,
                     pnanovdb_editor_token_t* scene,
                     pnanovdb_editor_token_t* object_name,
                     pnanovdb_editor_token_t* array_name,
                     pnanovdb_compute_array_t* array);
void add_array(pnanovdb_editor_t* editor, pnanovdb_compute_array_t* data_array)
{
    add_named_array(editor, editor->get_token("main"), editor->get_token("default"), editor->get_token("default"), data_array);
}

void add_gaussian_data(pnanovdb_editor_t* editor,
                       pnanovdb_raster_context_t* raster_ctx,
                       pnanovdb_compute_queue_t* queue,
                       pnanovdb_raster_gaussian_data_t* gaussian_data)
{
    // obsolete API, not implemented for now
}

void update_camera(pnanovdb_editor_t* editor, pnanovdb_camera_t* camera)
{
    // obsolete API, not implemented for now
}

void add_camera_view(pnanovdb_editor_t* editor, pnanovdb_camera_view_t* camera)
{
    // obsolete API, not implemented for now
}

void add_shader_params(pnanovdb_editor_t* editor, void* params, const pnanovdb_reflect_data_type_t* data_type)
{
    // obsolete API, not implemented for now
}

void sync_shader_params(pnanovdb_editor_t* editor, void* shader_params, pnanovdb_bool_t set_data)
{
    // obsolete API, not implemented for now
}

static std::atomic<bool> g_sigint_should_run = true;

static std::mutex g_sigint_mutex;
static int g_sigint_refcount = 0;

#if defined(_WIN32)
static bool g_sigint_handler_registered = false;

BOOL WINAPI editor_sigint_handler_win32(DWORD dwCtrlType)
{
    if (dwCtrlType == CTRL_C_EVENT)
    {
        printf("Ctrl-C pressed. Exiting NanoVDB Editor main loop.\n");
        g_sigint_should_run.store(false);
        return TRUE;
    }
    return FALSE;
}
#else
static struct sigaction g_sigint_sa;
static struct sigaction g_sigint_sa_old;

void editor_sigint_handler(int sig)
{
    printf("Ctrl-C pressed. Exiting NanoVDB Editor main loop.\n");
    g_sigint_should_run.store(false);
}
#endif

void editor_sigint_register()
{
    std::lock_guard<std::mutex> lock(g_sigint_mutex);

    int refcount = g_sigint_refcount;
    g_sigint_refcount++;
    // printf("NanoVDB Editor: Initialized signal handler refcount(%d)\n", refcount);
    if (refcount == 0)
    {
        // signal handling
        g_sigint_should_run.store(true);
#if defined(_WIN32)
        if (SetConsoleCtrlHandler(editor_sigint_handler_win32, TRUE))
        {
            g_sigint_handler_registered = true;
        }
        else
        {
            printf("NanoVDB Editor: Warning - failed to register Ctrl-C handler\n");
            g_sigint_handler_registered = false;
        }
#else
        g_sigint_sa.sa_handler = editor_sigint_handler;
        sigemptyset(&g_sigint_sa.sa_mask);
        g_sigint_sa.sa_flags = 0;
        sigaction(SIGINT, &g_sigint_sa, &g_sigint_sa_old);
#endif
    }
}

bool editor_sigint_should_run()
{
    return g_sigint_should_run.load();
}

void editor_sigint_unregister()
{
    std::lock_guard<std::mutex> lock(g_sigint_mutex);

    int refcount = g_sigint_refcount;
    g_sigint_refcount--;
    // printf("NanoVDB Editor: Released signal handler refcount(%d)\n", refcount);
    if (refcount == 1)
    {
        // restore old signal handler
#if defined(_WIN32)
        if (g_sigint_handler_registered)
        {
            if (!SetConsoleCtrlHandler(editor_sigint_handler_win32, FALSE))
            {
                printf("NanoVDB Editor: Warning - failed to unregister Ctrl-C handler\n");
            }
            g_sigint_handler_registered = false;
        }
#else
        // do not restore own handler
        if (g_sigint_sa_old.sa_handler != g_sigint_sa.sa_handler)
        {
            struct sigaction sa_restore;
            sigaction(SIGINT, &g_sigint_sa_old, &sa_restore);
            if (g_sigint_sa.sa_handler != sa_restore.sa_handler)
            {
                printf("NanoVDB Editor SIGINT handler restore failed. Control-C may not behave as expected\n");
            }
        }
#endif
    }
}

pnanovdb_int32_t editor_get_external_active_count(void* external_active_count)
{
    auto editor = static_cast<pnanovdb_editor_t*>(external_active_count);
    if (!editor_sigint_should_run())
    {
        return 1;
    }

    if (!editor->impl)
    {
        return 0;
    }

    pnanovdb_int32_t count = 0;
    if (editor->impl->should_stop.load())
    {
        count = 1;
    }
    return count;
};

static void run_show_loop(pnanovdb_editor_t* editor, pnanovdb_compute_device_t* device, pnanovdb_editor_config_t* config)
{
    if (!editor->impl->compute || !editor->impl->compiler || !device || !config)
    {
        return;
    }

    pnanovdb_int32_t image_width = s_default_width;
    pnanovdb_int32_t image_height = s_default_height;

    pnanovdb_imgui_window_interface_t* imgui_window_iface = pnanovdb_imgui_get_window_interface();
    if (!imgui_window_iface)
    {
        return;
    }

    imgui_instance_user::InstanceUserdata instance_user_data = {};
    instance_user_data.compute = editor->impl->compute;
    instance_user_data.pipeline = &editor->impl->pipeline;
    instance_user_data.pipeline_registry = editor->impl->pipeline_registry;
    instance_user_data.pipeline_config = editor->impl->pipeline_config;

    pnanovdb_imgui_settings_render_t imgui_user_settings = {};
    imgui_user_settings.window_width = image_width;
    imgui_user_settings.window_height = image_height;

    pnanovdb_imgui_instance_interface_t* imgui_instance_iface = get_user_imgui_instance_interface();
    void* imgui_instance_userdata = &instance_user_data;
    pnanovdb_imgui_window_t* imgui_window = imgui_window_iface->create(
        editor->impl->compute, device, &imgui_user_settings,
        imgui_instance_iface, imgui_instance_userdata, config->headless);

    if (!imgui_window)
    {
        return;
    }

    // Automatically enable encoder when streaming is requested
    if (config->streaming || config->stream_to_file)
    {
        imgui_user_settings.enable_encoder = PNANOVDB_TRUE;
    }
    // echo server settings from config to imgui_user_settings
    pnanovdb_uint32_t str_idx = 0u;
    for (;config->ip_address && str_idx < 255u && config->ip_address[str_idx]; str_idx++)
    {
        imgui_user_settings.server_address[str_idx] = config->ip_address[str_idx];
    }
    imgui_user_settings.server_address[str_idx] = '\0';
    imgui_user_settings.server_port = config->port;

    pnanovdb_compiler_instance_t* compiler_inst = editor->impl->compiler->create_instance();
    pnanovdb_compute_queue_t* device_queue = editor->impl->compute->device_interface.get_device_queue(device);
    // used for a worker thread
    pnanovdb_compute_queue_t* compute_queue = editor->impl->compute->device_interface.get_compute_queue(device);
    pnanovdb_compute_interface_t* compute_interface =
        editor->impl->compute->device_interface.get_compute_interface(device_queue);
    pnanovdb_compute_context_t* compute_context =
        editor->impl->compute->device_interface.get_compute_context(device_queue);

    // create runtime
    pnanovdb_pipeline_runtime_t* pipeline_runtime = editor->impl->pipeline.create_runtime(
        editor->impl->compute, device_queue);

    pnanovdb_camera_mat_t view = {};
    pnanovdb_camera_mat_t projection = {};

    pnanovdb_compute_texture_t* background_image = nullptr;

    // editor->impl->compute->device_interface.enable_profiler(
    //     compute_context, (void*)"editor", pnanovdb_editor::Profiler::report_callback);
    // editor->impl->compute->device_interface.get_memory_stats(device, Profiler::getInstance().getMemoryStats());

    // editor->impl->compiler->set_diagnostic_callback(compiler_inst,);

    auto create_background = [&]()
    {
        pnanovdb_compute_texture_desc_t tex_desc = {};
        tex_desc.texture_type = PNANOVDB_COMPUTE_TEXTURE_TYPE_2D;
        tex_desc.usage = PNANOVDB_COMPUTE_TEXTURE_USAGE_TEXTURE | PNANOVDB_COMPUTE_TEXTURE_USAGE_RW_TEXTURE;
        tex_desc.format = PNANOVDB_COMPUTE_FORMAT_R8G8B8A8_UNORM;
        tex_desc.width = image_width;
        tex_desc.height = image_height;
        tex_desc.depth = 1u;
        tex_desc.mip_levels = 1u;
        background_image = compute_interface->create_texture(compute_context, &tex_desc);
    };

    auto cleanup_background = [&]()
    {
        if (background_image)
        {
            compute_interface->destroy_texture(compute_context, background_image);
        }
        background_image = nullptr;
    };

    // we want sigint capture if this is the main thread and blocking progress as long as it runs
    bool enabled_sigint = !editor->impl->worker_thread.joinable();
    if (enabled_sigint)
    {
        editor_sigint_register();
    }

    bool should_run = true;
    while (should_run && editor_sigint_should_run())
    {
        // update memory stats periodically
        // if (imgui_user_instance && imgui_user_instance->pending.update_memory_stats)
        //{
        //    editor->impl->compute->device_interface.get_memory_stats(device,
        //    Profiler::getInstance().getMemoryStats()); imgui_user_instance->pending.update_memory_stats = false;
        //}

        create_background();

        bool rendered = false;

        pnanovdb_pipeline_runtime_target_t runtime_target = {};
        runtime_target.color = background_image;
        runtime_target.depth = nullptr;
        runtime_target.width = image_width;
        runtime_target.height = image_height;

        editor->impl->pipeline.update_runtime_main(
            editor->impl->compute,
            device_queue,
            pipeline_runtime,
            editor->impl->pipeline_config,
            &runtime_target
        );
        rendered = true;

        if (!rendered)
        {
            cleanup_background();
        }

        auto report_resolved_port = [](void* userdata, pnanovdb_int32_t port)
        {
            auto editor = (pnanovdb_editor_t*)userdata;
            editor->impl->resolved_port.store(port);
        };

        pnanovdb_camera_t* camera = nullptr;
        {
            const pnanovdb_compute_t* compute = editor->impl->compute;
            pnanovdb_pipeline_t* pipeline = &editor->impl->pipeline;
            pnanovdb_pipeline_config_t* pipe_config = editor->impl->pipeline_config;
            pnanovdb_compute_token_t* scene = pipeline->config_get_active_scene(pipe_config);
            pnanovdb_compute_token_t* camera_name = pipeline->config_get_active_camera(pipe_config, scene);
            pnanovdb_pipeline_object_mapped_t* mapped_object = nullptr;
            if(pipeline->config_map_params(pipe_config, scene, camera_name,
                PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_pipeline_object_mapped_t), (void**)&mapped_object))
            {
                camera = mapped_object->camera;
                pipeline->config_unmap_params(pipe_config, scene, camera_name,
                    PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_pipeline_object_mapped_t));
            }
            if (!camera)
            {
                pnanovdb_compute_token_t* obj_name = compute->get_token("default_camera");
                pipeline->config_add(pipe_config, scene, obj_name);

                pnanovdb_camera_t camera = {};
                pnanovdb_camera_init(&camera);
                pnanovdb_camera_state_default(&camera.state, PNANOVDB_TRUE);
                pnanovdb_camera_config_default(&camera.config);
                pipeline->config_set_camera(pipe_config, scene, obj_name, &camera);
                pipeline->config_set_active_camera(pipe_config, scene, obj_name);
            }
        }

        // update viewport image
        should_run = imgui_window_iface->update(
            editor->impl->compute, device_queue,
            background_image ? compute_interface->register_texture_as_transient(compute_context, background_image) :
                               nullptr,
            &image_width, &image_height, report_resolved_port, editor, imgui_window, camera, &imgui_user_settings,
            editor_get_external_active_count, editor);

        // align width/height setting to reality
        if (!imgui_user_settings.window_resize)
        {
            imgui_user_settings.window_width = image_width;
            imgui_user_settings.window_height = image_height;
        }

        if (background_image)
        {
            compute_interface->destroy_texture(compute_context, background_image);
        }

        if (editor->impl->should_stop.load())
        {
            should_run = false;
        }
    }
    editor->impl->compute->device_interface.wait_idle(device_queue);
    editor->impl->compute->device_interface.disable_profiler(compute_context);

    // destroy runtime
    if (pipeline_runtime)
    {
        editor->impl->pipeline.destroy_runtime(
            editor->impl->compute,
            device_queue,
            pipeline_runtime);
        pipeline_runtime = nullptr;
    }

    editor->impl->compiler->destroy_instance(compiler_inst);

    // Destroy ImGui first (while editor_scene is still valid) so saveIniSettings can access it
    imgui_window_iface->destroy(editor->impl->compute, device_queue, imgui_window);

    if (enabled_sigint)
    {
        editor_sigint_unregister();
    }
}

void show(pnanovdb_editor_t* editor, pnanovdb_compute_device_t* device, pnanovdb_editor_config_t* config)
{
    if (!editor || !editor->impl)
        return;

    run_show_loop(editor, device, config);
}

pnanovdb_int32_t get_resolved_port(pnanovdb_editor_t* editor, pnanovdb_bool_t should_wait)
{
    while (should_wait && editor->impl->show_active.load() &&
           editor->impl->resolved_port.load() == PNANOVDB_EDITOR_RESOLVED_PORT_PENDING)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return editor->impl->resolved_port.load();
}

void start(pnanovdb_editor_t* editor, pnanovdb_compute_device_t* device, pnanovdb_editor_config_t* config)
{
    if (!editor || !editor->impl || !config)
        return;

    // avoid multiple starts
    if (editor->impl->worker_thread.joinable())
    {
        return;
    }
    // capture start params both because of thread startup delay and for reset()
    editor->impl->start_params = std::make_unique<pnanovdb_editor_start_params_t>();
    auto start_params = editor->impl->start_params.get();
    start_params->device = device;
    start_params->config = *config;
    start_params->config_ip_address = config->ip_address ? std::string(config->ip_address) : std::string();
    start_params->config_ui_profile_name =
        config->ui_profile_name ? std::string(config->ui_profile_name) : std::string();
    start_params->config.ip_address = start_params->config_ip_address.c_str();
    start_params->config.ui_profile_name = start_params->config_ui_profile_name.c_str();

    editor->impl->show_active.store(true);
    editor->impl->worker_thread = std::thread([editor, device]()
    {
        run_show_loop(editor, device, &editor->impl->start_params->config);
    });
}

void stop(pnanovdb_editor_t* editor)
{
    if (!editor || !editor->impl)
        return;

    // signal worker to stop
    editor->impl->should_stop.store(true);

    if (editor->impl->worker_thread.joinable())
    {
        editor->impl->worker_thread.join();
    }
}

void reset(pnanovdb_editor_t* editor)
{
    if (!editor || !editor->impl)
    {
        return;
    }
    // grab the start params before they get destroyed during shutdown
    std::unique_ptr<pnanovdb_editor_start_params_t> tmp_params = std::move(editor->impl->start_params);
    if (!tmp_params)
    {
        return;
    }
    // grab compute/compiler as they are destroyed with impl during shutdown
    auto compute = editor->impl->compute;
    auto compiler = editor->impl->compiler;

    shutdown(editor);

    if (init_impl(editor, compute, compiler))
    {
        init(editor);
    }

    start(editor, tmp_params->device, &tmp_params->config);
}

/// Blocks the calling thread until the editor window is closed or an interrupt signal (SIGINT) is received.
/// This function is typically called from the main thread to keep the application running while the editor is active.
void wait_for_interrupt(pnanovdb_editor_t* editor)
{
    if (editor && editor->impl && editor->impl->show_active.load())
    {
        editor_sigint_register();
        while (editor->impl->show_active.load() && editor_sigint_should_run())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        editor_sigint_unregister();
    }
}

pnanovdb_bool_t get_camera_2(pnanovdb_editor_t* editor, pnanovdb_editor_token_t* scene, pnanovdb_camera_t* out_camera)
{
    if (!editor || !editor->impl || !scene || !out_camera)
    {
        return PNANOVDB_FALSE;
    }

    auto pipeline = &editor->impl->pipeline;
    auto pipe_config = editor->impl->pipeline_config;
    pnanovdb_compute_token_t* camera_name = pipeline->config_get_active_camera(
        pipe_config,
        cast(scene)
    );

    if (camera_name)
    {
        pnanovdb_pipeline_object_mapped_t* mapped_object = nullptr;
        if(pipeline->config_map_params(pipe_config, cast(scene), camera_name,
            PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_pipeline_object_mapped_t), (void**)&mapped_object))
        {
            *out_camera = *mapped_object->camera;
            pipeline->config_unmap_params(pipe_config, cast(scene), camera_name,
                PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_pipeline_object_mapped_t));
            return PNANOVDB_TRUE;
        }
    }
    pnanovdb_camera_init(out_camera);
    pnanovdb_camera_state_default(&out_camera->state, PNANOVDB_TRUE);
    pnanovdb_camera_config_default(&out_camera->config);
    return PNANOVDB_FALSE;
}

pnanovdb_camera_t* get_camera(pnanovdb_editor_t* editor, pnanovdb_editor_token_t* scene)
{
    if (!editor || !editor->impl || !scene)
    {
        return nullptr;
    }

    get_camera_2(editor, scene, &editor->impl->tmp_camera);
    return &editor->impl->tmp_camera;
}

pnanovdb_bool_t load_scene(pnanovdb_editor_t* editor, const char* filepath, pnanovdb_bool_t overwrite)
{
    if (!editor || !editor->impl || !filepath || filepath[0] == '\0')
        return PNANOVDB_FALSE;

    // TODO: add with serialization

    return PNANOVDB_FALSE;
}

pnanovdb_bool_t save_scene(pnanovdb_editor_t* editor, const char* filepath)
{
    if (!editor || !editor->impl || !filepath || filepath[0] == '\0')
        return PNANOVDB_FALSE;

    // TODO: add with serialization

    return PNANOVDB_FALSE;
}

void add_nanovdb_2(pnanovdb_editor_t* editor,
                   pnanovdb_editor_token_t* scene,
                   pnanovdb_editor_token_t* name,
                   pnanovdb_compute_array_t* array_in)
{
    if (!editor || !editor->impl || !scene || !name || !array_in)
    {
        return;
    }

    auto pipeline = &editor->impl->pipeline;
    auto pipe_config = editor->impl->pipeline_config;
    auto pipe_reg = editor->impl->pipeline_registry;
    pipeline->config_set_buffer_array(
        pipe_config,
        cast(scene),
        cast(name),
        editor->impl->compute->get_token("nanovdb"),
        array_in);

    pnanovdb_pipeline_type_t default_load = 0u;
    pnanovdb_pipeline_type_t default_render = 0u;
    pipeline->get_pipeline_type(
            pipe_reg, editor->impl->compute->get_token("nanovdb_load"), &default_load);
    pipeline->get_pipeline_type(
            pipe_reg, editor->impl->compute->get_token("nanovdb_render"), &default_render);

    pipeline->config_set_pipeline(
        pipe_config,
        cast(scene),
        cast(name),
        pnanovdb_pipeline_stage_load,
        0u,
        default_load);
    pipeline->config_set_pipeline(
        pipe_config,
        cast(scene),
        cast(name),
        pnanovdb_pipeline_stage_render,
        0u,
        default_render);
}

void add_gaussian_data_2(pnanovdb_editor_t* editor,
                         pnanovdb_editor_token_t* scene,
                         pnanovdb_editor_token_t* name,
                         const pnanovdb_editor_gaussian_data_desc_t* desc)
{
    if (!editor || !editor->impl || !scene || !name || !desc)
    {
        return;
    }

    auto pipeline = &editor->impl->pipeline;
    auto pipe_config = editor->impl->pipeline_config;
    auto pipe_reg = editor->impl->pipeline_registry;

    const char* names[6u] = {"means", "opacities", "quaternions", "scales", "sh_0", "sh_n"};
    pnanovdb_compute_array_t* arrays[7u] = {desc->means, desc->opacities, desc->quaternions,
        desc->scales, desc->sh_0, desc->sh_n};

    for (pnanovdb_uint32_t idx = 0u; idx < 6u; idx++)
    {
        pipeline->config_set_buffer_array(
            pipe_config,
            cast(scene),
            cast(name),
            editor->impl->compute->get_token(names[idx]),
            arrays[idx]);
    }

    pnanovdb_pipeline_type_t default_load = 0u;
    pnanovdb_pipeline_type_t default_render = 0u;
    pipeline->get_pipeline_type(
        pipe_reg, editor->impl->compute->get_token("gaussian_load"), &default_load);
    pipeline->get_pipeline_type(
        pipe_reg, editor->impl->compute->get_token("gaussian_render"), &default_render);

    pipeline->config_set_pipeline(
        pipe_config,
        cast(scene),
        cast(name),
        pnanovdb_pipeline_stage_load,
        0u,
        default_load);
    pipeline->config_set_pipeline(
        pipe_config,
        cast(scene),
        cast(name),
        pnanovdb_pipeline_stage_render,
        0u,
        default_render);
}

void add_nanovdb_3(pnanovdb_editor_t* editor,
                   pnanovdb_editor_token_t* scene,
                   pnanovdb_editor_token_t* name,
                   pnanovdb_compute_array_t* array_in,
                   pnanovdb_pipeline_type_t process_pipeline,
                   pnanovdb_pipeline_type_t render_pipeline)
{
    if (!editor || !editor->impl || !scene || !name || !array_in)
    {
        return;
    }

    auto pipeline = &editor->impl->pipeline;
    auto pipe_config = editor->impl->pipeline_config;
    auto pipe_reg = editor->impl->pipeline_registry;

    pipeline->config_set_buffer_array(
        pipe_config,
        cast(scene),
        cast(name),
        editor->impl->compute->get_token("nanovdb"),
        array_in);

    pnanovdb_pipeline_type_t default_load = 0u;
    pipeline->get_pipeline_type(
        pipe_reg, editor->impl->compute->get_token("nanovdb_load"), &default_load);

    pipeline->config_set_pipeline(
        pipe_config,
        cast(scene),
        cast(name),
        pnanovdb_pipeline_stage_load,
        0u,
        default_load);
    pipeline->config_set_pipeline(
        pipe_config,
        cast(scene),
        cast(name),
        pnanovdb_pipeline_stage_process,
        0u,
        process_pipeline);
    pipeline->config_set_pipeline(
        pipe_config,
        cast(scene),
        cast(name),
        pnanovdb_pipeline_stage_render,
        0u,
        render_pipeline);
}

void add_gaussian_data_4(pnanovdb_editor_t* editor,
                         pnanovdb_editor_token_t* scene,
                         pnanovdb_editor_token_t* name,
                         pnanovdb_pipeline_type_t process_pipeline,
                         pnanovdb_pipeline_type_t render_pipeline);
void add_gaussian_data_3(pnanovdb_editor_t* editor,
                         pnanovdb_editor_token_t* scene,
                         pnanovdb_editor_token_t* name,
                         const pnanovdb_editor_gaussian_data_desc_t* desc,
                         pnanovdb_pipeline_type_t process_pipeline,
                         pnanovdb_pipeline_type_t render_pipeline)
{
    if (!editor || !editor->impl || !scene || !name || !desc)
    {
        return;
    }

    auto pipeline = &editor->impl->pipeline;
    auto pipe_config = editor->impl->pipeline_config;

    const char* names[6u] = {"means", "opacities", "quaternions", "scales", "sh_0", "sh_n"};
    pnanovdb_compute_array_t* arrays[7u] = {desc->means, desc->opacities, desc->quaternions,
        desc->scales, desc->sh_0, desc->sh_n};

    for (pnanovdb_uint32_t idx = 0u; idx < 6u; idx++)
    {
        pipeline->config_set_buffer_array(
            pipe_config,
            cast(scene),
            cast(name),
            editor->impl->compute->get_token(names[idx]),
            arrays[idx]);
    }

    add_gaussian_data_4(editor, scene, name, process_pipeline, render_pipeline);
}

void add_gaussian_data_4(pnanovdb_editor_t* editor,
                         pnanovdb_editor_token_t* scene,
                         pnanovdb_editor_token_t* name,
                         pnanovdb_pipeline_type_t process_pipeline,
                         pnanovdb_pipeline_type_t render_pipeline)
{
    if (!editor || !editor->impl || !scene || !name)
    {
        return;
    }

    auto pipeline = &editor->impl->pipeline;
    auto pipe_config = editor->impl->pipeline_config;
    auto pipe_reg = editor->impl->pipeline_registry;

    pnanovdb_pipeline_type_t default_load = 0u;
    pipeline->get_pipeline_type(
        pipe_reg, editor->impl->compute->get_token("gaussian_load"), &default_load);

    pipeline->config_set_pipeline(
        pipe_config,
        cast(scene),
        cast(name),
        pnanovdb_pipeline_stage_load,
        0u,
        default_load);
    pipeline->config_set_pipeline(
        pipe_config,
        cast(scene),
        cast(name),
        pnanovdb_pipeline_stage_process,
        0u,
        process_pipeline);
    pipeline->config_set_pipeline(
        pipe_config,
        cast(scene),
        cast(name),
        pnanovdb_pipeline_stage_render,
        0u,
        render_pipeline);
}

void add_camera_view_2(pnanovdb_editor_t* editor, pnanovdb_editor_token_t* scene, pnanovdb_camera_view_t* camera)
{
    if (!editor || !editor->impl || !scene || !camera)
    {
        return;
    }
    if (!camera->name || !camera->name->str)
    {
        return;
    }

    auto pipeline = &editor->impl->pipeline;
    auto pipe_config = editor->impl->pipeline_config;
    pipeline->config_set_camera_view(
        pipe_config,
        cast(scene),
        cast(camera->name),
        camera);
}

void update_camera_2(pnanovdb_editor_t* editor, pnanovdb_editor_token_t* scene, pnanovdb_camera_t* camera)
{
    if (!editor || !editor->impl || !scene || !camera)
    {
        return;
    }

    auto pipeline = &editor->impl->pipeline;
    auto pipe_config = editor->impl->pipeline_config;
    pnanovdb_compute_token_t* camera_name = pipeline->config_get_active_camera(
        pipe_config,
        cast(scene)
    );
    pipeline->config_set_camera(pipe_config, cast(scene), camera_name, camera);
}

void remove(pnanovdb_editor_t* editor, pnanovdb_editor_token_t* scene, pnanovdb_editor_token_t* name)
{
    if (!editor || !editor->impl || !scene)
    {
        return;
    }

    auto pipeline = &editor->impl->pipeline;
    auto pipe_config = editor->impl->pipeline_config;
    pipeline->config_remove(
        pipe_config,
        cast(scene),
        cast(name));
}

static void update_editor_shader_name(const pnanovdb_compute_t* compute, pnanovdb_pipeline_t* pipeline, pnanovdb_pipeline_config_t* pipe_config,
                                      pnanovdb_editor_token_t* scene, pnanovdb_editor_token_t* name,
                                      pnanovdb_editor_token_t** p_shader_path, bool do_set)
{
    if (!p_shader_path || !(*p_shader_path))
    {
        return;
    }
    pnanovdb_pipeline_object_mapped_t* mapped_object = nullptr;
    if (!pipeline->config_map_params(pipe_config, cast(scene), cast(name),
        PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_pipeline_object_mapped_t), (void**)&mapped_object))
    {
        return;
    }
    if (pnanovdb_pipeline_stage_render < mapped_object->stage_count)
    {
        pnanovdb_pipeline_object_mapped_stage_t* stage = &mapped_object->stages[pnanovdb_pipeline_stage_render];
        for (pnanovdb_uint64_t pipeline_idx = 0u; pipeline_idx < stage->pipeline_count; pipeline_idx++)
        {
            pnanovdb_pipeline_object_mapped_pipeline_t* pipe = &stage->pipelines[pipeline_idx];
            if (pipe->params_data && pipe->params_data_type)
            {
                for (pnanovdb_uint64_t idx = 0u; idx < pipe->params_data_type->child_reflect_data_count; idx++)
                {
                    auto reflect_data = &pipe->params_data_type->child_reflect_datas[idx];
                    if (pnanovdb_reflect_layout_compare(
                            reflect_data->data_type,
                            PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_pipeline_shader_t)))
                    {
                        pnanovdb_pipeline_shader_t* shader = (pnanovdb_pipeline_shader_t*)(((uint8_t*)pipe->params_data) + reflect_data->data_offset);
                        if (do_set)
                        {
                            shader->shader_path = (*p_shader_path)->str;
                        }
                        else
                        {
                            (*p_shader_path) = cast(compute->get_token(shader->shader_path));
                        }
                        // only set the first shader path per pipeline
                        break;
                    }
                }
            }
        }
    }
    pipeline->config_unmap_params(pipe_config, cast(scene), cast(name), PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_pipeline_object_mapped_t));
}

void* map_params(pnanovdb_editor_t* editor,
                 pnanovdb_editor_token_t* scene,
                 pnanovdb_editor_token_t* name,
                 const pnanovdb_reflect_data_type_t* data_type)
{
    if (!editor || !editor->impl || !data_type || !scene)
    {
        return nullptr;
    }

    // for now, we only support mapping one data_type at a time
    editor->impl->map_params_mutex.lock();
    editor->impl->map_params_type = data_type;

    void* mapped_ptr = nullptr;
    auto pipeline = &editor->impl->pipeline;
    auto pipe_config = editor->impl->pipeline_config;
    if (pnanovdb_reflect_layout_compare(data_type, PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_editor_shader_name_t)))
    {
        update_editor_shader_name(editor->impl->compute, pipeline, pipe_config, scene, name, &editor->impl->tmp_editor_shader_name.shader_name, false);
        mapped_ptr = &editor->impl->tmp_editor_shader_name;
        editor->impl->map_params_status = PNANOVDB_TRUE;
    }
    else
    {
        auto pipeline = &editor->impl->pipeline;
        auto pipe_config = editor->impl->pipeline_config;
        editor->impl->map_params_status = pipeline->config_map_params(
            pipe_config, cast(scene), cast(name), editor->impl->map_params_type, &mapped_ptr);
    }
    return mapped_ptr;
}

void unmap_params(pnanovdb_editor_t* editor, pnanovdb_editor_token_t* scene, pnanovdb_editor_token_t* name)
{
    if (!editor || !editor->impl)
    {
        return;
    }

    auto pipeline = &editor->impl->pipeline;
    auto pipe_config = editor->impl->pipeline_config;
    if (editor->impl->map_params_status)
    {
        if (pnanovdb_reflect_layout_compare(editor->impl->map_params_type, PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_editor_shader_name_t)))
        {
            update_editor_shader_name(editor->impl->compute,  pipeline, pipe_config, scene, name, &editor->impl->tmp_editor_shader_name.shader_name, true);
        }
        else
        {
            pipeline->config_unmap_params(pipe_config, cast(scene), cast(name), editor->impl->map_params_type);
        }

        editor->impl->map_params_status = PNANOVDB_FALSE;
    }

    editor->impl->map_params_mutex.unlock();
}

pnanovdb_pipeline_params_t* map_pipeline_params(pnanovdb_editor_t* editor,
                                                pnanovdb_editor_token_t* scene,
                                                pnanovdb_editor_token_t* name,
                                                pnanovdb_pipeline_stage_t stage)
{
    if (!editor || !editor->impl || !scene || !name || stage >= pnanovdb_pipeline_stage_count)
    {
        return nullptr;
    }

    pnanovdb_pipeline_params_t pipe_params = {};
    auto pipeline = &editor->impl->pipeline;
    auto pipe_config = editor->impl->pipeline_config;
    pnanovdb_pipeline_object_mapped_t* mapped_object = nullptr;
    if(pipeline->config_map_params(pipe_config, cast(scene), cast(name), PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_pipeline_object_mapped_t), (void**)&mapped_object))
    {
        if (stage < mapped_object->stage_count)
        {
            auto cfg_stage = &mapped_object->stages[stage];
            if (cfg_stage->pipeline_count != 0u)
            {
                auto cfg_pipe = &cfg_stage->pipelines[0u];
                pipe_params.data = cfg_pipe->params_data;
                pipe_params.size = cfg_pipe->params_data_type ? cfg_pipe->params_data_type->element_size : 0llu;
                pipe_params.type = cfg_pipe->params_data_type;
            }
        }
    }
    editor->impl->tmp_pipeline_params = pipe_params;
    return &editor->impl->tmp_pipeline_params;
}

void unmap_pipeline_params(pnanovdb_editor_t* editor,
                           pnanovdb_editor_token_t* scene,
                           pnanovdb_editor_token_t* name,
                           pnanovdb_pipeline_stage_t stage)
{
    if (!editor || !editor->impl)
    {
        return;
    }

    auto pipeline = &editor->impl->pipeline;
    auto pipe_config = editor->impl->pipeline_config;
    pipeline->config_unmap_params(pipe_config, cast(scene), cast(name), PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_pipeline_object_mapped_t));
}

void set_pipeline(pnanovdb_editor_t* editor,
                  pnanovdb_editor_token_t* scene,
                  pnanovdb_editor_token_t* name,
                  pnanovdb_pipeline_stage_t stage,
                  pnanovdb_pipeline_type_t type)
{
    if (!editor || !editor->impl || !scene || !name)
    {
        return;
    }
    if (stage >= pnanovdb_pipeline_stage_count)
    {
        return;
    }

    auto pipeline = &editor->impl->pipeline;
    auto pipe_config = editor->impl->pipeline_config;
    pipeline->config_set_pipeline(
        pipe_config,
        cast(scene),
        cast(name),
        stage,
        0u,
        type);
}

pnanovdb_pipeline_type_t get_pipeline(pnanovdb_editor_t* editor,
                                      pnanovdb_editor_token_t* scene,
                                      pnanovdb_editor_token_t* name,
                                      pnanovdb_pipeline_stage_t stage)
{
    if (!editor || !editor->impl || !scene || !name)
    {
        return 0u;
    }

    pnanovdb_pipeline_type_t pipe_type = 0u;
    auto pipeline = &editor->impl->pipeline;
    auto pipe_config = editor->impl->pipeline_config;
    pnanovdb_pipeline_object_mapped_t* mapped_object = nullptr;
    if(pipeline->config_map_params(pipe_config, cast(scene), cast(name), PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_pipeline_object_mapped_t), (void**)&mapped_object))
    {
        if (stage < mapped_object->stage_count)
        {
            auto cfg_stage = &mapped_object->stages[stage];
            if (cfg_stage->pipeline_count != 0u)
            {
                auto cfg_pipe = &cfg_stage->pipelines[0u];
                pipe_type = cfg_pipe->pipeline_type;
            }
        }
        pipeline->config_unmap_params(pipe_config, cast(scene), cast(name), PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_pipeline_object_mapped_t));
    }
    return pipe_type;
}

pnanovdb_uint32_t get_process_step_count(pnanovdb_editor_t* editor,
                                         pnanovdb_editor_token_t* scene,
                                         pnanovdb_editor_token_t* name)
{
    if (!editor || !editor->impl || !scene || !name)
    {
        return 0u;
    }
    pnanovdb_uint32_t stage = pnanovdb_pipeline_stage_process;
    pnanovdb_uint32_t count = 0u;
    auto pipeline = &editor->impl->pipeline;
    auto pipe_config = editor->impl->pipeline_config;
    pnanovdb_pipeline_object_mapped_t* mapped_object = nullptr;
    if(pipeline->config_map_params(pipe_config, cast(scene), cast(name), PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_pipeline_object_mapped_t), (void**)&mapped_object))
    {
        if (stage < mapped_object->stage_count)
        {
            auto cfg_stage = &mapped_object->stages[stage];
            count = cfg_stage->pipeline_count;
        }
        pipeline->config_unmap_params(pipe_config, cast(scene), cast(name), PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_pipeline_object_mapped_t));
    }
    return count;
}

pnanovdb_pipeline_type_t get_process_step(pnanovdb_editor_t* editor,
                                          pnanovdb_editor_token_t* scene,
                                          pnanovdb_editor_token_t* name,
                                          pnanovdb_uint32_t step_index)
{
    if (!editor || !editor->impl || !scene || !name)
    {
        return 0u;
    }
    pnanovdb_uint32_t stage = pnanovdb_pipeline_stage_process;
    pnanovdb_pipeline_type_t pipe_type = 0u;
    auto pipeline = &editor->impl->pipeline;
    auto pipe_config = editor->impl->pipeline_config;
    pnanovdb_pipeline_object_mapped_t* mapped_object = nullptr;
    if(pipeline->config_map_params(pipe_config, cast(scene), cast(name), PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_pipeline_object_mapped_t), (void**)&mapped_object))
    {
        if (stage < mapped_object->stage_count)
        {
            auto cfg_stage = &mapped_object->stages[stage];
            if (step_index < cfg_stage->pipeline_count)
            {
                auto cfg_pipe = &cfg_stage->pipelines[step_index];
                pipe_type = cfg_pipe->pipeline_type;
            }
        }
        pipeline->config_unmap_params(pipe_config, cast(scene), cast(name), PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_pipeline_object_mapped_t));
    }
    return pipe_type;
}

void set_process_step(pnanovdb_editor_t* editor,
                      pnanovdb_editor_token_t* scene,
                      pnanovdb_editor_token_t* name,
                      pnanovdb_uint32_t step_index,
                      pnanovdb_pipeline_type_t type)
{
    if (!editor || !editor->impl || !scene || !name)
    {
        return;
    }
    pnanovdb_uint32_t stage = pnanovdb_pipeline_stage_process;
    auto pipeline = &editor->impl->pipeline;
    auto pipe_config = editor->impl->pipeline_config;
    pnanovdb_pipeline_object_mapped_t* mapped_object = nullptr;
    if(pipeline->config_map_params(pipe_config, cast(scene), cast(name), PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_pipeline_object_mapped_t), (void**)&mapped_object))
    {
        if (stage < mapped_object->stage_count)
        {
            auto cfg_stage = &mapped_object->stages[stage];
            if (step_index < cfg_stage->pipeline_count)
            {
                auto cfg_pipe = &cfg_stage->pipelines[step_index];
                cfg_pipe->pipeline_type = type;
            }
        }
        pipeline->config_unmap_params(pipe_config, cast(scene), cast(name), PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_pipeline_object_mapped_t));
    }
}


pnanovdb_pipeline_params_t* map_process_step_params(pnanovdb_editor_t* editor,
                                                    pnanovdb_editor_token_t* scene,
                                                    pnanovdb_editor_token_t* name,
                                                    pnanovdb_uint32_t step_index)
{
    if (!editor || !editor->impl || !scene || !name)
        return nullptr;

    pnanovdb_uint32_t stage = pnanovdb_pipeline_stage_process;
    pnanovdb_pipeline_params_t pipe_params = {};
    auto pipeline = &editor->impl->pipeline;
    auto pipe_config = editor->impl->pipeline_config;
    pnanovdb_pipeline_object_mapped_t* mapped_object = nullptr;
    if(pipeline->config_map_params(pipe_config, cast(scene), cast(name), PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_pipeline_object_mapped_t), (void**)&mapped_object))
    {
        if (stage < mapped_object->stage_count)
        {
            auto cfg_stage = &mapped_object->stages[stage];
            if (step_index < cfg_stage->pipeline_count)
            {
                auto cfg_pipe = &cfg_stage->pipelines[step_index];
                pipe_params.data = cfg_pipe->params_data;
                pipe_params.size = cfg_pipe->params_data_type ? cfg_pipe->params_data_type->element_size : 0llu;
                pipe_params.type = cfg_pipe->params_data_type;
            }
        }
    }
    editor->impl->tmp_pipeline_params = pipe_params;
    return &editor->impl->tmp_pipeline_params;
}

void unmap_process_step_params(pnanovdb_editor_t* editor,
                               pnanovdb_editor_token_t* scene,
                               pnanovdb_editor_token_t* name,
                               pnanovdb_uint32_t step_index)
{
    if (!editor || !editor->impl)
        return;

    auto pipeline = &editor->impl->pipeline;
    auto pipe_config = editor->impl->pipeline_config;
    pipeline->config_unmap_params(pipe_config, cast(scene), cast(name), PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_pipeline_object_mapped_t));
}

void mark_pipeline_dirty(pnanovdb_editor_t* editor, pnanovdb_editor_token_t* scene, pnanovdb_editor_token_t* name)
{
    if (!editor || !editor->impl || !scene || !name)
        return;

    auto pipeline = &editor->impl->pipeline;
    auto pipe_config = editor->impl->pipeline_config;
    pipeline->config_mark_pipeline_dirty(pipe_config, cast(scene), cast(name));
}

pnanovdb_bool_t set_custom_scene_params(pnanovdb_editor_t* editor,
                                        pnanovdb_editor_token_t* scene,
                                        pnanovdb_editor_token_t* json,
                                        char* error_buf,
                                        pnanovdb_uint64_t error_buf_size)
{
    if (!editor || !editor->impl || !scene || !json || !json->str)
    {
        return PNANOVDB_FALSE;
    }
    return PNANOVDB_FALSE;
}

const pnanovdb_reflect_data_type_t* get_custom_scene_params_data_type(pnanovdb_editor_t* editor,
                                                                      pnanovdb_editor_token_t* scene)
{
    if (!editor || !editor->impl || !scene)
    {
        return nullptr;
    }

    return nullptr;
}

void set_visible(pnanovdb_editor_t* editor,
                 pnanovdb_editor_token_t* scene,
                 pnanovdb_editor_token_t* name,
                 pnanovdb_bool_t visible)
{
    if (!editor || !editor->impl || !scene || !name)
    {
        return;
    }

    auto pipeline = &editor->impl->pipeline;
    auto pipe_config = editor->impl->pipeline_config;
    pnanovdb_pipeline_object_mapped_t* mapped_object = nullptr;
    if(pipeline->config_map_params(pipe_config, cast(scene), cast(name), PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_pipeline_object_mapped_t), (void**)&mapped_object))
    {
        if (mapped_object->is_visible)
        {
            *mapped_object->is_visible = visible;
        }
        pipeline->config_unmap_params(pipe_config, cast(scene), cast(name), PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_pipeline_object_mapped_t));
    }
}

pnanovdb_bool_t get_visible(pnanovdb_editor_t* editor, pnanovdb_editor_token_t* scene, pnanovdb_editor_token_t* name)
{
    if (!editor || !editor->impl || !scene || !name)
    {
        return PNANOVDB_FALSE;
    }

    pnanovdb_bool_t result = PNANOVDB_FALSE;
    auto pipeline = &editor->impl->pipeline;
    auto pipe_config = editor->impl->pipeline_config;
    pnanovdb_pipeline_object_mapped_t* mapped_object = nullptr;
    if(pipeline->config_map_params(pipe_config, cast(scene), cast(name), PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_pipeline_object_mapped_t), (void**)&mapped_object))
    {
        if (mapped_object->is_visible)
        {
            result = *mapped_object->is_visible;
        }
        pipeline->config_unmap_params(pipe_config, cast(scene), cast(name), PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_pipeline_object_mapped_t));
    }
    return result;
}

pnanovdb_bool_t get_pipeline_type(pnanovdb_editor_t* editor, pnanovdb_editor_token_t* name, pnanovdb_pipeline_type_t* out_type)
{
    if (!name || !name->str || !out_type)
    {
        return PNANOVDB_FALSE;
    }

    auto pipeline = &editor->impl->pipeline;
    auto pipe_reg = editor->impl->pipeline_registry;
    return pipeline->get_pipeline_type(pipe_reg, cast(name), out_type);
}

void add_named_array(pnanovdb_editor_t* editor,
                     pnanovdb_editor_token_t* scene,
                     pnanovdb_editor_token_t* object_name,
                     pnanovdb_editor_token_t* array_name,
                     pnanovdb_compute_array_t* array)
{
    if (!editor || !editor->impl || !scene || !object_name || !array_name || !array_name->str || !array)
    {
        return;
    }

    auto pipeline = &editor->impl->pipeline;
    auto pipe_config = editor->impl->pipeline_config;
    pipeline->config_set_buffer_array(pipe_config, cast(scene), cast(object_name), cast(array_name), array);
}

pnanovdb_compute_array_t* get_named_array(pnanovdb_editor_t* editor,
                                          pnanovdb_editor_token_t* scene,
                                          pnanovdb_editor_token_t* object_name,
                                          pnanovdb_editor_token_t* array_name)
{
    if (!editor || !editor->impl || !scene || !object_name || !array_name || !array_name->str)
    {
        return nullptr;
    }
    pnanovdb_compute_array_t* result = nullptr;
    auto pipeline = &editor->impl->pipeline;
    auto pipe_config = editor->impl->pipeline_config;
    pnanovdb_pipeline_object_mapped_t* mapped_object = nullptr;
    if(pipeline->config_map_params(pipe_config, cast(scene), cast(object_name),
        PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_pipeline_object_mapped_t), (void**)&mapped_object))
    {
        for (pnanovdb_uint64_t buffer_idx = 0u; buffer_idx < mapped_object->buffer_count; buffer_idx++)
        {
            if (mapped_object->buffer_names[buffer_idx]->id == array_name->id)
            {
                result = mapped_object->buffer_arrays[buffer_idx];
            }
        }
        pipeline->config_unmap_params(pipe_config, cast(scene), cast(object_name),
            PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_pipeline_object_mapped_t));
    }
    return result;
}

struct TokenLoader
{
    pnanovdb_compute_t compute = {};
    TokenLoader()
    {
        pnanovdb_compute_load(&compute, nullptr);
    }
    ~TokenLoader()
    {
        pnanovdb_compute_free(&compute);
    }
};

pnanovdb_editor_token_t* get_token(const char* name)
{
    static TokenLoader token_loader;
    return cast(token_loader.compute.get_token(name));
}

PNANOVDB_API pnanovdb_editor_t* pnanovdb_get_editor()
{
    static pnanovdb_editor_t editor = { PNANOVDB_REFLECT_INTERFACE_INIT(pnanovdb_editor_t) };

    editor.init_impl = init_impl;
    editor.init = init;
    editor.shutdown = shutdown;
    editor.show = show;
    editor.start = start;
    editor.stop = stop;
    editor.reset = reset;
    editor.wait_for_interrupt = wait_for_interrupt;
    editor.add_nanovdb = add_nanovdb;
    editor.add_array = add_array;
    editor.add_shader_params = add_shader_params;
    editor.sync_shader_params = sync_shader_params;
    editor.add_gaussian_data = add_gaussian_data;
    editor.update_camera = update_camera;
    editor.add_camera_view = add_camera_view;
    editor.get_resolved_port = get_resolved_port;

    // New token-based API
    editor.get_camera = get_camera;
    editor.get_camera_2 = get_camera_2;
    editor.get_token = get_token;
    editor.add_nanovdb_2 = add_nanovdb_2;
    editor.add_gaussian_data_2 = add_gaussian_data_2;
    editor.add_camera_view_2 = add_camera_view_2;
    editor.update_camera_2 = update_camera_2;
    editor.remove = remove;
    editor.map_params = map_params;
    editor.unmap_params = unmap_params;
    editor.set_pipeline = set_pipeline;
    editor.get_pipeline = get_pipeline;
    editor.get_pipeline_type = get_pipeline_type;
    editor.mark_pipeline_dirty = mark_pipeline_dirty;
    editor.add_nanovdb_3 = add_nanovdb_3;
    editor.add_gaussian_data_3 = add_gaussian_data_3;
    editor.add_gaussian_data_4 = add_gaussian_data_4;
    editor.set_visible = set_visible;
    editor.get_visible = get_visible;
    editor.add_named_array = add_named_array;
    editor.get_named_array = get_named_array;
    editor.map_pipeline_params = map_pipeline_params;
    editor.unmap_pipeline_params = unmap_pipeline_params;
    editor.set_custom_scene_params = set_custom_scene_params;
    editor.get_custom_scene_params_data_type = get_custom_scene_params_data_type;
    editor.get_process_step_count = get_process_step_count;
    editor.get_process_step = get_process_step;
    editor.set_process_step = set_process_step;
    editor.map_process_step_params = map_process_step_params;
    editor.unmap_process_step_params = unmap_process_step_params;
    editor.load_scene = load_scene;
    editor.save_scene = save_scene;

    return &editor;
}
}
