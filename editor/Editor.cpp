// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/editor/Editor.cpp

    \author Petra Hapalova

    \brief
*/

#include "Editor.h"

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
#include <unordered_map>
#include <mutex>
#include <cstring>


static const pnanovdb_uint32_t s_default_width = 1440u;
static const pnanovdb_uint32_t s_default_height = 720u;

static const char* s_raster_file = "";

namespace pnanovdb_editor
{
enum class ViewportShader : int
{
    Editor
};

static const char* s_viewport_shaders[] = { "editor/editor.slang", "editor/wireframe.slang" };
static const char* s_default_shader = s_viewport_shaders[(int)ViewportShader::Editor];

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
    editor->impl->raster_ctx = NULL;
    editor->impl->shader_params = NULL;
    editor->impl->shader_params_data_type = NULL;
    editor->impl->views = new EditorView();
    editor->impl->resolved_port = PNANOVDB_EDITOR_RESOLVED_PORT_PENDING;

    // Temp: Initialize resources for _2 API but with a single scene
    editor->impl->device = NULL;
    editor->impl->compute_queue = NULL;
    editor->impl->device_queue = NULL;
    editor->impl->raster = new pnanovdb_raster_t();
    pnanovdb_raster_load(editor->impl->raster, editor->impl->compute);
    pnanovdb_camera_init(&editor->impl->scene_camera);
    editor->impl->camera = &editor->impl->scene_camera;

    return PNANOVDB_TRUE;
}

void init(pnanovdb_editor_t* editor)
{
}

void shutdown(pnanovdb_editor_t* editor)
{
    if (editor->impl->editor_worker)
    {
        editor->stop(editor);
    }
    if (editor->impl->views)
    {
        delete editor->impl->views;
    }
    // Temp: Clean up all gaussian data in the map
    if (editor->impl->raster && editor->impl->device_queue)
    {
        for (auto& entry : editor->impl->gaussian_data_map)
        {
            if (entry.second)
            {
                editor->impl->raster->destroy_gaussian_data(
                    editor->impl->raster->compute, editor->impl->device_queue, entry.second);
            }
        }
    }
    editor->impl->gaussian_data_map.clear();
    // Temp: Clean up all camera views in the map
    for (auto& entry : editor->impl->camera_view_map)
    {
        if (entry.second)
        {
            delete[] entry.second->configs;
            delete[] entry.second->states;
            delete entry.second;
        }
    }
    editor->impl->camera_view_map.clear();
    if (editor->impl->raster)
    {
        pnanovdb_raster_free(editor->impl->raster);
        delete editor->impl->raster;
    }
    if (editor->impl)
    {
        delete editor->impl;
        editor->impl = nullptr;
    }
}

void add_nanovdb(pnanovdb_editor_t* editor, pnanovdb_compute_array_t* nanovdb_array)
{
    if (!nanovdb_array)
    {
        return;
    }

    if (editor->impl->editor_worker)
    {
        EditorWorker* worker = editor->impl->editor_worker;
        worker->pending_nanovdb.set_pending(nanovdb_array);
    }
    else
    {
        editor->impl->nanovdb_array = nanovdb_array;
    }

    EditorView* views = editor->impl->views;
    if (views)
    {
        views->add_nanovdb_view(nanovdb_array, editor->impl->shader_params);
    }
}

void add_array(pnanovdb_editor_t* editor, pnanovdb_compute_array_t* data_array)
{
    if (!data_array)
    {
        return;
    }
    if (editor->impl->editor_worker)
    {
        EditorWorker* worker = editor->impl->editor_worker;
        worker->pending_data_array.set_pending(data_array);
    }
    else
    {
        editor->impl->data_array = data_array;
    }
}
void add_gaussian_data(pnanovdb_editor_t* editor,
                       pnanovdb_raster_context_t* raster_ctx,
                       pnanovdb_compute_queue_t* queue,
                       pnanovdb_raster_gaussian_data_t* gaussian_data)
{
    if (!gaussian_data || !raster_ctx)
    {
        return;
    }

    auto ptr = pnanovdb_raster::cast(gaussian_data);
    if (!ptr->shader_params->data)
    {
        return;
    }

    pnanovdb_raster_shader_params_t* raster_params = (pnanovdb_raster_shader_params_t*)ptr->shader_params->data;

    if (editor->impl->editor_worker)
    {
        EditorWorker* worker = editor->impl->editor_worker;
        worker->pending_gaussian_data.set_pending(gaussian_data);
        worker->pending_raster_ctx.set_pending(raster_ctx);
        worker->pending_shader_params.set_pending(raster_params);
        worker->pending_shader_params_data_type.set_pending(ptr->shader_params_data_type);
    }
    else
    {
        editor->impl->gaussian_data = gaussian_data;
        editor->impl->raster_ctx = raster_ctx;
        editor->impl->shader_params = raster_params;
        editor->impl->shader_params_data_type = ptr->shader_params_data_type;
    }

    EditorView* views = editor->impl->views;
    if (views)
    {
        views->add_gaussian_view(raster_ctx, gaussian_data, raster_params);
    }
}

