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

#include "imgui/ImguiWindow.h"
#include "imgui/UploadBuffer.h"

#include "compute/ComputeShader.h"
#include "raster/Raster.h"

#include "nanovdb_editor/putil/WorkerThread.hpp"

#include <nanovdb/io/IO.h>

#include <thread>
#include <atomic>

#define TEST_IO 0

#define USE_IMGUI_INSTANCE

#ifdef USE_IMGUI_INSTANCE
#    include "ImguiInstance.h"
#endif

static const pnanovdb_uint32_t s_default_width = 1440u;
static const pnanovdb_uint32_t s_default_height = 720u;

static const char* s_raster_file = "";

namespace pnanovdb_editor
{
template <typename T>
struct PendingData
{
    std::atomic<T*> pending_data{ nullptr };

    T* set_pending(T* data)
    {
        return pending_data.exchange(data, std::memory_order_acq_rel);
    }

    // Returns true if there was pending data, and updates current_data/old_data
    bool process_pending(T*& current_data, T*& old_data)
    {
        T* data = pending_data.exchange(nullptr, std::memory_order_acq_rel);
        if (data)
        {
            old_data = current_data;
            current_data = data;
            return true;
        }
        return false;
    }
};

template <typename T>
struct ConstPendingData
{
    std::atomic<const T*> pending_data{ nullptr };

    // Returns previous pointer (if any) so caller can release it if needed
    const T* set_pending(const T* data)
    {
        return pending_data.exchange(data, std::memory_order_acq_rel);
    }

    // Returns true if there was pending data, and updates current_data/old_data
    bool process_pending(const T*& current_data, const T*& old_data)
    {
        const T* data = pending_data.exchange(nullptr, std::memory_order_acq_rel);
        if (data)
        {
            old_data = current_data;
            current_data = data;
            return true;
        }
        return false;
    }
};

struct EditorWorker
{
    std::thread* thread;
    std::atomic<bool> should_stop{ false };
    std::atomic<int> set_params{ 0 };
    std::atomic<int> get_params{ 0 };
    PendingData<pnanovdb_compute_array_t> pending_nanovdb;
    PendingData<pnanovdb_compute_array_t> pending_data_array;
    PendingData<pnanovdb_raster_context_t> pending_raster_ctx;
    PendingData<pnanovdb_raster_gaussian_data_t> pending_gaussian_data;
    PendingData<pnanovdb_camera_t> pending_camera;
    PendingData<void> pending_shader_params;
    ConstPendingData<pnanovdb_reflect_data_type_t> pending_shader_params_data_type;
};

// views representing a loaded scene via editor's API, does not own the data
struct EditorView
{
    std::map<std::string, pnanovdb_camera_view_t*> cameras;
    std::string current_view_scene;
    std::map<std::string, imgui_instance_user::GaussianDataContext> gaussians;
    // TODO add points
};

enum class ViewportShader : int
{
    Editor
};

static const char* s_viewport_shaders[] = { "editor/editor.slang", "editor/wireframe.slang" };

// default shader used for the NanoVDB viewer
static const char* s_default_shader = s_viewport_shaders[(int)ViewportShader::Editor];
static const char* s_raster_shader = s_viewport_shaders[(int)ViewportShader::Editor];

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

// user defines a group of shader parameters which can be controlled in the editor
struct ShaderParams
{
#if NANOVDB_EDITOR_SHADER_PARAMS_SIZE
    uint64_t pad[NANOVDB_EDITOR_SHADER_PARAMS_SIZE];
#else
    uint64_t pad[16u];
#endif
};

static void save_image(const char* filename, float* mapped_data, uint32_t image_width, uint32_t image_height)
{
    FILE* file = fopen(filename, "wb");
    if (!file)
    {
        printf("Could not create file to save the capture '%s'", filename);
        return;
    }

    char headerField0 = 'B';
    char headerField1 = 'M';
    uint32_t size = 54 + image_width * image_height * 4u;
    uint16_t reserved1 = 0;
    uint16_t reserved2 = 0;
    uint32_t offset = 54;
    uint32_t headerSize = 40;
    uint32_t width = image_width;
    uint32_t height = image_height;
    uint16_t colorPlanes = 1;
    uint16_t bitsPerPixel = 32;
    uint32_t compressionMethod = 0;
    uint32_t imageSize = image_width * image_height * 4u;
    uint32_t hRes = 2000;
    uint32_t vRes = 2000;
    uint32_t numColors = 0;
    uint32_t numImportantColors = 0;

    fwrite(&headerField0, 1, 1, file);
    fwrite(&headerField1, 1, 1, file);
    fwrite(&size, 4, 1, file);
    fwrite(&reserved1, 2, 1, file);
    fwrite(&reserved2, 2, 1, file);
    fwrite(&offset, 4, 1, file);
    fwrite(&headerSize, 4, 1, file);
    fwrite(&width, 4, 1, file);
    fwrite(&height, 4, 1, file);
    fwrite(&colorPlanes, 2, 1, file);
    fwrite(&bitsPerPixel, 2, 1, file);
    fwrite(&compressionMethod, 4, 1, file);
    fwrite(&imageSize, 4, 1, file);
    fwrite(&hRes, 4, 1, file);
    fwrite(&vRes, 4, 1, file);
    fwrite(&numColors, 4, 1, file);
    fwrite(&numImportantColors, 4, 1, file);
    fwrite(mapped_data, 1u, image_width * image_height * 4u, file);

    fclose(file);
}

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
    editor->impl->views = NULL;

