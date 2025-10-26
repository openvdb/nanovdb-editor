// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/editor/Editor.cpp

    \author Petra Hapalova

    \brief
*/

#include "Editor.h"
#include "EditorToken.h"
#include "EditorSceneManager.h"

#include "ShaderMonitor.h"
#include "Console.h"
#include "Profiler.h"
#include "ShaderCompileUtils.h"
#include "EditorScene.h"
#include "ImguiInstance.h"
#include "RenderSettingsConfig.h"

#include "imgui/ImguiWindow.h"
#include "imgui/UploadBuffer.h"

#include "compute/ComputeShader.h"
#include "raster/Raster.h"

#include "nanovdb_editor/putil/WorkerThread.hpp"

#include <filesystem>


static const pnanovdb_uint32_t s_default_width = 1440u;
static const pnanovdb_uint32_t s_default_height = 720u;

static const char* s_raster_file = "";

namespace pnanovdb_editor
{

template <typename WorkerOp, typename ImmediateOp>
static void dispatch_worker_or_immediate(pnanovdb_editor_t* editor, WorkerOp worker_op, ImmediateOp immediate_op)
{
    if (editor->impl->editor_worker)
    {
        worker_op(editor->impl->editor_worker);
    }
    else
    {
        immediate_op();
    }
}

// mirrored from shader
struct EditorParams
{
    pnanovdb_camera_mat_t view_inv;
    pnanovdb_camera_mat_t projection_inv;
    pnanovdb_camera_mat_t view;
    pnanovdb_camera_mat_t projection;
    uint32_t width;
    uint32_t height;
    uint32_t pad1;
    uint32_t pad2;
};

struct GaussianDataDeleter
{
    pnanovdb_editor_t* editor = nullptr;

    void operator()(pnanovdb_raster_gaussian_data_t* data) const
    {
        if (editor && editor->impl && editor->impl->raster && editor->impl->device_queue && data)
        {
            editor->impl->raster->destroy_gaussian_data(editor->impl->raster->compute, editor->impl->device_queue, data);
        }
        else
        {
            if (editor && editor->impl && editor->impl->compute)
            {
                auto* queue = editor->impl->device_queue;
                auto* compute = editor->impl->compute;
                pnanovdb_compute_interface_t* compute_interface = nullptr;
                pnanovdb_compute_context_t* compute_context = nullptr;

                if (queue)
                {
                    compute_interface = compute->device_interface.get_compute_interface(queue);
                    compute_context = compute->device_interface.get_compute_context(queue);
                }

                if (compute_interface && compute_context)
                {
                    auto log_print = compute_interface->get_log_print(compute_context);
                    if (log_print)
                    {
                        log_print(
                            PNANOVDB_COMPUTE_LOG_LEVEL_WARNING,
                            "GaussianDataDeleter: cannot destroy gaussian data (editor:%p impl:%p raster:%p queue:%p data:%p)",
                            editor, editor ? editor->impl : nullptr,
                            (editor && editor->impl) ? editor->impl->raster : nullptr,
                            (editor && editor->impl) ? editor->impl->device_queue : nullptr, data);
                    }
                }
            }
        }
    }
};

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
    editor->impl->editor_worker = NULL;
    editor->impl->gaussian_data = NULL;
    editor->impl->nanovdb_array = NULL;
    editor->impl->data_array = NULL;
    editor->impl->camera = NULL;
    editor->impl->camera_view = NULL;
    editor->impl->raster_ctx = NULL;
    editor->impl->shader_params = NULL;
    editor->impl->shader_params_data_type = NULL;
    editor->impl->scene_view = NULL;
    editor->impl->resolved_port = PNANOVDB_EDITOR_RESOLVED_PORT_PENDING;
    editor->impl->scene_manager = NULL;
    editor->impl->device = NULL;
    editor->impl->device_queue = NULL;
    editor->impl->compute_queue = NULL;

    return PNANOVDB_TRUE;
}

void init(pnanovdb_editor_t* editor)
{
    editor->impl->scene_manager = new EditorSceneManager();
    editor->impl->scene_view = new SceneView();

    editor->impl->raster = new pnanovdb_raster_t();
    pnanovdb_raster_load(editor->impl->raster, editor->impl->compute);
}

void shutdown(pnanovdb_editor_t* editor)
{
    if (editor->impl->editor_worker)
    {
        editor->stop(editor);
    }
    if (editor->impl->scene_view)
    {
        delete editor->impl->scene_view;
        editor->impl->scene_view = nullptr;
    }
    if (editor->impl->scene_manager)
    {
        delete editor->impl->scene_manager;
        editor->impl->scene_manager = nullptr;
    }
    if (editor->impl->raster)
    {
        pnanovdb_raster_free(editor->impl->raster);
        delete editor->impl->raster;
        editor->impl->raster = nullptr;
    }
    if (editor->impl->camera)
    {
        delete editor->impl->camera;
        editor->impl->camera = nullptr;
    }
    if (editor->impl)
    {
        delete editor->impl;
        editor->impl = nullptr;
    }
}

void add_nanovdb(pnanovdb_editor_t* editor, pnanovdb_compute_array_t* nanovdb_array)
{
    Console::getInstance().addLog("[OBSOLETE API] add_nanovdb() is deprecated and no longer supported.");
    Console::getInstance().addLog(
        "[OBSOLETE API] Please use the new token-based API: add_nanovdb_2(editor, scene, name, array)");
    Console::getInstance().addLog("[OBSOLETE API] This ensures proper ownership management and multi-scene support.");

    // Do nothing - old API is no longer supported
    (void)editor;
    (void)nanovdb_array;
}

void add_array(pnanovdb_editor_t* editor, pnanovdb_compute_array_t* data_array)
{
    Console::getInstance().addLog("[OBSOLETE API] add_array() is deprecated and no longer supported.");
    Console::getInstance().addLog(
        "[OBSOLETE API] Please migrate to the new token-based API for proper resource management.");

    // Do nothing - old API is no longer supported
    (void)editor;
    (void)data_array;
}
void add_gaussian_data(pnanovdb_editor_t* editor,
                       pnanovdb_raster_context_t* raster_ctx,
                       pnanovdb_compute_queue_t* queue,
                       pnanovdb_raster_gaussian_data_t* gaussian_data)
{
    Console::getInstance().addLog("[OBSOLETE API] add_gaussian_data() is deprecated and no longer supported.");
    Console::getInstance().addLog(
        "[OBSOLETE API] Please use the new token-based API: add_gaussian_data_2(editor, scene, name, desc)");
    Console::getInstance().addLog(
        "[OBSOLETE API] This ensures proper ownership management, deferred destruction, and multi-scene support.");

    // Do nothing - old API is no longer supported
    (void)editor;
    (void)raster_ctx;
    (void)queue;
    (void)gaussian_data;
}

void update_camera(pnanovdb_editor_t* editor, pnanovdb_camera_t* camera)
{
    // Deprecated API: prefer token-based update_camera_2
    Console::getInstance().addLog(
        "[OBSOLETE API] update_camera() is deprecated and no longer supported. Use update_camera_2(editor, scene, camera).");

    // Do nothing - old API is no longer supported
    (void)editor;
    (void)camera;
}