void update_camera(pnanovdb_editor_t* editor, pnanovdb_camera_t* camera)
{
    if (editor->impl->editor_worker)
    {
        EditorWorker* worker = editor->impl->editor_worker;
        worker->pending_camera.set_pending(camera);
    }
    else
    {
        editor->impl->camera = camera;
    }
}

void add_camera_view(pnanovdb_editor_t* editor, pnanovdb_camera_view_t* camera)
{
    EditorView* views = editor->impl->views;
    if (!views || !camera)
    {
        return;
    }
    // replace existing view if name matches
    if (camera->name)
    {
        const char* name_str = pnanovdb_editor_token_get_string(camera->name);
        if (name_str)
        {
            views->add_camera(name_str, camera);
        }
    }
}

void add_shader_params(pnanovdb_editor_t* editor, void* params, const pnanovdb_reflect_data_type_t* data_type)
{
    if (!params || !data_type)
    {
        return;
    }
    if (editor->impl->editor_worker)
    {
        EditorWorker* worker = editor->impl->editor_worker;
        worker->pending_shader_params.set_pending(params);
        worker->pending_shader_params_data_type.set_pending(data_type);
    }
    else
    {
        editor->impl->shader_params = params;
        editor->impl->shader_params_data_type = data_type;
    }
}