    return PNANOVDB_TRUE;
}

void init(pnanovdb_editor_t* editor)
{
    editor->impl->views = new EditorView();
}

void shutdown(pnanovdb_editor_t* editor)
{
    if (editor->impl->views)
    {
        delete static_cast<EditorView*>(editor->impl->views);
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
        EditorWorker* worker = static_cast<EditorWorker*>(editor->impl->editor_worker);
        worker->pending_nanovdb.set_pending(nanovdb_array);
    }
    else
    {
        editor->impl->nanovdb_array = nanovdb_array;
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
        EditorWorker* worker = static_cast<EditorWorker*>(editor->impl->editor_worker);
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
        // TODO: set defaults
        return;
    }
    pnanovdb_raster_shader_params_t* raster_params = (pnanovdb_raster_shader_params_t*)ptr->shader_params->data;
    if (editor->impl->editor_worker)
    {
        EditorWorker* worker = static_cast<EditorWorker*>(editor->impl->editor_worker);
        worker->pending_gaussian_data.set_pending(gaussian_data);
        worker->pending_raster_ctx.set_pending(raster_ctx);
        worker->pending_shader_params.set_pending(ptr->shader_params);
        worker->pending_shader_params_data_type.set_pending(ptr->shader_params_data_type);
    }
    else
    {
        editor->impl->gaussian_data = gaussian_data;
        editor->impl->raster_ctx = raster_ctx;
        editor->impl->shader_params = raster_params;
        editor->impl->shader_params_data_type = ptr->shader_params_data_type;
    }
    EditorView* views = static_cast<EditorView*>(editor->impl->views);
    if (views && raster_params->name)
    {
        views->current_view_scene = raster_params->name;
        auto it = views->gaussians.find(raster_params->name);
        if (it != views->gaussians.end())
        {
            // replace existing view if name matches
            views->gaussians.erase(it);
        }
        views->gaussians[raster_params->name] = { raster_ctx, gaussian_data, raster_params, nullptr };
    }
}

void update_camera(pnanovdb_editor_t* editor, pnanovdb_camera_t* camera)
{
    if (editor->impl->editor_worker)
    {
        EditorWorker* worker = static_cast<EditorWorker*>(editor->impl->editor_worker);
        worker->pending_camera.set_pending(camera);
    }
    else
    {
        editor->impl->camera = camera;
    }
}