void add_camera_view(pnanovdb_editor_t* editor, pnanovdb_camera_view_t* camera)
{
    Console::getInstance().addLog("[OBSOLETE API] add_camera_view() is deprecated and no longer supported.");
    Console::getInstance().addLog(
        "[OBSOLETE API] Please use the new token-based API: add_camera_view_2(editor, scene, camera)");
    Console::getInstance().addLog("[OBSOLETE API] This ensures proper ownership management and multi-scene support.");

    // Do nothing - old API is no longer supported
    (void)editor;
    (void)camera;
}

void add_shader_params(pnanovdb_editor_t* editor, void* params, const pnanovdb_reflect_data_type_t* data_type)
{
    Console::getInstance().addLog("[OBSOLETE API] add_shader_params() is deprecated and no longer supported.");
    Console::getInstance().addLog("[OBSOLETE API] Shader params are now managed automatically per scene object.");
    Console::getInstance().addLog("[OBSOLETE API] Use map_params/unmap_params with token-based API to modify parameters.");

    // Do nothing - old API is no longer supported
    (void)editor;
    (void)params;
    (void)data_type;
}

void sync_shader_params(pnanovdb_editor_t* editor, void* shader_params, pnanovdb_bool_t set_data)
{
    Console::getInstance().addLog("[OBSOLETE API] sync_shader_params() is deprecated and no longer supported.");
    Console::getInstance().addLog("[OBSOLETE API] Use map_params/unmap_params with token-based API to modify parameters.");

    // Do nothing - old API is no longer supported
    (void)editor;
    (void)shader_params;
    (void)set_data;
}

pnanovdb_int32_t editor_get_external_active_count(void* external_active_count)
{
    auto editor = static_cast<pnanovdb_editor_t*>(external_active_count);
    if (!editor->impl || !editor->impl->editor_worker)
    {
        return 0;
    }

    auto worker = editor->impl->editor_worker;
    pnanovdb_int32_t count = 0;
    if (worker->should_stop.load())
    {
        count = 1;
    }
    return count;
};