void sync_shader_params(pnanovdb_editor_t* editor, void* shader_params, pnanovdb_bool_t set_data)
{
    if (!editor->impl->editor_worker)
    {
        return;
    }
    if (!shader_params || shader_params != editor->impl->shader_params)
    {
        // only sync current shader params
        return;
    }

    EditorWorker* worker = editor->impl->editor_worker;
    if (set_data)
    {
        worker->set_params.fetch_add(1);
        while (worker->set_params.load() > 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    else
    {
        worker->get_params.fetch_add(1);
        while (worker->get_params.load() > 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
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
    if (worker->set_params.load() > 0 || worker->get_params.load() > 0 || worker->should_stop.load())
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

    // Store device and queue for _2 API methods
    editor->impl->device = device;
    editor->impl->device_queue = device_queue;
    editor->impl->compute_queue = compute_queue;

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

    pnanovdb_raster_t raster = {};
    pnanovdb_raster_load(&raster, editor->impl->compute);

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

    for (const char* shader : s_viewport_shaders)
    {
        imgui_user_instance->viewport_shaders.push_back(shader);
    }

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

    pnanovdb_editor::EditorSceneConfig scene_config{ imgui_user_instance, editor,        imgui_user_settings,
                                                     device_queue,        compiler_inst, s_default_shader };
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
            editor->impl->raster_ctx = nullptr;
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
                log_print(PNANOVDB_COMPUTE_LOG_LEVEL_INFO, "Stopped\n");
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

        // update scene
        editor_scene.sync_selected_view_with_current();
        editor_scene.sync_shader_params_from_editor();

        // update raster
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
                    imgui_user_instance->shader_params.get_compute_array_for_shader(
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
                    &raster, raster.compute, worker_queue, pending_raster_filepath.c_str(), pending_voxel_size,
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
                    editor_scene.handle_nanovdb_data_load(pending_nanovdb_array, pending_raster_filepath);
                    editor_scene.update_viewport_shader(s_default_shader);
                }
                else if (imgui_user_instance->viewport_option == imgui_instance_user::ViewportOption::Raster2D)
                {
                    editor_scene.handle_gaussian_data_load(pending_raster_ctx, pending_gaussian_data,
                                                           pending_raster_params, pending_raster_filepath, &raster,
                                                           old_gaussian_data_ptr);
                }
                pnanovdb_editor::Console::getInstance().addLog(
                    "Rasterization of '%s' was successful", pending_raster_filepath.c_str());
            }
            else
            {
                pnanovdb_editor::Console::getInstance().addLog(
                    "Rasterization of '%s' failed", pending_raster_filepath.c_str());
            }

            pending_raster_filepath = "";
            raster_worker.removeCompletedTask(raster_task_id);

            imgui_user_instance->progress.reset();
        }

        // Rendering
        if (editor->impl->nanovdb_array)
        {
            if (imgui_user_instance->pending.update_shader)
            {
                std::lock_guard<std::mutex> lock(imgui_user_instance->compiler_settings_mutex);

                imgui_user_instance->pending.update_shader = false;
                editor->impl->compute->destroy_shader_context(editor->impl->compute, device_queue, shader_context);
                shader_context = editor->impl->compute->create_shader_context(imgui_user_instance->shader_name.c_str());
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

            raster.raster_gaussian_2d(raster.compute, device_queue, editor->impl->raster_ctx, editor->impl->gaussian_data,
                                      background_image, image_width, image_height, &view, &projection, &raster_params);
        }
        else
        {
            cleanup_background();
        }

        if (editor->impl->camera && imgui_user_settings->sync_camera == PNANOVDB_FALSE)
        {
            imgui_window_iface->get_camera(imgui_window, &editor->impl->camera->state, &editor->impl->camera->config);
        }

        imgui_window_iface->update_camera(imgui_window, imgui_user_settings);

        // update after switching views
        editor_scene.sync_default_camera_view();

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

        // Process pending removals - cleanup resources that were marked for removal
        std::vector<std::string> removals_to_process;
        {
            std::lock_guard<std::mutex> lock(editor->impl->pending_removals_mutex);
            removals_to_process.swap(editor->impl->pending_removals);
        }

        for (const auto& name_str : removals_to_process)
        {
            // Remove gaussian data from map if it exists
            auto gaussian_it = editor->impl->gaussian_data_map.find(name_str);
            if (gaussian_it != editor->impl->gaussian_data_map.end())
            {
                pnanovdb_raster_gaussian_data_t* gaussian_data = gaussian_it->second;

                // If this is the current gaussian data, clear it
                if (editor->impl->gaussian_data == gaussian_data)
                {
                    editor->impl->gaussian_data = nullptr;
                }

                // Destroy the gaussian data
                if (editor->impl->raster && editor->impl->device_queue && gaussian_data)
                {
                    editor->impl->raster->destroy_gaussian_data(
                        editor->impl->raster->compute, editor->impl->device_queue, gaussian_data);
                }

                // Remove from map
                editor->impl->gaussian_data_map.erase(gaussian_it);
            }

            // Remove camera view from map if it exists
            auto camera_it = editor->impl->camera_view_map.find(name_str);
            if (camera_it != editor->impl->camera_view_map.end())
            {
                pnanovdb_camera_view_t* camera_view = camera_it->second;

                // Clean up the camera view
                if (camera_view)
                {
                    delete[] camera_view->configs;
                    delete[] camera_view->states;
                }

                // Remove from map
                editor->impl->camera_view_map.erase(camera_it);
            }
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

    raster.destroy_context(editor->impl->compute, device_queue, editor->impl->raster_ctx);
    editor->impl->device_queue = nullptr;
    editor->impl->compute_queue = nullptr;
    editor->impl->device = nullptr;
    editor->impl->raster = nullptr;
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

pnanovdb_camera_t* get_camera(pnanovdb_editor_t* editor, pnanovdb_editor_token_t* scene)
{
    if (!editor || !scene)
    {
        return nullptr;
    }

    const char* name_str = pnanovdb_editor_token_get_string(scene);
    if (name_str)
    {
        editor->impl->views->set_current_scene(name_str);
    }

    return editor->impl->camera;
}

// Temp: Token management
static std::unordered_map<std::string, pnanovdb_editor_token_t*> s_token_registry;
static std::mutex s_token_mutex;

pnanovdb_editor_token_t* get_token(const char* name)
{
    if (!name)
    {
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(s_token_mutex);

    std::string name_str(name);
    auto it = s_token_registry.find(name_str);
    if (it != s_token_registry.end())
    {
        return it->second;
    }

    // Create new token
    pnanovdb_editor_token_t* token = new pnanovdb_editor_token_t();
    token->id = std::hash<std::string>{}(name_str);
    token->str = strdup(name);

    s_token_registry[name_str] = token;
    return token;
}

void add_nanovdb_2(pnanovdb_editor_t* editor,
                   pnanovdb_editor_token_t* scene,
                   pnanovdb_editor_token_t* name,
                   pnanovdb_compute_array_t* array)
{
    if (!editor || !editor->impl || !array)
    {
        return;
    }

    if (name && name->str)
    {
        array->filepath = name->str;
    }

    add_nanovdb(editor, array);
}

void add_gaussian_data_2(pnanovdb_editor_t* editor,
                         pnanovdb_editor_token_t* scene,
                         pnanovdb_editor_token_t* name,
                         const pnanovdb_editor_gaussian_data_desc_t* desc)
{
    if (!editor || !editor->impl || !desc)
    {
        return;
    }

    // Check if we have the required resources
    if (!editor->impl->raster || !editor->impl->device_queue || !editor->impl->compute_queue)
    {
        // Resources not yet available (show/start not called yet)
        return;
    }

    // Create or get raster context
    pnanovdb_raster_context_t* raster_ctx = editor->impl->raster_ctx;
    if (!raster_ctx)
    {
        // Create a new raster context
        raster_ctx = editor->impl->raster->create_context(editor->impl->compute, editor->impl->compute_queue);
        if (!raster_ctx)
        {
            return;
        }
        editor->impl->raster_ctx = raster_ctx;
    }

    // Create default shader params
    const pnanovdb_reflect_data_type_t* data_type = PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_raster_shader_params_t);
    const pnanovdb_raster_shader_params_t* defaults = (const pnanovdb_raster_shader_params_t*)data_type->default_value;
    pnanovdb_raster_shader_params_t raster_params = *defaults;
    raster_params.data_type = data_type;
    if (name && name->str)
    {
        raster_params.name = name->str;
    }

    pnanovdb_compute_array_t* arrays[6] = {};
    arrays[0] = desc->means;
    arrays[1] = desc->opacities;
    arrays[2] = desc->quaternions;
    arrays[3] = desc->scales;
    arrays[4] = desc->sh_0;
    arrays[5] = desc->sh_n;

    // Create gaussian data
    pnanovdb_raster_gaussian_data_t* gaussian_data = nullptr;
    editor->impl->raster->create_gaussian_data_from_arrays(editor->impl->raster, editor->impl->compute,
                                                           editor->impl->device_queue, arrays, 6u, &gaussian_data,
                                                           &raster_params, &raster_ctx);

    if (gaussian_data)
    {
        // Store in map indexed by name, take ownership of the gaussian data
        editor->impl->gaussian_data_map[name->str] = gaussian_data;

        add_gaussian_data(editor, raster_ctx, editor->impl->device_queue, gaussian_data);
    }
}

void add_camera_view_2(pnanovdb_editor_t* editor, pnanovdb_editor_token_t* scene, pnanovdb_camera_view_t* camera)
{
    if (!editor || !editor->impl || !camera)
    {
        return;
    }

    const char* name_str = pnanovdb_editor_token_get_string(camera->name);
    if (!name_str)
    {
        return;
    }

    // Store pointer in map, take ownership of the camera view
    editor->impl->camera_view_map[name_str] = camera;

    add_camera_view(editor, camera);
}

void update_camera_2(pnanovdb_editor_t* editor, pnanovdb_editor_token_t* scene, pnanovdb_camera_t* camera)
{
    if (!editor || !editor->impl || !camera)
    {
        return;
    }


    editor->impl->scene_camera = *camera;

    update_camera(editor, &editor->impl->scene_camera);
}

void remove(pnanovdb_editor_t* editor, pnanovdb_editor_token_t* scene, pnanovdb_editor_token_t* name)
{
    if (!editor || !editor->impl || !editor->impl->views)
    {
        return;
    }

    const char* name_str = pnanovdb_editor_token_get_string(name);
    if (!name_str)
    {
        return;
    }

    // Remove from views
    editor->impl->views->remove_view(std::string(name_str));

    // Add to pending removals list for cleanup at end of frame
    {
        std::lock_guard<std::mutex> lock(editor->impl->pending_removals_mutex);
        editor->impl->pending_removals.push_back(std::string(name_str));
    }
}

void* map_params(pnanovdb_editor_t* editor,
                 pnanovdb_editor_token_t* scene,
                 pnanovdb_editor_token_t* name,
                 const pnanovdb_reflect_data_type_t* data_type)
{
    // Map params would allow reading/writing parameters
    if (!editor || !editor->impl)
    {
        return nullptr;
    }

    // In a full implementation, this would look up the object by scene/name
    // and return a pointer to its parameters
    return editor->impl->shader_params;
}

void unmap_params(pnanovdb_editor_t* editor, pnanovdb_editor_token_t* scene, pnanovdb_editor_token_t* name)
{
    // Unmap params would flush any changes
    if (!editor || !editor->impl)
    {
        return;
    }

    // In a full implementation, this would sync changes back to the server
    sync_shader_params(editor, editor->impl->shader_params, PNANOVDB_TRUE);
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