void add_camera_view(pnanovdb_editor_t* editor, pnanovdb_camera_view_t* camera)
{
    EditorView* views = static_cast<EditorView*>(editor->impl->views);
    if (!views || !camera)
    {
        return;
    }
    // replace existing view if name matches
    if (camera->name)
    {
        views->cameras[camera->name] = camera;
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
        EditorWorker* worker = static_cast<EditorWorker*>(editor->impl->editor_worker);
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
    if (shader_params != editor->impl->shader_params)
    {
        // only sync current shader params
        return;
    }

    EditorWorker* worker = static_cast<EditorWorker*>(editor->impl->editor_worker);
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

    auto worker = static_cast<EditorWorker*>(editor->impl->editor_worker);
    pnanovdb_int32_t count = 0;
    if (worker->set_params.load() > 0 || worker->get_params.load() > 0 ||
        worker->should_stop.load())
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

    pnanovdb_imgui_window_interface_t* imgui_window_iface = pnanovdb_imgui_get_window_interface();
    if (!imgui_window_iface)
    {
        return;
    }
    pnanovdb_imgui_settings_render_t* imgui_user_settings = nullptr;

#ifdef USE_IMGUI_INSTANCE
    pnanovdb_imgui_instance_interface_t* imgui_instance_iface = get_user_imgui_instance_interface();
    imgui_instance_user::Instance* imgui_user_instance = nullptr;
    void* imgui_instance_userdata = &imgui_user_instance;
#endif
    pnanovdb_imgui_window_t* imgui_window = imgui_window_iface->create(
        editor->impl->compute, device, image_width, image_height, (void**)&imgui_user_settings,
#ifdef USE_IMGUI_INSTANCE
        PNANOVDB_FALSE, &imgui_instance_iface, &imgui_instance_userdata, 1u,
#else
        PNANOVDB_TRUE, nullptr, nullptr, 0u,
#endif
        config->headless);

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

    // Automatically enable encoder when streaming is requested
    if (config->streaming || config->stream_to_file)
    {
        imgui_user_settings->enable_encoder = PNANOVDB_TRUE;
        if (config->ip_address != nullptr)
        {
            snprintf(imgui_user_settings->server_address, sizeof(imgui_user_settings->server_address), "%s",
                     config->ip_address);
        }
        imgui_user_settings->server_port = config->port;
        if (config->stream_to_file)
        {
            imgui_user_settings->encode_to_file = PNANOVDB_TRUE;
        }
    }

#ifdef USE_IMGUI_INSTANCE
    if (!imgui_user_instance)
    {
        return;
    }
    pnanovdb_compiler_settings_init(&imgui_user_instance->compiler_settings);
#else
    pnanovdb_compiler_settings_t compile_settings = {};
    pnanovdb_compiler_settings_init(&compile_settings);
#endif
    pnanovdb_compiler_instance_t* compiler_inst = editor->impl->compiler->create_instance();
    pnanovdb_compute_queue_t* device_queue = editor->impl->compute->device_interface.get_device_queue(device);
    // used for a worker thread
    pnanovdb_compute_queue_t* compute_queue = editor->impl->compute->device_interface.get_compute_queue(device);
    pnanovdb_compute_interface_t* compute_interface =
        editor->impl->compute->device_interface.get_compute_interface(device_queue);
    pnanovdb_compute_context_t* compute_context =
        editor->impl->compute->device_interface.get_compute_context(device_queue);

    pnanovdb_camera_mat_t view = {};
    pnanovdb_camera_mat_t projection = {};

    pnanovdb_compute_upload_buffer_t compute_upload_buffer;
    pnanovdb_compute_upload_buffer_init(compute_interface, compute_context, &compute_upload_buffer,
                                        PNANOVDB_COMPUTE_BUFFER_USAGE_CONSTANT, PNANOVDB_COMPUTE_FORMAT_UNKNOWN, 0u);

    pnanovdb_compute_upload_buffer_t shader_params_upload_buffer;
    pnanovdb_compute_upload_buffer_init(compute_interface, compute_context, &shader_params_upload_buffer,
                                        PNANOVDB_COMPUTE_BUFFER_USAGE_CONSTANT, PNANOVDB_COMPUTE_FORMAT_UNKNOWN, 0u);

    pnanovdb_compute_texture_t* background_image = nullptr;
    pnanovdb_compute_buffer_t* readback_buffer = nullptr;

    std::string capture_filename = "./data/pnanovdbeditor_capture.bmp";
#ifdef USE_IMGUI_INSTANCE
    pnanovdb_shader_context_t* shader_context = nullptr;
    pnanovdb_compute_buffer_t* nanovdb_buffer = nullptr;
    pnanovdb_compute_array_t* viewport_shader_params_array = nullptr;
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

    // raster 2d shader params, init array passed to raster2d with default values
    const char* raster2d_shader_name = "raster/gaussian_rasterize_2d.slang";
    const pnanovdb_reflect_data_type_t* raster_shader_params_data_type =
        PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_raster_shader_params_t);
    const pnanovdb_raster_shader_params_t* default_raster_shader_params =
        (const pnanovdb_raster_shader_params_t*)raster_shader_params_data_type->default_value;
    pnanovdb_raster_shader_params_t init_raster_shader_params = *default_raster_shader_params;
    init_raster_shader_params.near_plane_override = imgui_user_settings->camera_config.near_plane;
    init_raster_shader_params.far_plane_override = imgui_user_settings->camera_config.far_plane;

    // init raster params with default values and load UI group
    pnanovdb_compute_array_t* raster2d_shader_params_array = editor->impl->compute->create_array(
        raster_shader_params_data_type->element_size, 1u, &init_raster_shader_params);
    imgui_user_instance->shader_params.loadGroup(imgui_instance_user::s_raster2d_shader_group, true);
    imgui_user_instance->shader_params.set_compute_array_for_shader(raster2d_shader_name, raster2d_shader_params_array);

    editor->impl->compute->device_interface.enable_profiler(
        compute_context, (void*)"editor", pnanovdb_editor::Profiler::report_callback);
    editor->impl->compute->device_interface.get_memory_stats(device, Profiler::getInstance().getMemoryStats());

    // views UI
    imgui_user_instance->camera_views = &(static_cast<EditorView*>(editor->impl->views)->cameras);
    imgui_user_instance->gaussian_views = &(static_cast<EditorView*>(editor->impl->views)->gaussians);

    ShaderCallback callback =
        pnanovdb_editor::get_shader_recompile_callback(imgui_user_instance, editor->impl->compiler, compiler_inst);
    monitor_shader_dir(pnanovdb_shader::getShaderDir().c_str(), callback);

    imgui_user_instance->set_default_shader(s_default_shader);

    if (editor->impl->nanovdb_array && editor->impl->nanovdb_array->filepath)
    {
        imgui_user_instance->nanovdb_filepath = editor->impl->nanovdb_array->filepath;
    }

    if (editor->impl->nanovdb_array)
    {
        imgui_user_instance->nanovdb_array = std::shared_ptr<pnanovdb_compute_array_t>(editor->impl->nanovdb_array,
                                                                                       [](pnanovdb_compute_array_t*)
                                                                                       {
                                                                                           // No-op deleter - editor
                                                                                           // manages memory lifecycle
                                                                                       });
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
#endif

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
        }
        else
        {
            imgui_user_instance->viewport_option = imgui_instance_user::ViewportOption::NanoVDB;
        }
    }

    bool should_run = true;
    while (should_run)
    {
        if (editor->impl->editor_worker)
        {
            bool updated = false;
            auto* worker = static_cast<EditorWorker*>(editor->impl->editor_worker);

            pnanovdb_compute_array_t* old_nanovdb_array = nullptr;
            updated = worker->pending_nanovdb.process_pending(editor->impl->nanovdb_array, old_nanovdb_array);
            if (updated)
            {
                imgui_user_instance->viewport_option = imgui_instance_user::ViewportOption::NanoVDB;
            }
            // TODO remove when handled in loaded data
            if (old_nanovdb_array)
            {
                editor->impl->compute->destroy_array(old_nanovdb_array);
                old_nanovdb_array = nullptr;
            }
            pnanovdb_compute_array_t* old_array = nullptr;
            worker->pending_data_array.process_pending(editor->impl->data_array, old_array);
            if (old_array)
            {
                editor->impl->compute->destroy_array(old_array);
                old_array = nullptr;
            }
            pnanovdb_raster_context_t* old_raster_ctx = nullptr;
            worker->pending_raster_ctx.process_pending(editor->impl->raster_ctx, old_raster_ctx);
            pnanovdb_raster_gaussian_data_t* old_gaussian_data = nullptr;
            updated = worker->pending_gaussian_data.process_pending(editor->impl->gaussian_data, old_gaussian_data);
            if (updated)
            {
                imgui_user_instance->viewport_option = imgui_instance_user::ViewportOption::Raster2D;
            }
            pnanovdb_camera_t* old_camera = nullptr;
            updated = worker->pending_camera.process_pending(editor->impl->camera, old_camera);
            if (updated)
            {
                imgui_user_settings->camera_state = editor->impl->camera->state;
                imgui_user_settings->camera_config = editor->impl->camera->config;
                imgui_user_settings->sync_camera = PNANOVDB_TRUE;
            }
            void* old_shader_params = nullptr;
            worker->pending_shader_params.process_pending(editor->impl->shader_params, old_shader_params);
            const pnanovdb_reflect_data_type_t* old_shader_params_data_type = nullptr;
            updated = worker->pending_shader_params_data_type.process_pending(
                editor->impl->shader_params_data_type, old_shader_params_data_type);
            if (updated)
            {
                if (editor->impl->shader_params &&
                    pnanovdb_reflect_layout_compare(editor->impl->shader_params_data_type,
                                                    PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_raster_shader_params_t)) ==
                        PNANOVDB_TRUE)
                {
                    // init raster shader param's camera from imgui camera
                    pnanovdb_raster_shader_params_t* raster_params =
                        (pnanovdb_raster_shader_params_t*)editor->impl->shader_params;
                    if (raster_params->near_plane_override == 0.f)
                    {
                        raster_params->near_plane_override = imgui_user_settings->camera_config.near_plane;
                    }
                    if (raster_params->far_plane_override == 0.f)
                    {
                        raster_params->far_plane_override = imgui_user_settings->camera_config.far_plane;
                    }
                }
            }
        }

        if (editor->impl->editor_worker && static_cast<EditorWorker*>(editor->impl->editor_worker)->should_stop.load())
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

        // create background image texture
        pnanovdb_compute_texture_desc_t tex_desc = {};
        tex_desc.texture_type = PNANOVDB_COMPUTE_TEXTURE_TYPE_2D;
        tex_desc.usage = PNANOVDB_COMPUTE_TEXTURE_USAGE_TEXTURE | PNANOVDB_COMPUTE_TEXTURE_USAGE_RW_TEXTURE;
        tex_desc.format = PNANOVDB_COMPUTE_FORMAT_R8G8B8A8_UNORM;
        tex_desc.width = image_width;
        tex_desc.height = image_height;
        tex_desc.depth = 1u;
        tex_desc.mip_levels = 1u;
        background_image = compute_interface->create_texture(compute_context, &tex_desc);

        imgui_window_iface->get_camera_view_proj(imgui_window, &image_width, &image_height, &view, &projection);
        pnanovdb_camera_mat_t view_inv = pnanovdb_camera_mat_inverse(view);
        pnanovdb_camera_mat_t projection_inv = pnanovdb_camera_mat_inverse(projection);

        bool should_capture = false;