void show(pnanovdb_editor_t* editor, pnanovdb_compute_device_t* device, pnanovdb_editor_config_t* config)
{
    if (!editor->impl->compute || !editor->impl->compiler || !device || !config)
    {
        return;
    }

    pnanovdb_int32_t image_width = s_default_width;
    pnanovdb_int32_t image_height = s_default_height;

    const char* profile_name =
        config->ui_profile_name ? config->ui_profile_name : imgui_instance_user::s_render_settings_default;
    int saved_width = 0, saved_height = 0;
    if (imgui_instance_user::ini_window_resolution(profile_name, &saved_width, &saved_height))
    {
        if (saved_width > 0 && saved_height > 0)
        {
            image_width = saved_width;
            image_height = saved_height;
        }
    }

    pnanovdb_imgui_window_interface_t* imgui_window_iface = pnanovdb_imgui_get_window_interface();
    if (!imgui_window_iface)
    {
        return;
    }
    pnanovdb_imgui_settings_render_t* imgui_user_settings = nullptr;
    pnanovdb_imgui_instance_interface_t* imgui_instance_iface = get_user_imgui_instance_interface();
    imgui_instance_user::Instance* imgui_user_instance = nullptr;
    void* imgui_instance_userdata = &imgui_user_instance;
    pnanovdb_imgui_window_t* imgui_window = imgui_window_iface->create(
        editor->impl->compute, device, image_width, image_height, (void**)&imgui_user_settings, PNANOVDB_FALSE,
        &imgui_instance_iface, &imgui_instance_userdata, 1u, config->headless);

    if (!imgui_window || !imgui_user_settings)
    {
        return;
    }

    if (editor->impl->camera)
    {
        imgui_user_settings->camera_state = editor->impl->camera->state;
        imgui_user_settings->camera_config = editor->impl->camera->config;
        imgui_user_settings->sync_camera = PNANOVDB_TRUE;
    }

    imgui_instance_user::RenderSettingsConfig render_config;
    render_config.load(*config);
    render_config.applyToSettings(*imgui_user_settings);

    // Automatically enable encoder when streaming is requested
    if (config->streaming || config->stream_to_file)
    {
        imgui_user_settings->enable_encoder = PNANOVDB_TRUE;
    }

    if (!imgui_user_instance)
    {
        return;
    }

    imgui_user_instance->device_index = editor->impl->compute->device_interface.get_device_index(device);
    pnanovdb_compiler_settings_init(&imgui_user_instance->compiler_settings);

    pnanovdb_compiler_instance_t* compiler_inst = editor->impl->compiler->create_instance();
    pnanovdb_compute_queue_t* device_queue = editor->impl->compute->device_interface.get_device_queue(device);
    // used for a worker thread
    pnanovdb_compute_queue_t* compute_queue = editor->impl->compute->device_interface.get_compute_queue(device);
    pnanovdb_compute_interface_t* compute_interface =
        editor->impl->compute->device_interface.get_compute_interface(device_queue);
    pnanovdb_compute_context_t* compute_context =
        editor->impl->compute->device_interface.get_compute_context(device_queue);

    editor->impl->device = device;
    editor->impl->device_queue = device_queue;
    editor->impl->compute_queue = compute_queue;

    // Create raster context once - never recreate or destroy it during runtime
    if (!editor->impl->raster_ctx && editor->impl->raster)
    {
        editor->impl->raster_ctx = editor->impl->raster->create_context(editor->impl->raster->compute, device_queue);
    }

    // Create and set default scene with proper is_y_up setting from render settings
    if (SceneView* views = editor->impl->scene_view)
    {
        pnanovdb_editor_token_t* default_scene = EditorToken::getInstance().getToken(pnanovdb_editor::DEFAULT_SCENE_NAME);
        views->get_or_create_scene(default_scene);

        // Set current scene if no current scene is set
        if (!views->get_current_scene_token())
        {
            views->set_current_scene(default_scene);
        }
    }

    pnanovdb_camera_mat_t view = {};
    pnanovdb_camera_mat_t projection = {};

    pnanovdb_compute_upload_buffer_t compute_upload_buffer;
    pnanovdb_compute_upload_buffer_init(compute_interface, compute_context, &compute_upload_buffer,
                                        PNANOVDB_COMPUTE_BUFFER_USAGE_CONSTANT, PNANOVDB_COMPUTE_FORMAT_UNKNOWN, 0u);

    pnanovdb_compute_upload_buffer_t shader_params_upload_buffer;
    pnanovdb_compute_upload_buffer_init(compute_interface, compute_context, &shader_params_upload_buffer,
                                        PNANOVDB_COMPUTE_BUFFER_USAGE_CONSTANT, PNANOVDB_COMPUTE_FORMAT_UNKNOWN, 0u);

    pnanovdb_compute_texture_t* background_image = nullptr;

    pnanovdb_shader_context_t* shader_context = nullptr;
    pnanovdb_compute_buffer_t* nanovdb_buffer = nullptr;
    pnanovdb_compute_array_t* uploaded_nanovdb_array = nullptr;

    pnanovdb_util::WorkerThread raster_worker;
    pnanovdb_util::WorkerThread::TaskId raster_task_id = pnanovdb_util::WorkerThread::invalidTaskId();
    std::string pending_raster_filepath;
    float pending_voxel_size = 1.f / 128.f;
    pnanovdb_raster_gaussian_data_t* pending_gaussian_data = nullptr;
    pnanovdb_raster_context_t* pending_raster_ctx = nullptr;
    pnanovdb_raster_shader_params_t* pending_raster_params = nullptr;
    pnanovdb_compute_array_t* pending_nanovdb_array = nullptr;
    pnanovdb_compute_array_t* pending_shader_params_arrays[pnanovdb_raster::shader_param_count];
    for (pnanovdb_uint32_t idx = 0u; idx < pnanovdb_raster::shader_param_count; idx++)
    {
        pending_shader_params_arrays[idx] = nullptr;
    }

    // raster 2d shader params data type needed for initialization
    const pnanovdb_reflect_data_type_t* raster_shader_params_data_type =
        PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_raster_shader_params_t);
    const pnanovdb_raster_shader_params_t* default_raster_shader_params =
        (const pnanovdb_raster_shader_params_t*)raster_shader_params_data_type->default_value;
    pnanovdb_raster_shader_params_t init_raster_shader_params = *default_raster_shader_params;

    editor->impl->compute->device_interface.enable_profiler(
        compute_context, (void*)"editor", pnanovdb_editor::Profiler::report_callback);
    editor->impl->compute->device_interface.get_memory_stats(device, Profiler::getInstance().getMemoryStats());

    if (editor->impl->nanovdb_array && editor->impl->nanovdb_array->filepath)
    {
        imgui_user_instance->nanovdb_filepath = editor->impl->nanovdb_array->filepath;
    }

    imgui_user_instance->raster_filepath = std::string(s_raster_file);

    imgui_user_instance->compiler = editor->impl->compiler;
    imgui_user_instance->compute = editor->impl->compute;

    bool dispatch_shader = true;

    editor->impl->compiler->set_diagnostic_callback(compiler_inst,
                                                    [](const char* message)
                                                    {
                                                        if (message && message[0] != '\0')
                                                        {
                                                            pnanovdb_editor::Console::getInstance().addLog("%s", message);
                                                        }
                                                    });

    pnanovdb_editor::EditorSceneConfig scene_config{ imgui_user_instance, editor,
                                                     imgui_user_settings, device_queue,
                                                     compiler_inst,       imgui_user_instance->shader_name.c_str() };
    EditorScene editor_scene(scene_config);

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

    if (imgui_user_instance->viewport_option == imgui_instance_user::ViewportOption::Last)
    {
        if (editor->impl->gaussian_data)
        {
            imgui_user_instance->viewport_option = imgui_instance_user::ViewportOption::Raster2D;
            editor->impl->nanovdb_array = nullptr;
        }
        else
        {
            imgui_user_instance->viewport_option = imgui_instance_user::ViewportOption::NanoVDB;
            editor->impl->gaussian_data = nullptr;
        }
    }

    if (editor->impl->editor_worker)
    {
        // Signal that the render loop has started
        editor->impl->editor_worker->is_starting.store(false);
        auto log_print = compute_interface->get_log_print(compute_context);
        if (log_print)
        {
            log_print(PNANOVDB_COMPUTE_LOG_LEVEL_INFO, "Render loop started\n");
        }
    }

    bool should_run = true;
    while (should_run)
    {
        if (editor->impl->editor_worker && editor->impl->editor_worker->should_stop.load())
        {
            auto log_print = compute_interface->get_log_print(compute_context);
            if (log_print)
            {
                log_print(PNANOVDB_COMPUTE_LOG_LEVEL_INFO, "Render loop stopped\n");
            }
            should_run = false;
            break;
        }

        // pending raster data deleted next frame after being replaced
        std::shared_ptr<pnanovdb_raster_gaussian_data_t> old_gaussian_data_ptr = nullptr;

        imgui_window_iface->get_camera_view_proj(imgui_window, &image_width, &image_height, &view, &projection);
        pnanovdb_camera_mat_t view_inv = pnanovdb_camera_mat_inverse(view);
        pnanovdb_camera_mat_t projection_inv = pnanovdb_camera_mat_inverse(projection);

        // update memory stats periodically
        if (imgui_user_instance && imgui_user_instance->pending.update_memory_stats)
        {
            editor->impl->compute->device_interface.get_memory_stats(device, Profiler::getInstance().getMemoryStats());
            imgui_user_instance->pending.update_memory_stats = false;
        }

        create_background();

        editor_scene.process_pending_editor_changes();
        editor_scene.process_pending_ui_changes();

        // handle raster completion (check before enqueuing new tasks)
        if (raster_worker.isTaskRunning(raster_task_id))
        {
            imgui_user_instance->progress.text = raster_worker.getTaskProgressText(raster_task_id);
            imgui_user_instance->progress.value = raster_worker.getTaskProgress(raster_task_id);
        }
        else if (raster_worker.isTaskCompleted(raster_task_id))
        {
            if (raster_worker.isTaskSuccessful(raster_task_id))
            {
                if (imgui_user_instance->viewport_option == imgui_instance_user::ViewportOption::NanoVDB)
                {
                    editor_scene.handle_nanovdb_data_load(pending_nanovdb_array, pending_raster_filepath.c_str());
                }
                else if (imgui_user_instance->viewport_option == imgui_instance_user::ViewportOption::Raster2D)
                {
                    editor_scene.handle_gaussian_data_load(pending_gaussian_data, pending_raster_params,
                                                           pending_raster_filepath.c_str(), old_gaussian_data_ptr);
                }
                pnanovdb_editor::Console::getInstance().addLog(
                    "Rasterization of '%s' was successful", pending_raster_filepath.c_str());

                // Re-sync to pick up the newly selected view for immediate rendering
                editor_scene.sync_selected_view_with_current();
            }
            else
            {
                pnanovdb_editor::Console::getInstance().addLog(
                    "Rasterization of '%s' failed", pending_raster_filepath.c_str());
            }

            // Clean up temporary worker thread raster context (if created during rasterization)
            if (pending_raster_ctx)
            {
                editor->impl->raster->destroy_context(editor->impl->compute, device_queue, pending_raster_ctx);
                pending_raster_ctx = nullptr;
            }

            pending_raster_filepath = "";
            raster_worker.removeCompletedTask(raster_task_id);

            imgui_user_instance->progress.reset();
        }

        // update scene
        editor_scene.sync_selected_view_with_current();
        editor_scene.sync_shader_params_from_editor();

        // update raster (enqueue new rasterization tasks)
        if (imgui_user_instance->pending.update_raster)
        {
            imgui_user_instance->pending.update_raster = false;

            if (raster_worker.hasRunningTask())
            {
                pnanovdb_editor::Console::getInstance().addLog(
                    "Error: Rasterization already in progress", imgui_user_instance->raster_filepath.c_str());
            }
            else
            {
                pending_raster_filepath = imgui_user_instance->raster_filepath;
                pending_voxel_size = 1.f / imgui_user_instance->raster_voxels_per_unit;

                // get user params for the raster shader
                editor->impl->compute->destroy_array(
                    pending_shader_params_arrays[pnanovdb_raster::gaussian_frag_color_slang]);

                pending_shader_params_arrays[pnanovdb_raster::gaussian_frag_color_slang] =
                    editor->impl->scene_manager->shader_params.get_compute_array_for_shader(
                        "raster/gaussian_frag_color.slang", editor->impl->compute);

                // create new default params
                pending_raster_params = &init_raster_shader_params;
                pending_raster_params->name = nullptr;
                pending_raster_params->data_type = raster_shader_params_data_type;

                pnanovdb_compute_queue_t* worker_queue =
                    imgui_user_instance->viewport_option == imgui_instance_user::ViewportOption::NanoVDB ? compute_queue :
                                                                                                           device_queue;

                raster_task_id = raster_worker.enqueue(
                    [&raster_worker](
                        pnanovdb_raster_t* raster, const pnanovdb_compute_t* compute, pnanovdb_compute_queue_t* queue,
                        const char* filepath, float voxel_size, pnanovdb_compute_array_t** nanovdb_array,
                        pnanovdb_raster_gaussian_data_t** gaussian_data, pnanovdb_raster_context_t** raster_context,
                        pnanovdb_compute_array_t** shader_params_arrays, pnanovdb_raster_shader_params_t* raster_params,
                        pnanovdb_profiler_report_t profiler) -> bool
                    {
                        return raster->raster_file(raster, compute, queue, filepath, voxel_size, nanovdb_array,
                                                   gaussian_data, raster_context, shader_params_arrays, raster_params,
                                                   profiler, (void*)(&raster_worker));
                    },
                    editor->impl->raster, editor->impl->compute, worker_queue, pending_raster_filepath.c_str(),
                    pending_voxel_size,
                    imgui_user_instance->viewport_option == imgui_instance_user::ViewportOption::NanoVDB ?
                        &pending_nanovdb_array :
                        nullptr,
                    imgui_user_instance->viewport_option == imgui_instance_user::ViewportOption::Raster2D ?
                        &pending_gaussian_data :
                        nullptr,
                    imgui_user_instance->viewport_option == imgui_instance_user::ViewportOption::Raster2D ?
                        &pending_raster_ctx :
                        nullptr,
                    pending_shader_params_arrays, pending_raster_params, pnanovdb_editor::Profiler::report_callback);

                pnanovdb_editor::Console::getInstance().addLog(
                    "Running rasterization: '%s'...", pending_raster_filepath.c_str());
            }
        }

        // Rendering
        if (editor->impl->nanovdb_array)
        {
            if (imgui_user_instance->pending.update_shader)
            {
                std::lock_guard<std::mutex> lock(imgui_user_instance->compiler_settings_mutex);

                imgui_user_instance->pending.update_shader = false;
                editor->impl->compute->destroy_shader_context(editor->impl->compute, device_queue, shader_context);
                shader_context = editor->impl->compute->create_shader_context(editor->impl->shader_name.c_str());
                if (editor->impl->compute->init_shader(editor->impl->compute, device_queue, shader_context,
                                                       &imgui_user_instance->compiler_settings) == PNANOVDB_FALSE)
                {
                    // compilation has failed, don't dispatch the shader
                    dispatch_shader = false;
                    cleanup_background();
                }
                else
                {
                    // now that shader definitely exists, reload shader params
                    editor_scene.reload_shader_params_for_current_view();
                    // Refresh params for all views using this shader that may have been created before JSON existed
                    if (auto* sm = editor_scene.get_scene_manager())
                    {
                        sm->refresh_params_for_shader(editor->impl->compute, editor->impl->shader_name.c_str());
                    }

                    dispatch_shader = true;
                }
            }
            if (dispatch_shader)
            {
                EditorParams editor_params = {};
                editor_params.view_inv = pnanovdb_camera_mat_transpose(view_inv);
                editor_params.projection_inv = pnanovdb_camera_mat_transpose(projection_inv);
                editor_params.view = pnanovdb_camera_mat_transpose(view);
                editor_params.projection = pnanovdb_camera_mat_transpose(projection);
                editor_params.width = image_width;
                editor_params.height = image_height;

                EditorParams* mapped = (EditorParams*)pnanovdb_compute_upload_buffer_map(
                    compute_context, &compute_upload_buffer, sizeof(EditorParams));
                *mapped = editor_params;
                auto* upload_transient = pnanovdb_compute_upload_buffer_unmap(compute_context, &compute_upload_buffer);

                void* shader_params_data = pnanovdb_compute_upload_buffer_map(
                    compute_context, &shader_params_upload_buffer, PNANOVDB_COMPUTE_CONSTANT_BUFFER_MAX_SIZE);
                editor_scene.get_shader_params_for_current_view(shader_params_data);

                auto* shader_upload_transient =
                    pnanovdb_compute_upload_buffer_unmap(compute_context, &shader_params_upload_buffer);

                if (editor->impl->nanovdb_array != uploaded_nanovdb_array && nanovdb_buffer)
                {
                    compute_interface->destroy_buffer(compute_context, nanovdb_buffer);
                    nanovdb_buffer = nullptr;
                }

                pnanovdb_compute_buffer_transient_t* readback_transient = nullptr;
                editor->impl->compute->dispatch_shader_on_nanovdb_array(
                    editor->impl->compute, device, shader_context, editor->impl->nanovdb_array, image_width, image_height,
                    background_image, upload_transient, shader_upload_transient, &nanovdb_buffer, &readback_transient);

                if (nanovdb_buffer)
                {
                    uploaded_nanovdb_array = editor->impl->nanovdb_array;
                }
            }
            else
            {
                cleanup_background();
            }
        }
        else if (editor->impl->gaussian_data && editor->impl->raster_ctx)
        {
            pnanovdb_raster_shader_params_t raster_params = {};
            editor_scene.get_shader_params_for_current_view(&raster_params);

            editor->impl->raster->raster_gaussian_2d(
                editor->impl->raster->compute, device_queue, editor->impl->raster_ctx, editor->impl->gaussian_data,
                background_image, image_width, image_height, &view, &projection, &raster_params);
        }
        else
        {
            cleanup_background();
        }

        // 3-frame destruction pipeline for gaussian data:
        {
            // This prevents GPU from accessing freed memory by deferring destruction for 3 frames
            // 1. Destroy items in ready queue (have been pending for 3 frames)
            if (!editor->impl->gaussian_data_destruction_queue_ready.empty())
            {
                editor->impl->gaussian_data_destruction_queue_ready.clear();
            }

            // 2. Move pending queue to ready queue (advance pipeline)
            if (!editor->impl->gaussian_data_destruction_queue_pending.empty())
            {
                editor->impl->gaussian_data_destruction_queue_ready =
                    std::move(editor->impl->gaussian_data_destruction_queue_pending);
                editor->impl->gaussian_data_destruction_queue_pending.clear();
            }

            // 3. Move gaussian_data_old to pending queue (start of pipeline)
            if (editor->impl->gaussian_data_old)
            {
                editor->impl->gaussian_data_destruction_queue_pending.push_back(
                    std::move(editor->impl->gaussian_data_old));
            }
            if (editor->impl->camera && imgui_user_settings->sync_camera == PNANOVDB_FALSE)
            {
                imgui_window_iface->get_camera(imgui_window, &editor->impl->camera->state, &editor->impl->camera->config);

                // Sync the updated camera back to the scene's viewport camera for properties display
                editor_scene.sync_scene_camera_from_editor();
            }
        }

        imgui_window_iface->update_camera(imgui_window, imgui_user_settings);

        // update viewport image
        should_run = imgui_window_iface->update(
            editor->impl->compute, device_queue,
            background_image ? compute_interface->register_texture_as_transient(compute_context, background_image) :
                               nullptr,
            &image_width, &image_height, &editor->impl->resolved_port, imgui_window, imgui_user_settings,
            editor_get_external_active_count, editor);

        if (background_image)
        {
            compute_interface->destroy_texture(compute_context, background_image);
        }
    }
    editor->impl->compute->device_interface.wait_idle(device_queue);
    editor->impl->compute->device_interface.disable_profiler(compute_context);
    editor->impl->compute->destroy_shader(
        compute_interface, &editor->impl->compute->shader_interface, compute_context, shader_context);
    editor->impl->compiler->destroy_instance(compiler_inst);

    pnanovdb_compute_upload_buffer_destroy(compute_context, &compute_upload_buffer);
    pnanovdb_compute_upload_buffer_destroy(compute_context, &shader_params_upload_buffer);

    imgui_window_iface->destroy(editor->impl->compute, device_queue, imgui_window, imgui_user_settings);

    if (editor->impl->raster_ctx)
    {
        editor->impl->raster->destroy_context(editor->impl->compute, device_queue, editor->impl->raster_ctx);
        editor->impl->raster_ctx = nullptr;
    }
    if (editor->impl->raster)
    {
        editor->impl->raster = nullptr;
    }
    if (editor->impl->device_queue)
    {
        editor->impl->device_queue = nullptr;
    }
    if (editor->impl->compute_queue)
    {
        editor->impl->compute_queue = nullptr;
    }
    if (editor->impl->device)
    {
        editor->impl->device = nullptr;
    }
}