#ifdef USE_IMGUI_INSTANCE
        // update pending GUI states
        if (imgui_user_instance->pending.capture_image)
        {
            imgui_user_instance->pending.capture_image = false;
            should_capture = true;
        }
        if (imgui_user_instance->pending.load_nvdb)
        {
            imgui_user_instance->pending.load_nvdb = false;
            const char* nvdb_filepath = imgui_user_instance->nanovdb_filepath.c_str();
            pnanovdb_compute_array_t* loaded_array = editor->impl->compute->load_nanovdb(nvdb_filepath);
            if (loaded_array)
            {
                editor->add_nanovdb(editor, loaded_array);
                imgui_user_instance->nanovdb_array =
                    std::shared_ptr<pnanovdb_compute_array_t>(loaded_array,
                                                              [](pnanovdb_compute_array_t*)
                                                              {
                                                                  // No-op deleter - editor manages memory lifecycle
                                                              });
            }
            auto nvdb_shader = s_default_shader;
            if (imgui_user_instance->shader_name != nvdb_shader)
            {
                imgui_user_instance->shader_name = nvdb_shader;
                imgui_user_instance->pending.viewport_shader_name = nvdb_shader;
                imgui_user_instance->pending.update_shader = true;
            }
        }
        if (imgui_user_instance->pending.save_nanovdb)
        {
            imgui_user_instance->pending.save_nanovdb = false;
            const char* nvdb_filepath = imgui_user_instance->nanovdb_filepath.c_str();
            if (editor->impl->nanovdb_array)
            {
                pnanovdb_bool_t result = editor->impl->compute->save_nanovdb(editor->impl->nanovdb_array, nvdb_filepath);
                if (result == PNANOVDB_TRUE)
                {
                    pnanovdb_editor::Console::getInstance().addLog("NanoVDB saved to '%s'", nvdb_filepath);
                }
                else
                {
                    pnanovdb_editor::Console::getInstance().addLog("Failed to save NanoVDB to '%s'", nvdb_filepath);
                }
            }
        }
        if (imgui_user_instance->pending.print_slice)
        {
            imgui_user_instance->pending.print_slice = false;
#    if TEST_IO
            FILE* input_file = fopen("./data/smoke.nvdb", "rb");
            FILE* output_file = fopen("./data/slice_output.bmp", "wb");
            test_pnanovdb_io_print_slice(input_file, output_file);
            fclose(input_file);
            fclose(output_file);
#    endif
        }

        // update memory stats periodically
        if (imgui_user_instance && imgui_user_instance->pending.update_memory_stats)
        {
            editor->impl->compute->device_interface.get_memory_stats(device, Profiler::getInstance().getMemoryStats());
            imgui_user_instance->pending.update_memory_stats = false;
        }

        if (imgui_user_instance->viewport_option == imgui_instance_user::ViewportOption::Raster2D)
        {
            auto sync_camera_from_params_to_ui = [&](pnanovdb_raster_shader_params_t* raster_params)
            {
                if (raster_params->near_plane_override != imgui_user_settings->camera_config.near_plane)
                {
                    imgui_user_settings->camera_config.near_plane = raster_params->near_plane_override;
                    imgui_user_settings->sync_camera = PNANOVDB_TRUE;
                }
                if (raster_params->far_plane_override != imgui_user_settings->camera_config.far_plane)
                {
                    imgui_user_settings->camera_config.far_plane = raster_params->far_plane_override;
                    imgui_user_settings->sync_camera = PNANOVDB_TRUE;
                }
            };

            auto init_camera_overrides = [&](pnanovdb_raster_shader_params_t* raster_params)
            {
                if (raster_params->near_plane_override == 0.f)
                {
                    raster_params->near_plane_override = imgui_user_settings->camera_config.near_plane;
                }
                if (raster_params->far_plane_override == 0.f)
                {
                    raster_params->far_plane_override = imgui_user_settings->camera_config.far_plane;
                }
            };

            auto copy_editor_params_to_ui = [&]()
            {
                raster2d_shader_params_array = editor->impl->compute->create_array(
                    raster_shader_params_data_type->element_size, 1u, editor->impl->shader_params);
                imgui_user_instance->shader_params.set_compute_array_for_shader(
                    raster2d_shader_name, raster2d_shader_params_array);
            };

            auto get_ui_params = [&]()
            {
                editor->impl->compute->destroy_array(raster2d_shader_params_array);
                return imgui_user_instance->shader_params.get_compute_array_for_shader<pnanovdb_raster_shader_params_t>(
                    raster2d_shader_name, editor->impl->compute);
            };

            // sync raster between editor and selected editor's view in Scene tab
            EditorView* views = static_cast<EditorView*>(editor->impl->views);

            auto save_current_view_state = [&](const std::string& view_name)
            {
                auto current_it = views->gaussians.find(view_name);
                if (current_it == views->gaussians.end())
                {
                    return;
                }

                // Save render settings from UI
                imgui_user_instance->views_render_settings[view_name] = *imgui_user_settings;

                pnanovdb_camera_state_t camera_state = {};
                imgui_window_iface->get_camera(imgui_window, &camera_state, nullptr);
                imgui_user_instance->views_render_settings[view_name].camera_state = camera_state;

                // Save current shader params from UI back to the view's stored params
                if (imgui_user_instance->viewport_option == imgui_instance_user::ViewportOption::Raster2D &&
                    current_it->second.shader_params)
                {
                    auto current_shader_params = get_ui_params();

                    if (current_shader_params && current_shader_params->data)
                    {
                        size_t data_size = current_shader_params->element_count * current_shader_params->element_size;
                        std::memcpy(current_it->second.shader_params, current_shader_params->data, data_size);
                        editor->impl->compute->destroy_array(current_shader_params);
                    }
                }
            };

            auto load_view_into_editor_and_ui = [&](const std::string& view_name)
            {
                auto new_it = views->gaussians.find(view_name);
                if (new_it == views->gaussians.end())
                {
                    return;
                }

                editor->impl->gaussian_data = new_it->second.gaussian_data;
                editor->impl->raster_ctx = new_it->second.raster_ctx;
                editor->impl->shader_params = new_it->second.shader_params;
                editor->impl->shader_params_data_type = raster_shader_params_data_type;

                if (editor->impl->editor_worker && editor->impl->shader_params)
                {
                    pnanovdb_raster_shader_params_t* raster_params =
                        (pnanovdb_raster_shader_params_t*)editor->impl->shader_params;
                    init_camera_overrides(raster_params);
                    copy_editor_params_to_ui();
                    sync_camera_from_params_to_ui(raster_params);
                }

                // Load render settings into UI
                if (new_it->second.render_settings == nullptr)
                {
                    // Create new settings from current if none exists for this view yet
                    imgui_user_instance->views_render_settings[view_name] = *imgui_user_settings;
                    new_it->second.render_settings = &imgui_user_instance->views_render_settings[view_name];
                }
                else
                {
                    *imgui_user_settings = *new_it->second.render_settings;
                    imgui_user_settings->sync_camera = PNANOVDB_TRUE;
                }
            };

            // Handle pending UI selection change (Gaussian views only)
            if (!imgui_user_instance->pending.viewport_gaussian_view.empty() &&
                imgui_user_instance->pending.viewport_gaussian_view != imgui_user_instance->selected_scene_item)
            {
                save_current_view_state(imgui_user_instance->selected_scene_item);

                // Update both editor and UI to the new selection
                const auto& new_view_name = imgui_user_instance->pending.viewport_gaussian_view;
                views->current_view_scene = new_view_name;
                imgui_user_instance->selected_scene_item = new_view_name;
                imgui_user_instance->selected_view_type = imgui_instance_user::ViewsTypes::GaussianScenes;

                load_view_into_editor_and_ui(new_view_name);

                imgui_user_instance->pending.viewport_gaussian_view.clear();
            }
            // Handle editor-driven selection change (only if no pending UI change)
            // Do not override when the user has explicitly selected a camera in the Scene window
            else if (views->current_view_scene != imgui_user_instance->selected_scene_item &&
                     imgui_user_instance->selected_view_type != imgui_instance_user::ViewsTypes::Cameras)
            {
                save_current_view_state(imgui_user_instance->selected_scene_item);

                // Update UI to match editor's selection
                imgui_user_instance->selected_scene_item = views->current_view_scene;

                if (editor->impl->nanovdb_array)
                {
                     imgui_user_instance->selected_view_type = imgui_instance_user::ViewsTypes::NanoVDBs;
                }
                else if (editor->impl->gaussian_data)
                {
                    imgui_user_instance->selected_view_type = imgui_instance_user::ViewsTypes::GaussianScenes;
                }

                load_view_into_editor_and_ui(views->current_view_scene);
            }

            if (editor->impl->gaussian_data && editor->impl->raster_ctx)
            {
                bool has_raster_editor_params =
                    editor->impl->shader_params_data_type &&
                    pnanovdb_reflect_layout_compare(
                        editor->impl->shader_params_data_type, raster_shader_params_data_type) == PNANOVDB_TRUE;

                if (has_raster_editor_params && editor->impl->shader_params)
                {
                    if (editor->impl->editor_worker)
                    {
                        EditorWorker* worker = static_cast<EditorWorker*>(editor->impl->editor_worker);
                        if (worker->set_params.load() > 0)
                        {
                            // Push editor params to UI
                            pnanovdb_raster_shader_params_t* raster_params =
                                (pnanovdb_raster_shader_params_t*)editor->impl->shader_params;
                            init_camera_overrides(raster_params);
                            copy_editor_params_to_ui();
                            sync_camera_from_params_to_ui(raster_params);
                            worker->set_params.fetch_sub(1);
                        }
                        else
                        {
                            // Pull UI params for camera sync
                            raster2d_shader_params_array = get_ui_params();
                            pnanovdb_raster_shader_params_t* raster_params =
                                (pnanovdb_raster_shader_params_t*)raster2d_shader_params_array->data;
                            sync_camera_from_params_to_ui(raster_params);
                        }

                        if (worker->get_params.load() > 0)
                        {
                            // Copy UI params back to editor
                            if (raster2d_shader_params_array && raster2d_shader_params_array->data)
                            {
                                size_t data_size = raster2d_shader_params_array->element_count *
                                                   raster2d_shader_params_array->element_size;
                                std::memcpy(editor->impl->shader_params, raster2d_shader_params_array->data, data_size);
                            }
                            worker->get_params.fetch_sub(1);
                        }
                    }
                    else
                    {
                        pnanovdb_raster_shader_params_t* raster_params =
                            (pnanovdb_raster_shader_params_t*)editor->impl->shader_params;
                        init_camera_overrides(raster_params);
                        copy_editor_params_to_ui();

                        // Clear editor params so UI becomes source of truth
                        editor->impl->shader_params = nullptr;
                        editor->impl->shader_params_data_type = nullptr;
                    }
                }
                else // Use UI params as source of truth
                {
                    raster2d_shader_params_array = get_ui_params();

                    // Sync camera from UI params
                    pnanovdb_raster_shader_params_t* raster_params =
                        (pnanovdb_raster_shader_params_t*)raster2d_shader_params_array->data;
                    sync_camera_from_params_to_ui(raster_params);
                }
            }
        }
        // TODO NanoVDB scene views

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
                    imgui_user_instance->shader_params.get_compute_array_for_shader<ShaderParams>(
                        "raster/gaussian_frag_color.slang", editor->impl->compute);

                // create new default params
                pending_raster_params = &init_raster_shader_params;

                std::filesystem::path fsPath(pending_raster_filepath);
                std::string filename = fsPath.stem().string();
                imgui_user_instance->loaded.filenames.insert(filename);
                pending_raster_params->name = (*imgui_user_instance->loaded.filenames.rbegin()).c_str();
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
        {
            if (raster_worker.isTaskRunning(raster_task_id))
            {
                imgui_user_instance->progress.text = raster_worker.getTaskProgressText(raster_task_id);
                imgui_user_instance->progress.value = raster_worker.getTaskProgress(raster_task_id);
            }
            else if (raster_worker.isTaskCompleted(raster_task_id))
            {
                if (raster_worker.isTaskSuccessful(raster_task_id))
                {
                    // Update viewport shader if needed
                    if (imgui_user_instance->viewport_option == imgui_instance_user::ViewportOption::NanoVDB)
                    {
                        // TODO replace with editor->add_nanovdb_array()
                        editor->impl->nanovdb_array = pending_nanovdb_array;

                        if (imgui_user_instance->shader_name != s_raster_shader)
                        {
                            imgui_user_instance->shader_name = s_raster_shader;
                            imgui_user_instance->pending.viewport_shader_name = s_raster_shader;
                            imgui_user_instance->pending.update_shader = true;
                        }
                    }
                    else if (imgui_user_instance->viewport_option == imgui_instance_user::ViewportOption::Raster2D)
                    {
                        auto ptr = pnanovdb_raster::cast(pending_gaussian_data);

                        pnanovdb_raster_shader_params_t* raster_params =
                            (pnanovdb_raster_shader_params_t*)ptr->shader_params->data;

                        for (auto itPrev = imgui_user_instance->loaded.gaussian_views.begin();
                             itPrev != imgui_user_instance->loaded.gaussian_views.end(); ++itPrev)
                        {
                            if (pending_raster_params->name == itPrev->shader_params->name)
                            {
                                old_gaussian_data_ptr = itPrev->gaussian_data;
                                imgui_user_instance->loaded.gaussian_views.erase(itPrev);
                                break;
                            }
                        }

                        auto& it = imgui_user_instance->loaded.gaussian_views.emplace_back();
                        it.raster_ctx = pending_raster_ctx;
                        it.gaussian_data = std::shared_ptr<pnanovdb_raster_gaussian_data_t>(
                            pending_gaussian_data,
                            [destroy_fn = raster.destroy_gaussian_data, compute = raster.compute,
                             queue = device_queue](pnanovdb_raster_gaussian_data_t* ptr)
                            {
                                destroy_fn(compute, queue, ptr);
                                // printf("Destroyed gaussian data - %p\n", ptr);
                            });
                        it.shader_params = pending_raster_params;
                        it.render_settings = nullptr;
                        // printf("Created gaussian data - %p\n", pending_gaussian_data);

                        editor->add_gaussian_data(editor, pending_raster_ctx, device_queue, pending_gaussian_data);
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
        }

        // update viewport according to the selected option
        if (imgui_user_instance->viewport_option == imgui_instance_user::ViewportOption::NanoVDB)
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
                    dispatch_shader = true;
                }
            }
            if (dispatch_shader && editor->impl->nanovdb_array)
            {
                EditorParams editor_params = {};
                editor_params.view_inv = pnanovdb_camera_mat_transpose(view_inv);
                editor_params.projection_inv = pnanovdb_camera_mat_transpose(projection_inv);
                editor_params.view = pnanovdb_camera_mat_transpose(view);
                editor_params.projection = pnanovdb_camera_mat_transpose(projection);
                editor_params.width = image_width;
                editor_params.height = image_height;

                EditorParams* mapped =
                    (EditorParams*)pnanovdb_compute_upload_buffer_map(compute_context, &compute_upload_buffer, 16u);
                *mapped = editor_params;
                auto* upload_transient = pnanovdb_compute_upload_buffer_unmap(compute_context, &compute_upload_buffer);

#    if NANOVDB_EDITOR_SHADER_PARAMS_SIZE
                ShaderParams* shader_params_mapped = (ShaderParams*)pnanovdb_compute_upload_buffer_map(
                    compute_context, &shader_params_upload_buffer, NANOVDB_EDITOR_SHADER_PARAMS_SIZE);
#    else
                ShaderParams* shader_params_mapped = (ShaderParams*)pnanovdb_compute_upload_buffer_map(
                    compute_context, &shader_params_upload_buffer, 16u);
#    endif
                editor->impl->compute->destroy_array(viewport_shader_params_array);
                viewport_shader_params_array =
                    imgui_user_instance->shader_params.get_compute_array_for_shader<ShaderParams>(
                        imgui_user_instance->shader_name.c_str(), editor->impl->compute);
                if (viewport_shader_params_array)
                {
                    size_t data_size =
                        viewport_shader_params_array->element_count * viewport_shader_params_array->element_size;
                    std::memcpy(shader_params_mapped, viewport_shader_params_array->data, data_size);
                }
                auto* shader_upload_transient =
                    pnanovdb_compute_upload_buffer_unmap(compute_context, &shader_params_upload_buffer);

                pnanovdb_compute_buffer_transient_t* readback_transient = nullptr;
                if (should_capture)
                {
                    pnanovdb_compute_buffer_desc_t readback_desc = {};
                    readback_desc.size_in_bytes = pnanovdb_uint64_t(image_width * image_height * 4u);
                    readback_desc.usage = PNANOVDB_COMPUTE_BUFFER_USAGE_COPY_DST;
                    readback_buffer = compute_interface->create_buffer(
                        compute_context, PNANOVDB_COMPUTE_MEMORY_TYPE_READBACK, &readback_desc);
                    readback_transient =
                        compute_interface->register_buffer_as_transient(compute_context, readback_buffer);
                }

                if (editor->impl->nanovdb_array != uploaded_nanovdb_array && nanovdb_buffer)
                {
                    compute_interface->destroy_buffer(compute_context, nanovdb_buffer);
                    nanovdb_buffer = nullptr;
                }

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
        else if (imgui_user_instance->viewport_option == imgui_instance_user::ViewportOption::Raster2D)
        {
            if (editor->impl->gaussian_data && editor->impl->raster_ctx)
            {
                const pnanovdb_raster_shader_params_t* raster_shader_params =
                    (pnanovdb_raster_shader_params_t*)raster2d_shader_params_array->data;
                pnanovdb_raster_gaussian_data_t* current_gaussian_data = editor->impl->gaussian_data;

                raster.raster_gaussian_2d(raster.compute, device_queue, editor->impl->raster_ctx, current_gaussian_data,
                                          background_image, image_width, image_height, &view, &projection,
                                          raster_shader_params);
            }
            else
            {
                cleanup_background();
            }
        }

        // update camera settings
        if (imgui_user_instance->pending.save_camera)
        {
            imgui_user_instance->pending.save_camera = false;

            imgui_user_instance->saved_render_settings[imgui_user_instance->render_settings_name] = *imgui_user_settings;

            pnanovdb_camera_state_t camera_state = {};
            imgui_window_iface->get_camera(imgui_window, &camera_state, nullptr);
            imgui_user_instance->saved_render_settings[imgui_user_instance->render_settings_name].camera_state =
                camera_state;

            imgui_user_instance->pending.save_render_settings = true;
        }
        if (imgui_user_instance->pending.load_camera)
        {
            imgui_user_instance->pending.load_camera = false;

            *imgui_user_settings = imgui_user_instance->saved_render_settings[imgui_user_instance->render_settings_name];
            imgui_user_settings->sync_camera = PNANOVDB_TRUE;

            imgui_user_instance->pending.load_render_settings = true;
        }
        if (editor->impl->camera && imgui_user_settings->sync_camera == PNANOVDB_FALSE)
        {
            imgui_window_iface->get_camera(imgui_window, &editor->impl->camera->state, &editor->impl->camera->config);
        }
#else
        // default to NanoVDB viewport if there is no imgui instance
        if (editor->impl->nanovdb_array != uploaded_nanovdb_array)
        {
            shader_context = editor->impl->compute->create_shader_context(s_default_shader);
            editor->impl->compute->init_shader(editor->impl->compute, device_queue, shader_context, &compile_settings);

            ShaderParams shader_params = {};
            EditorParams editor_params = {};
            editor_params.view_inv = pnanovdb_camera_mat_transpose(view_inv);
            editor_params.projection_inv = pnanovdb_camera_mat_transpose(projection_inv);
            editor_params.view = pnanovdb_camera_mat_transpose(view);
            editor_params.projection = pnanovdb_camera_mat_transpose(projection);
            editor_params.width = image_width;
            editor_params.height = image_height;

            editor->impl->compute->dispatch_shader_on_nanovdb_array(
                compute_interface, compute_context, shader_context, editor->impl->nanovdb_array, image_width,
                image_height, background_image, upload_transient, user_upload_transient, nanovdb_buffer, nullptr, );
            uploaded_nanovdb_array = editor->nanovdb_array;
        }
#endif
        imgui_window_iface->update_camera(imgui_window, imgui_user_settings);

        // update viewport image
        should_run = imgui_window_iface->update(
            editor->impl->compute, device_queue,
            background_image ? compute_interface->register_texture_as_transient(compute_context, background_image) :
                               nullptr,
            &image_width, &image_height, imgui_window, imgui_user_settings, editor_get_external_active_count, editor);

        if (background_image)
        {
            compute_interface->destroy_texture(compute_context, background_image);
        }

        if (should_capture && readback_buffer)
        {
            editor->impl->compute->device_interface.wait_idle(device_queue);

            float* mapped_data = (float*)compute_interface->map_buffer(compute_context, readback_buffer);
            save_image(capture_filename.c_str(), mapped_data, image_width, image_height);
            compute_interface->unmap_buffer(compute_context, readback_buffer);
        }
    }
    editor->impl->compute->device_interface.wait_idle(device_queue);

    editor->impl->compute->destroy_array(viewport_shader_params_array);
    editor->impl->compute->destroy_array(raster2d_shader_params_array);

    // for (auto& it : imgui_user_instance->loaded.nanovdb_arrays)
    // {
    //     editor->impl->compute->destroy_array(it);
    // }

    imgui_user_instance->loaded.gaussian_views.clear();

    pnanovdb_raster_free(&raster);

    editor->impl->compute->device_interface.disable_profiler(compute_context);

    editor->impl->compute->destroy_shader(
        compute_interface, &editor->impl->compute->shader_interface, compute_context, shader_context);
    editor->impl->compiler->destroy_instance(compiler_inst);

    pnanovdb_compute_upload_buffer_destroy(compute_context, &compute_upload_buffer);
    pnanovdb_compute_upload_buffer_destroy(compute_context, &shader_params_upload_buffer);

    imgui_window_iface->destroy(editor->impl->compute, device_queue, imgui_window, imgui_user_settings);
}

void start(pnanovdb_editor_t* editor, pnanovdb_compute_device_t* device, pnanovdb_editor_config_t* config)
{
#if !defined(NANOVDB_EDITOR_USE_GLFW)
    if (editor->impl->editor_worker)
    {
        return;
    }
    auto* editor_worker = new EditorWorker();
    editor_worker->thread = new std::thread([editor, device, config]() { editor->show(editor, device, config); });
    editor->impl->editor_worker = (void*)editor_worker;
#else
    editor->show(editor, device, config);
#endif
}

void stop(pnanovdb_editor_t* editor)
{
#if !defined(NANOVDB_EDITOR_USE_GLFW)
    if (!editor->impl->editor_worker)
    {
        return;
    }
    auto* editor_worker = static_cast<EditorWorker*>(editor->impl->editor_worker);
    editor_worker->should_stop.store(true);
    editor_worker->thread->join();
    delete editor_worker->thread;
    delete editor_worker;
    editor->impl->editor_worker = nullptr;
#endif
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

    return &editor;
}
}