pnanovdb_int32_t get_resolved_port(pnanovdb_editor_t* editor, pnanovdb_bool_t should_wait)
{
    auto port_atomic = reinterpret_cast<std::atomic<pnanovdb_int32_t>*>(&editor->impl->resolved_port);
    while (should_wait && port_atomic->load() == PNANOVDB_EDITOR_RESOLVED_PORT_PENDING)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return port_atomic->load();
}

void start(pnanovdb_editor_t* editor, pnanovdb_compute_device_t* device, pnanovdb_editor_config_t* config)
{
    // cache config
    if (editor->impl)
    {
        editor->impl->config = *config;
        editor->impl->config_ip_address = config->ip_address ? std::string(config->ip_address) : std::string();
        editor->impl->config_ui_profile_name =
            config->ui_profile_name ? std::string(config->ui_profile_name) : std::string();
        editor->impl->config.ip_address = editor->impl->config_ip_address.c_str();
        editor->impl->config.ui_profile_name = editor->impl->config_ui_profile_name.c_str();
    }

    if (config->headless)
    {
        if (editor->impl->editor_worker)
        {
            return;
        }

        auto* editor_worker = new EditorWorker();

        // to be safe, we must make a deep copy of config
        editor_worker->config = *config;
        editor_worker->config_ip_address = config->ip_address ? std::string(config->ip_address) : std::string();
        editor_worker->config_ui_profile_name =
            config->ui_profile_name ? std::string(config->ui_profile_name) : std::string();
        editor_worker->config.ip_address = editor_worker->config_ip_address.c_str();
        editor_worker->config.ui_profile_name = editor_worker->config_ui_profile_name.c_str();

        editor_worker->thread =
            new std::thread([editor, device, editor_worker]() { editor->show(editor, device, &editor_worker->config); });
        editor->impl->editor_worker = editor_worker;
    }
    else
    {
        editor->show(editor, device, config);
    }
}

void stop(pnanovdb_editor_t* editor)
{
    if (!editor->impl->editor_worker)
    {
        return;
    }

    auto* editor_worker = editor->impl->editor_worker;
    editor_worker->should_stop.store(true);
    editor_worker->thread->join();
    delete editor_worker->thread;
    delete editor_worker;
    editor->impl->editor_worker = nullptr;
}

void reset(pnanovdb_editor_t* editor)
{
    auto device = editor->impl->device;
    auto compute = editor->impl->compute;
    auto compiler = editor->impl->compiler;

    pnanovdb_editor_config_t config = editor->impl->config;
    std::string config_ip_address = editor->impl->config_ip_address;
    std::string config_ui_profile_name = editor->impl->config_ui_profile_name;
    config.ip_address = config_ip_address.c_str();
    config.ui_profile_name = config_ui_profile_name.c_str();

    shutdown(editor);

    init_impl(editor, compute, compiler);

    start(editor, device, &config);
}

pnanovdb_camera_t* get_camera(pnanovdb_editor_t* editor, pnanovdb_editor_token_t* scene)
{
    if (!editor || !scene)
    {
        return nullptr;
    }

    // Note: This is a fallback for compatibility - ideally update_camera_2 should be called explicitly
    // If no explicit camera has been set via update_camera_2, we return the viewport camera
    // to ensure get_camera always returns a valid camera pointer for the scene

    // If camera has been explicitly set via update_camera_2, return it
    if (editor->impl->camera)
    {
        return editor->impl->camera;
    }

    // Otherwise, try to get the viewport camera from the scene
    if (editor->impl->scene_view)
    {
        pnanovdb_editor_token_t* viewport_token = editor->impl->scene_view->get_viewport_camera_token();
        pnanovdb_camera_view_t* viewport_view = editor->impl->scene_view->get_camera(scene, viewport_token);
        if (viewport_view && viewport_view->configs && viewport_view->states && viewport_view->num_cameras > 0)
        {
            // Create a temporary camera object from the viewport camera
            if (!editor->impl->camera)
            {
                editor->impl->camera = new pnanovdb_camera_t();
                pnanovdb_camera_init(editor->impl->camera);
            }
            editor->impl->camera->config = viewport_view->configs[0];
            editor->impl->camera->state = viewport_view->states[0];
            return editor->impl->camera;
        }
    }

    return nullptr;
}

// Temp: Token management
static std::unordered_map<std::string, pnanovdb_editor_token_t*> s_token_registry;
static std::mutex s_token_mutex;

pnanovdb_editor_token_t* get_token(const char* name)
{
    return EditorToken::getInstance().getToken(name);
}

// Helper function to check if two tokens are equal
static inline bool tokens_equal(pnanovdb_editor_token_t* a, pnanovdb_editor_token_t* b)
{
    if (a == b)
        return true;
    if (!a || !b)
        return false;
    return a->id == b->id;
}

// Helper function to get string from token (safe)
static inline const char* token_to_string(pnanovdb_editor_token_t* token)
{
    return token ? token->str : "<null>";
}

void add_nanovdb_2(pnanovdb_editor_t* editor,
                   pnanovdb_editor_token_t* scene,
                   pnanovdb_editor_token_t* name,
                   pnanovdb_compute_array_t* array)
{
    if (!editor || !editor->impl || !scene || !name || !array)
    {
        return;
    }

    Console::getInstance().addLog("[API] add_nanovdb_2: scene='%s' (id=%llu), name='%s' (id=%llu)",
                                  token_to_string(scene), (unsigned long long)scene->id, token_to_string(name),
                                  (unsigned long long)name->id);

    // Pre-create params array initialized from JSON
    pnanovdb_compute_array_t* params_array = editor->impl->scene_manager->create_initialized_shader_params(
        editor->impl->compute, editor->impl->shader_name.c_str(), nullptr, PNANOVDB_COMPUTE_CONSTANT_BUFFER_MAX_SIZE);

    editor->impl->scene_manager->add_nanovdb(
        scene, name, array, params_array, editor->impl->compute, editor->impl->shader_name.c_str());

    Console::getInstance().addLog("[API] Added NanoVDB '%s' to scene '%s'", name->str, scene->str);

    dispatch_worker_or_immediate(
        editor,
        // Worker mode: queue for render thread
        [&](EditorWorker* worker)
        {
            worker->pending_nanovdb.set_pending(array);
            worker->last_added_scene_token_id.store(scene->id, std::memory_order_relaxed);
            worker->last_added_name_token_id.store(name->id, std::memory_order_relaxed);
            worker->views_need_sync.store(true);
        },
        // Non-worker mode: execute immediately
        [&]()
        {
            editor->impl->nanovdb_array = array;

            void* shader_params_ptr = nullptr;
            editor->impl->scene_manager->with_object(scene, name,
                                                     [&](SceneObject* obj)
                                                     {
                                                         if (obj)
                                                         {
                                                             shader_params_ptr = obj->shader_params;
                                                             editor->impl->shader_params = obj->shader_params;
                                                             editor->impl->shader_params_data_type = nullptr;
                                                         }
                                                     });

            if (SceneView* views = editor->impl->scene_view)
            {
                views->add_nanovdb_to_scene(scene, name, array, shader_params_ptr);
            }
        });
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

    Console::getInstance().addLog("[API] add_gaussian_data_2: scene='%s' (id=%llu), name='%s' (id=%llu)",
                                  token_to_string(scene), (unsigned long long)scene->id, token_to_string(name),
                                  (unsigned long long)name->id);

    if (!editor->impl->compute)
    {
        Console::getInstance().addLog("Error: No compute interface available");
        return;
    }

    pnanovdb_compute_device_t* device = editor->impl->device;
    pnanovdb_compute_queue_t* device_queue = editor->impl->device_queue;

    auto* worker = editor->impl->editor_worker;
    if (worker && (!device || !device_queue))
    {
        while (worker->is_starting.load())
        {
            if (worker->should_stop.load() || editor->impl->editor_worker != worker)
            {
                Console::getInstance().addLog("Worker not started; aborting wait due to stop/requested shutdown");
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        // Refresh device handles now that the worker initialized them
        device = editor->impl->device;
        device_queue = editor->impl->device_queue;
    }

    pnanovdb_raster_gaussian_data_t* gaussian_data = nullptr;

    pnanovdb_bool_t success = editor->impl->raster->create_gaussian_data_from_desc(
        editor->impl->raster, editor->impl->compute, device_queue, desc, name->str, &gaussian_data, nullptr, nullptr);

    if (success == PNANOVDB_FALSE || !gaussian_data)
    {
        Console::getInstance().addLog("[API] Error: Failed to create gaussian data from descriptor");
        return;
    }

    // Pre-create params array initialized from JSON
    const pnanovdb_reflect_data_type_t* raster_params_dt = PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_raster_shader_params_t);

    pnanovdb_compute_array_t* raster_params_array = editor->impl->scene_manager->create_initialized_shader_params(
        editor->impl->compute, pnanovdb_editor::s_raster2d_gaussian_shader, pnanovdb_editor::s_raster2d_shader_group,
        sizeof(pnanovdb_raster_shader_params_t), raster_params_dt);

    // Add with deferred destruction handling
    std::shared_ptr<pnanovdb_raster_gaussian_data_t> old_owner;
    editor->impl->scene_manager->add_gaussian_data(
        scene, name, gaussian_data, raster_params_array, raster_params_dt, editor->impl->compute, editor->impl->raster,
        editor->impl->device_queue, pnanovdb_editor::s_raster2d_gaussian_shader, &old_owner);

    // Chain old data through gaussian_data_old for deferred destruction
    if (old_owner)
    {
        if (editor->impl->gaussian_data_old)
        {
            // If gaussian_data_old already has data, add it to pending queue first
            editor->impl->gaussian_data_destruction_queue_pending.push_back(std::move(editor->impl->gaussian_data_old));
        }
        editor->impl->gaussian_data_old = std::move(old_owner);
    }

    Console::getInstance().addLog("[API] Added Gaussian data '%s' to scene '%s'", name->str, scene->str);

    dispatch_worker_or_immediate(
        editor,
        // Worker mode: queue for render thread
        [&](EditorWorker* worker)
        {
            worker->pending_gaussian_data.set_pending(gaussian_data);
            worker->pending_shader_params.set_pending(raster_params_array ? raster_params_array->data : nullptr);
            worker->pending_shader_params_data_type.set_pending(raster_params_dt);
            worker->last_added_scene_token_id.store(scene->id, std::memory_order_relaxed);
            worker->last_added_name_token_id.store(name->id, std::memory_order_relaxed);
            worker->views_need_sync.store(true);
        },
        // Non-worker mode: execute immediately
        [&]()
        {
            editor->impl->gaussian_data = gaussian_data;

            pnanovdb_raster_shader_params_t* shader_params_ptr = nullptr;
            editor->impl->scene_manager->with_object(scene, name,
                                                     [&](SceneObject* obj)
                                                     {
                                                         if (obj)
                                                         {
                                                             shader_params_ptr =
                                                                 (pnanovdb_raster_shader_params_t*)obj->shader_params;
                                                             editor->impl->shader_params = obj->shader_params;
                                                             editor->impl->shader_params_data_type = raster_params_dt;
                                                         }
                                                     });

            if (SceneView* views = editor->impl->scene_view)
            {
                views->add_gaussian_to_scene(scene, name, gaussian_data, shader_params_ptr);
            }
        });
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

    Console::getInstance().addLog("[API] add_camera_view_2: scene='%s' (id=%llu), camera='%s' (id=%llu)", scene->str,
                                  (unsigned long long)scene->id, camera->name->str, (unsigned long long)camera->name->id);

    editor->impl->scene_manager->add_camera(scene, camera->name, camera);

    dispatch_worker_or_immediate(
        editor,
        // Worker mode: queue for render thread
        [&](EditorWorker* worker) { worker->views_need_sync.store(true); },
        // Non-worker mode: execute immediately
        [&]()
        {
            if (SceneView* views = editor->impl->scene_view)
            {
                views->add_camera(scene, camera->name, camera);
            }
        });
}

void update_camera_2(pnanovdb_editor_t* editor, pnanovdb_editor_token_t* scene, pnanovdb_camera_t* camera)
{
    if (!editor || !editor->impl || !scene || !camera)
    {
        return;
    }

    // 1) Update the scene's viewport camera view (name == VIEWPORT_CAMERA)
    SceneView* views = editor->impl->scene_view;
    if (views)
    {
        pnanovdb_editor_token_t* viewport_token = views->get_viewport_camera_token();
        pnanovdb_camera_view_t* viewport_view = views->get_camera(scene, viewport_token);
        if (viewport_view && viewport_view->configs && viewport_view->states)
        {
            *viewport_view->configs = camera->config;
            *viewport_view->states = camera->state;
        }
    }

    // 2) If this scene is currently displayed, update the editor's active camera
    bool is_current_scene_displayed = false;
    if (views)
    {
        pnanovdb_editor_token_t* current_scene = views->get_current_scene_token();
        is_current_scene_displayed = (current_scene && current_scene->id == scene->id);
    }

    if (is_current_scene_displayed)
    {
        dispatch_worker_or_immediate(
            editor,
            // Worker mode: queue for render thread (deep copy to take ownership)
            [&](EditorWorker* worker)
            {
                pnanovdb_camera_t* owned = new pnanovdb_camera_t();
                pnanovdb_camera_init(owned);
                owned->config = camera->config;
                owned->state = camera->state;

                pnanovdb_camera_t* prev_pending = worker->pending_camera.set_pending(owned);
                if (prev_pending)
                {
                    // Previous pending camera was also heap-allocated by us
                    delete prev_pending;
                }
            },
            // Non-worker mode: execute immediately (ensure internal ownership)
            [&]()
            {
                if (!editor->impl->camera)
                {
                    editor->impl->camera = new pnanovdb_camera_t();
                    pnanovdb_camera_init(editor->impl->camera);
                }
                editor->impl->camera->config = camera->config;
                editor->impl->camera->state = camera->state;
            });
    }
}

// Helper function that executes the actual removal logic
// Called either immediately (non-worker mode) or deferred (worker mode)
void execute_removal(pnanovdb_editor_t* editor, pnanovdb_editor_token_t* scene, pnanovdb_editor_token_t* name)
{
    if (!editor || !editor->impl || !scene || !name)
    {
        return;
    }

    std::string name_str(name->str);

    // Get the object from scene manager BEFORE removing it, so we can save its info
    // IMPORTANT: Must save data before remove() since the object will be destroyed
    bool obj_found = false;
    SceneObjectType obj_type = SceneObjectType::NanoVDB; // default
    pnanovdb_editor_token_t* obj_name_token = nullptr;
    void* obj_nanovdb_array = nullptr;
    void* obj_gaussian_data = nullptr;
    void* obj_shader_params = nullptr;

    editor->impl->scene_manager->with_object(scene, name,
                                             [&](SceneObject* obj)
                                             {
                                                 if (obj)
                                                 {
                                                     obj_found = true;
                                                     obj_type = obj->type;
                                                     obj_name_token = obj->name_token;
                                                     obj_nanovdb_array = obj->nanovdb_array;
                                                     obj_gaussian_data = obj->gaussian_data;
                                                     obj_shader_params = obj->shader_params;
                                                 }
                                             });

    // Remove from scene manager (data ownership) for this specific scene only
    // After this call, 'obj' pointer is INVALID (object destroyed)
    bool removed_from_manager = editor->impl->scene_manager->remove(scene, name);
    if (removed_from_manager)
    {
        Console::getInstance().addLog("[API] Removed from scene manager (scene-specific)");
    }

    // Update views (this function is always called from render thread, so it's safe)
    SceneView* views = editor->impl->scene_view;
    pnanovdb_editor_token_t* name_token = name ? name : EditorToken::getInstance().getToken(name_str.c_str());

    pnanovdb_editor_token_t* new_view = nullptr;
    if (views->remove_and_fix_current(scene, name_token, &new_view))
    {
        Console::getInstance().addLog("[API] Removed view from UI");
        if (new_view)
        {
            Console::getInstance().addLog("[API] Switched view to '%s'", new_view->str);
        }
        else
        {
            Console::getInstance().addLog("[API] No views remaining in scene");
        }
    }

    // If we removed the object from the specified scene, only clear renderer data if it's not referenced in any scene
    if (obj_found && removed_from_manager && editor->impl->scene_manager)
    {
        // Check remaining objects while holding the mutex (prevents race conditions)
        bool same_name_same_type_exists_elsewhere = false;
        bool any_gaussian_exists = false;

        editor->impl->scene_manager->for_each_object(
            [&](SceneObject* o)
            {
                if (!o)
                    return true; // Continue
                if (o->type == SceneObjectType::GaussianData)
                {
                    any_gaussian_exists = true;
                }
                if (obj_name_token && o->name_token && obj_type == o->type && obj_name_token->id == o->name_token->id)
                {
                    same_name_same_type_exists_elsewhere = true;
                }
                return true; // Continue iteration
            });

        switch (obj_type)
        {
        case SceneObjectType::NanoVDB:
            if (!same_name_same_type_exists_elsewhere)
            {
                if (editor->impl->nanovdb_array == obj_nanovdb_array)
                {
                    editor->impl->nanovdb_array = nullptr;
                    Console::getInstance().addLog("[API] Cleared nanovdb_array from renderer");
                }
                // Clear shader_params if it points to the freed array's data
                if (editor->impl->shader_params == obj_shader_params)
                {
                    editor->impl->shader_params = nullptr;
                    editor->impl->shader_params_data_type = nullptr;
                    Console::getInstance().addLog("[API] Cleared shader_params from renderer");
                }
                if (editor->impl->editor_worker)
                {
                    editor->impl->editor_worker->pending_nanovdb.set_pending(nullptr);
                    Console::getInstance().addLog("[API] Cleared pending nanovdb data");

                    // Clear pending_shader_params if it points to this object's freed array data
                    std::lock_guard<std::recursive_mutex> lock(editor->impl->editor_worker->shader_params_mutex);
                    void* pending_params =
                        editor->impl->editor_worker->pending_shader_params.pending_data.load(std::memory_order_acquire);
                    if (pending_params == obj_shader_params)
                    {
                        editor->impl->editor_worker->pending_shader_params.set_pending(nullptr);
                        editor->impl->editor_worker->pending_shader_params_data_type.set_pending(nullptr);
                        Console::getInstance().addLog("[API] Cleared pending shader_params");
                    }
                }
            }
            break;

        case SceneObjectType::GaussianData:
            if (!same_name_same_type_exists_elsewhere)
            {
                if (editor->impl->gaussian_data == obj_gaussian_data)
                {
                    editor->impl->gaussian_data = nullptr;
                    Console::getInstance().addLog("[API] Cleared gaussian_data from renderer");
                }
                // Clear shader_params if it points to the freed array's data
                if (editor->impl->shader_params == obj_shader_params)
                {
                    editor->impl->shader_params = nullptr;
                    editor->impl->shader_params_data_type = nullptr;
                    Console::getInstance().addLog("[API] Cleared shader_params from renderer");
                }
                if (editor->impl->editor_worker)
                {
                    editor->impl->editor_worker->pending_gaussian_data.set_pending(nullptr);
                    Console::getInstance().addLog("[API] Cleared pending gaussian data");

                    // Clear pending_shader_params if it points to this object's freed array data
                    std::lock_guard<std::recursive_mutex> lock(editor->impl->editor_worker->shader_params_mutex);
                    void* pending_params =
                        editor->impl->editor_worker->pending_shader_params.pending_data.load(std::memory_order_acquire);
                    if (pending_params == obj_shader_params)
                    {
                        editor->impl->editor_worker->pending_shader_params.set_pending(nullptr);
                        editor->impl->editor_worker->pending_shader_params_data_type.set_pending(nullptr);
                        Console::getInstance().addLog("[API] Cleared pending shader_params");
                    }
                }
            }
            break;

        case SceneObjectType::Camera:
            // Nothing to clear globally for cameras
            break;

        default:
            break;
        }
    }

    if (removed_from_manager)
    {
        Console::getInstance().addLog("[API] Removed object '%s' from scene '%s'", name->str, scene->str);
    }
    else
    {
        Console::getInstance().addLog("[API] Warning: Object '%s' not found in scene '%s'", name->str, scene->str);
    }
}

void remove(pnanovdb_editor_t* editor, pnanovdb_editor_token_t* scene, pnanovdb_editor_token_t* name)
{
    if (!editor || !editor->impl || !scene)
    {
        return;
    }

    if (!name)
    {
        // TODO: Remove when supported removing scenes when name is nullptr
        return;
    }

    // If name is nullptr, remove ALL objects from the scene AND the scene itself
    if (!name)
    {
        Console::getInstance().addLog("[API] remove: Removing entire scene='%s' (id=%llu) and all its objects",
                                      token_to_string(scene), (unsigned long long)scene->id);

        // Collect all object names from the specified scene
        std::vector<pnanovdb_editor_token_t*> objects_to_remove;
        editor->impl->scene_manager->for_each_object(
            [&](SceneObject* obj)
            {
                if (obj && obj->scene_token && obj->scene_token->id == scene->id)
                {
                    objects_to_remove.push_back(obj->name_token);
                }
                return true; // Continue iteration
            });

        Console::getInstance().addLog("[API] Found %zu objects to remove from scene", objects_to_remove.size());

        dispatch_worker_or_immediate(
            editor,
            // Worker mode: queue all removals for render thread
            [&](EditorWorker* worker)
            {
                std::lock_guard<std::mutex> lock(worker->pending_removals_mutex);

                // Queue each object removal
                for (auto* obj_name : objects_to_remove)
                {
                    worker->pending_removals.push_back({ scene, obj_name });
                }

                // Queue scene removal (nullptr name signals to remove the scene itself after objects)
                worker->pending_removals.push_back({ scene, nullptr });

                Console::getInstance().addLog(
                    "[API] Queued %zu object removals + scene removal for next frame", objects_to_remove.size());
            },
            // Non-worker mode: execute removals immediately
            [&]()
            {
                for (auto* obj_name : objects_to_remove)
                {
                    execute_removal(editor, scene, obj_name);
                }

                // After removing all objects, remove the scene itself from SceneView
                if (editor->impl->scene_view)
                {
                    bool scene_removed = editor->impl->scene_view->remove_scene(scene);
                    if (scene_removed)
                    {
                        Console::getInstance().addLog("[API] Removed scene '%s' from SceneView", scene->str);
                    }
                    else
                    {
                        Console::getInstance().addLog("[API] Scene '%s' was not found in SceneView", scene->str);
                    }
                }

                Console::getInstance().addLog("[API] Completed removal of scene '%s' and all its objects", scene->str);
            });

        return;
    }

    Console::getInstance().addLog("[API] remove: scene='%s' (id=%llu), name='%s' (id=%llu)", token_to_string(scene),
                                  (unsigned long long)scene->id, token_to_string(name), (unsigned long long)name->id);

    dispatch_worker_or_immediate(
        editor,
        // Worker mode: queue removal for next frame boundary (prevents UAF)
        [&](EditorWorker* worker)
        {
            Console::getInstance().addLog("[API] Queuing removal for next frame");
            std::lock_guard<std::mutex> lock(worker->pending_removals_mutex);
            worker->pending_removals.push_back({ scene, name });
        },
        // Non-worker mode: execute removal immediately (same thread as render)
        [&]() { execute_removal(editor, scene, name); });
}

/*!
    \brief Map shader parameters for read/write access

    Returns a pointer to the shader parameters for the specified scene object.

    IMPORTANT LIFETIME REQUIREMENTS:
    - The returned pointer is valid ONLY between map_params() and unmap_params() calls
    - The object (scene, name) MUST NOT be removed while params are mapped
    - Removing the object while mapped results in USE-AFTER-FREE (undefined behavior)
    - Always call unmap_params() when finished modifying parameters

    In worker mode (editor.start()):
    - Holds shader_params_mutex for the entire map/unmap window
    - This protects against concurrent parameter updates from render thread
    - Does NOT protect against object removal - caller's responsibility!

    Thread Safety Contract:
    - Client must ensure object lifetime spans the map/unmap window
    - Do NOT call remove() for a mapped object
    - Similar to STL iterator invalidation rules

    \param editor Editor instance
    \param scene Scene token
    \param name Object name token
    \param data_type Expected parameter type (for validation)
    \return Pointer to shader params, or nullptr if not found or type mismatch
*/
void* map_params(pnanovdb_editor_t* editor,
                 pnanovdb_editor_token_t* scene,
                 pnanovdb_editor_token_t* name,
                 const pnanovdb_reflect_data_type_t* data_type)
{
    if (!editor || !editor->impl || !data_type || !scene || !name)
    {
        return nullptr;
    }

    if (!editor->impl->editor_worker)
    {
        // Non-worker mode: just return params without locking
        // WARNING: Caller must ensure object lifetime - removing the object while using
        // this pointer results in use-after-free (similar to STL iterator invalidation)
        void* result = nullptr;
        editor->impl->scene_manager->with_object(
            scene, name,
            [&](SceneObject* obj)
            {
                if (obj && obj->shader_params && pnanovdb_reflect_layout_compare(obj->shader_params_data_type, data_type))
                {
                    result = obj->shader_params;
                }
            });
        return result;
    }

    // Worker mode: Lock mutex to protect concurrent access during map/unmap window
    // Note: The lock is held until unmap_params() is called
    // WARNING: This mutex protects against concurrent parameter updates, but does NOT
    // protect against object removal. Caller must ensure the object is not removed
    // between map_params() and unmap_params() calls.
    editor->impl->editor_worker->shader_params_mutex.lock();

    const char* type_name = data_type->struct_typename ? data_type->struct_typename : "<unknown>";
    Console::getInstance().addLog("[API] map_params: scene='%s' (id=%llu), name='%s' (id=%llu), type='%s'",
                                  token_to_string(scene), (unsigned long long)scene->id, token_to_string(name),
                                  (unsigned long long)name->id, type_name);

    // Find params in scene manager
    void* result = nullptr;
    editor->impl->scene_manager->with_object(
        scene, name,
        [&](SceneObject* obj)
        {
            if (obj && obj->shader_params && pnanovdb_reflect_layout_compare(obj->shader_params_data_type, data_type))
            {
                Console::getInstance().addLog("[API] map_params: Found params in scene manager");
                result = obj->shader_params;
            }
        });

    // If not found, unlock and return nullptr
    if (!result)
    {
        Console::getInstance().addLog("[API] map_params: No matching params found");
        editor->impl->editor_worker->shader_params_mutex.unlock();
    }

    return result;
}

/*!
    \brief Unmap shader parameters and signal changes

    MUST be called after map_params() to release locks and signal render thread.

    After calling unmap_params():
    - The pointer from map_params() becomes invalid
    - Render thread will sync changes on next frame
    - Mutex is released (in worker mode)

    \param editor Editor instance
    \param scene Scene token
    \param name Object name token
*/
void unmap_params(pnanovdb_editor_t* editor, pnanovdb_editor_token_t* scene, pnanovdb_editor_token_t* name)
{
    if (!editor || !editor->impl)
    {
        return;
    }

    // Log token information for debugging
    if (scene && name)
    {
        Console::getInstance().addLog("[API] unmap_params: scene='%s' (id=%llu), name='%s' (id=%llu)",
                                      token_to_string(scene), (unsigned long long)scene->id, token_to_string(name),
                                      (unsigned long long)name->id);
    }

    // Unlock mutex (was locked in map_params)
    if (editor->impl->editor_worker)
    {
        editor->impl->editor_worker->shader_params_mutex.unlock();

        // Signal editor thread that params were modified
        // Editor will sync to UI on next frame when it checks params_dirty flag
        editor->impl->editor_worker->params_dirty.store(true);
    }
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
    editor.get_token = get_token;
    editor.add_nanovdb_2 = add_nanovdb_2;
    editor.add_gaussian_data_2 = add_gaussian_data_2;
    editor.add_camera_view_2 = add_camera_view_2;
    editor.update_camera_2 = update_camera_2;
    editor.remove = remove;
    editor.map_params = map_params;
    editor.unmap_params = unmap_params;

    return &editor;
}
}
