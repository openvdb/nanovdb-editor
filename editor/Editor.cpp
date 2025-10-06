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

#include "nanovdb_editor/putil/Raster.hpp"
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
    PendingData<pnanovdb_raster_gaussian_data_t> pending_gaussian_data;
    PendingData<pnanovdb_camera_t> pending_camera;
    PendingData<void> pending_shader_params;
    ConstPendingData<pnanovdb_reflect_data_type_t> pending_shader_params_data_type;
};

struct EditorView
{
    std::map<std::string, pnanovdb_camera_view_t*> cameras;
};

enum class ViewportShader : int
{
    Editor
};

static const char* s_viewport_shaders[] = {
    "editor/gaussians.slang",
    "editor/ellipsoids.slang",
    "editor/editor.slang",
    "editor/wireframe.slang" };

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

void init(pnanovdb_editor_t* editor)
{
    editor->views = new EditorView();
}

void shutdown(pnanovdb_editor_t* editor)
{
    if (editor->data_array)
    {
        editor->compute->destroy_array(editor->data_array);
        editor->data_array = nullptr;
    }
    if (editor->nanovdb_array)
    {
        editor->compute->destroy_array(editor->nanovdb_array);
        editor->nanovdb_array = nullptr;
    }
    if (editor->views)
    {
        delete static_cast<EditorView*>(editor->views);
    }
}

void add_nanovdb(pnanovdb_editor_t* editor, pnanovdb_compute_array_t* nanovdb_array)
{
    if (!nanovdb_array)
    {
        return;
    }
    if (editor->editor_worker)
    {
        EditorWorker* worker = static_cast<EditorWorker*>(editor->editor_worker);
        worker->pending_nanovdb.set_pending(nanovdb_array);
    }
    else
    {
        if (editor->nanovdb_array)
        {
            editor->compute->destroy_array(editor->nanovdb_array);
        }
        editor->nanovdb_array = nanovdb_array;
    }
}

void add_array(pnanovdb_editor_t* editor, pnanovdb_compute_array_t* data_array)
{
    if (!data_array)
    {
        return;
    }
    if (editor->editor_worker)
    {
        EditorWorker* worker = static_cast<EditorWorker*>(editor->editor_worker);
        worker->pending_data_array.set_pending(data_array);
    }
    else
    {
        if (editor->data_array)
        {
            editor->compute->destroy_array(editor->data_array);
        }
        editor->data_array = data_array;
    }
}
void add_gaussian_data(pnanovdb_editor_t* editor,
                       pnanovdb_raster_t* raster,
                       pnanovdb_compute_queue_t* queue,
                       pnanovdb_raster_gaussian_data_t* gaussian_data)
{
    if (editor->editor_worker)
    {
        EditorWorker* worker = static_cast<EditorWorker*>(editor->editor_worker);
        worker->pending_gaussian_data.set_pending(gaussian_data);
    }
    else
    {
        if (editor->gaussian_data)
        {
            raster->destroy_gaussian_data(editor->compute, queue, editor->gaussian_data);
        }
        editor->gaussian_data = gaussian_data;
    }
}

void add_camera(pnanovdb_editor_t* editor, pnanovdb_camera_t* camera)
{
    if (editor->editor_worker)
    {
        EditorWorker* worker = static_cast<EditorWorker*>(editor->editor_worker);
        worker->pending_camera.set_pending(camera);
    }
    else
    {
        editor->camera = camera;
    }
}

void add_camera_view(pnanovdb_editor_t* editor, pnanovdb_camera_view_t* camera)
{
    EditorView* views = static_cast<EditorView*>(editor->views);
    if (!views || !camera)
    {
        return;
    }
    // replace existing view if name matches
    views->cameras[camera->name] = camera;
}

void add_shader_params(pnanovdb_editor_t* editor, void* params, const pnanovdb_reflect_data_type_t* data_type)
{
    if (!params || !data_type)
    {
        return;
    }
    if (editor->editor_worker)
    {
        EditorWorker* worker = static_cast<EditorWorker*>(editor->editor_worker);
        worker->pending_shader_params.set_pending(params);
        worker->pending_shader_params_data_type.set_pending(data_type);
    }
    else
    {
        editor->shader_params = params;
        editor->shader_params_data_type = data_type;
    }
}

void sync_shader_params(pnanovdb_editor_t* editor, const pnanovdb_reflect_data_type_t* data_type, pnanovdb_bool_t set_data)
{
    if (!editor->editor_worker)
    {
        return;
    }
    // TODO don't have set_params and get_params per data_type now
    EditorWorker* worker = static_cast<EditorWorker*>(editor->editor_worker);
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
    if (!editor->editor_worker)
    {
        return 0;
    }

    auto worker = static_cast<EditorWorker*>(editor->editor_worker);
    pnanovdb_int32_t count = 0;
    if (worker->set_params.load() > 0 || worker->get_params.load() > 0)
    {
        count = 1;
    }
    return count;
};

void show(pnanovdb_editor_t* editor, pnanovdb_compute_device_t* device, pnanovdb_editor_config_t* config)
{
    if (!editor->compute || !editor->compiler || !device || !config)
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
    pnanovdb_imgui_window_t* imgui_window =
        imgui_window_iface->create(editor->compute, device, image_width, image_height, (void**)&imgui_user_settings,
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

    if (editor->camera)
    {
        imgui_user_settings->camera_state = editor->camera->state;
        imgui_user_settings->camera_config = editor->camera->config;
        imgui_user_settings->sync_camera = PNANOVDB_TRUE;
    }

    // Automatically enable encoder when streaming is requested
    if (config->streaming)
    {
        imgui_user_settings->enable_encoder = PNANOVDB_TRUE;
        if (config->ip_address != nullptr)
        {
            snprintf(imgui_user_settings->server_address, sizeof(imgui_user_settings->server_address), "%s",
                     config->ip_address);
        }
        imgui_user_settings->server_port = config->port;
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
    pnanovdb_compiler_instance_t* compiler_inst = editor->compiler->create_instance();
    pnanovdb_compute_queue_t* device_queue = editor->compute->device_interface.get_device_queue(device);
    pnanovdb_compute_queue_t* compute_queue = editor->compute->device_interface.get_compute_queue(device); // used for a
                                                                                                           // worker
                                                                                                           // thread
    pnanovdb_compute_interface_t* compute_interface =
        editor->compute->device_interface.get_compute_interface(device_queue);
    pnanovdb_compute_context_t* compute_context = editor->compute->device_interface.get_compute_context(device_queue);

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

    pnanovdb_shader_context_t* shader_context = nullptr;
    pnanovdb_shader_context_t* shader_context_sortpass = nullptr;
    pnanovdb_shader_context_t* shader_context_prepass = nullptr;
    pnanovdb_compute_buffer_t* nanovdb_buffer = nullptr;
    pnanovdb_compute_array_t* viewport_shader_params_array = nullptr;
    pnanovdb_compute_array_t* uploaded_nanovdb_array = nullptr;

    pnanovdb_raster_t raster = {};
    pnanovdb_raster_load(&raster, editor->compute);

    pnanovdb_util::WorkerThread raster_worker;
    pnanovdb_util::WorkerThread::TaskId raster_task_id = pnanovdb_util::WorkerThread::invalidTaskId();
    std::string pending_raster_filepath;
    float pending_voxel_size = 1.f / 128.f;
    pnanovdb_raster_gaussian_data_t* pending_gaussian_data = nullptr;
    pnanovdb_raster_context_t* pending_raster_ctx = nullptr;
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

    // init with imgui values
    pnanovdb_compute_array_t* raster2d_shader_params_array =
        editor->compute->create_array(raster_shader_params_data_type->element_size, 1u, &init_raster_shader_params);
    imgui_user_instance->shader_params.set_compute_array_for_shader(raster2d_shader_name, raster2d_shader_params_array);

    editor->compute->device_interface.enable_profiler(
        compute_context, (void*)"editor", pnanovdb_editor::Profiler::report_callback);
    editor->compute->device_interface.get_memory_stats(device, Profiler::getInstance().getMemoryStats());

    // views UI
    imgui_user_instance->camera_views = &(static_cast<EditorView*>(editor->views)->cameras);

#ifdef USE_IMGUI_INSTANCE
    ShaderCallback callback =
        pnanovdb_editor::get_shader_recompile_callback(imgui_user_instance, editor->compiler, compiler_inst);
    monitor_shader_dir(pnanovdb_shader::getShaderDir().c_str(), callback);

    imgui_user_instance->set_default_shader(s_default_shader);

    if (editor->nanovdb_array && editor->nanovdb_array->filepath)
    {
        imgui_user_instance->nanovdb_filepath = editor->nanovdb_array->filepath;
    }

    if (editor->nanovdb_array)
    {
        imgui_user_instance->nanovdb_array = std::shared_ptr<pnanovdb_compute_array_t>(editor->nanovdb_array,
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

    imgui_user_instance->compiler = editor->compiler;
    imgui_user_instance->compute = editor->compute;

    bool dispatch_shader = true;

    editor->compiler->set_diagnostic_callback(compiler_inst,
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
        if (editor->gaussian_data)
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
        if (editor->editor_worker)
        {
            bool updated = false;
            auto* worker = static_cast<EditorWorker*>(editor->editor_worker);

            pnanovdb_compute_array_t* old_nanovdb_array = nullptr;
            updated = worker->pending_nanovdb.process_pending(editor->nanovdb_array, old_nanovdb_array);
            if (updated)
            {
                imgui_user_instance->viewport_option = imgui_instance_user::ViewportOption::NanoVDB;
            }
            if (old_nanovdb_array)
            {
                editor->compute->destroy_array(old_nanovdb_array);
                old_nanovdb_array = nullptr;
            }
            pnanovdb_compute_array_t* old_array = nullptr;
            worker->pending_data_array.process_pending(editor->data_array, old_array);
            if (old_array)
            {
                editor->compute->destroy_array(old_array);
                old_array = nullptr;
            }
            pnanovdb_raster_gaussian_data_t* old_gaussian_data = nullptr;
            updated = worker->pending_gaussian_data.process_pending(editor->gaussian_data, old_gaussian_data);
            if (updated)
            {
                imgui_user_instance->viewport_option = imgui_instance_user::ViewportOption::Raster2D;
                imgui_user_instance->pending.shader_selection_mode =
                    imgui_instance_user::ShaderSelectionMode::UseShaderGroup;
                imgui_user_instance->shader_group = "raster/raster2d_group";
            }
            if (old_gaussian_data)
            {
                raster.destroy_gaussian_data(editor->compute, compute_queue, old_gaussian_data);
                old_gaussian_data = nullptr;
            }
            pnanovdb_camera_t* old_camera = nullptr;
            updated = worker->pending_camera.process_pending(editor->camera, old_camera);
            if (updated)
            {
                imgui_user_settings->camera_state = editor->camera->state;
                imgui_user_settings->camera_config = editor->camera->config;
                imgui_user_settings->sync_camera = PNANOVDB_TRUE;
            }
            void* old_shader_params = nullptr;
            worker->pending_shader_params.process_pending(editor->shader_params, old_shader_params);
            const pnanovdb_reflect_data_type_t* old_shader_params_data_type = nullptr;
            updated = worker->pending_shader_params_data_type.process_pending(
                editor->shader_params_data_type, old_shader_params_data_type);
            if (updated)
            {
                if (editor->shader_params &&
                    pnanovdb_reflect_layout_compare(
                        editor->shader_params_data_type, PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_raster_shader_params_t)))
                {
                    // init raster shader param's camera from imgui camera
                    pnanovdb_raster_shader_params_t* raster_params =
                        (pnanovdb_raster_shader_params_t*)editor->shader_params;
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

        if (editor->editor_worker && static_cast<EditorWorker*>(editor->editor_worker)->should_stop.load())
        {
            should_run = false;
            break;
        }

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
            pnanovdb_compute_array_t* loaded_array = editor->compute->load_nanovdb(nvdb_filepath);
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
                imgui_user_instance->pending.shader_name = nvdb_shader;
                imgui_user_instance->pending.update_shader = true;
            }
        }
        if (imgui_user_instance->pending.save_nanovdb)
        {
            imgui_user_instance->pending.save_nanovdb = false;
            const char* nvdb_filepath = imgui_user_instance->nanovdb_filepath.c_str();
            if (editor->nanovdb_array)
            {
                pnanovdb_bool_t result = editor->compute->save_nanovdb(editor->nanovdb_array, nvdb_filepath);
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
                pending_shader_params_arrays[pnanovdb_raster::gaussian_frag_color_slang] =
                    imgui_user_instance->shader_params.get_compute_array_for_shader<ShaderParams>(
                        "raster/gaussian_frag_color.slang", editor->compute);

                raster_task_id = raster_worker.enqueue(
                    [&raster_worker](
                        pnanovdb_raster_t* raster, const pnanovdb_compute_t* compute, pnanovdb_compute_queue_t* queue,
                        const char* filepath, float voxel_size, pnanovdb_compute_array_t** nanovdb_array,
                        pnanovdb_raster_gaussian_data_t** gaussian_data, pnanovdb_raster_context_t** raster_context,
                        pnanovdb_compute_array_t** shader_params_arrays, pnanovdb_profiler_report_t profiler) -> bool
                    {
                        return pnanovdb_raster::raster_file(raster, compute, queue, filepath, voxel_size, nanovdb_array,
                                                            gaussian_data, raster_context, shader_params_arrays,
                                                            profiler, (void*)(&raster_worker));
                    },
                    &raster, raster.compute, compute_queue, pending_raster_filepath.c_str(), pending_voxel_size,
                    imgui_user_instance->viewport_option == imgui_instance_user::ViewportOption::NanoVDB ?
                        &pending_nanovdb_array :
                        nullptr,
                    imgui_user_instance->viewport_option == imgui_instance_user::ViewportOption::Raster2D ?
                        &pending_gaussian_data :
                        nullptr,
                    imgui_user_instance->viewport_option == imgui_instance_user::ViewportOption::Raster2D ?
                        &pending_raster_ctx :
                        nullptr,
                    pending_shader_params_arrays, pnanovdb_editor::Profiler::report_callback);

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
                // Update with new data and reset
                if (pending_gaussian_data)
                {
                    if (editor->gaussian_data)
                    {
                        raster.destroy_gaussian_data(raster.compute, compute_queue, editor->gaussian_data);
                    }
                    editor->gaussian_data = pending_gaussian_data;
                    pending_gaussian_data = nullptr;
                }
                if (pending_raster_ctx)
                {
                    if (editor->raster_ctx)
                    {
                        raster.destroy_context(raster.compute, compute_queue, editor->raster_ctx);
                    }
                    editor->raster_ctx = pending_raster_ctx;
                    pending_raster_ctx = nullptr;
                }
                if (pending_nanovdb_array)
                {
                    if (editor->nanovdb_array)
                    {
                        editor->compute->destroy_array(editor->nanovdb_array);
                    }
                    editor->nanovdb_array = pending_nanovdb_array;
                    pending_nanovdb_array = nullptr;
                }

                for (pnanovdb_uint32_t idx = 0u; idx < pnanovdb_raster::shader_param_count; idx++)
                {
                    editor->compute->destroy_array(pending_shader_params_arrays[idx]);
                }

                if (raster_worker.isTaskSuccessful(raster_task_id))
                {
                    // Update viewport shader if needed
                    if (imgui_user_instance->viewport_option == imgui_instance_user::ViewportOption::NanoVDB)
                    {
                        if (imgui_user_instance->shader_name != s_raster_shader)
                        {
                            imgui_user_instance->shader_name = s_raster_shader;
                            imgui_user_instance->pending.shader_name = s_raster_shader;
                            imgui_user_instance->pending.update_shader = true;
                        }
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

        // update memory stats periodically
        if (imgui_user_instance && imgui_user_instance->pending.update_memory_stats)
        {
            editor->compute->device_interface.get_memory_stats(device, Profiler::getInstance().getMemoryStats());
            imgui_user_instance->pending.update_memory_stats = false;
        }

        // update viewport according to the selected option
        if (imgui_user_instance->viewport_option == imgui_instance_user::ViewportOption::NanoVDB)
        {
            if (imgui_user_instance->pending.update_shader)
            {
                std::lock_guard<std::mutex> lock(imgui_user_instance->compiler_settings_mutex);

                imgui_user_instance->pending.update_shader = false;
                editor->compute->destroy_shader_context(editor->compute, device_queue, shader_context);
                shader_context = editor->compute->create_shader_context(imgui_user_instance->shader_name.c_str());
                if (editor->compute->init_shader(editor->compute, device_queue, shader_context,
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
            if (!shader_context_sortpass)
            {
                shader_context_sortpass = editor->compute->create_shader_context("editor/nanovdb_sortpass.slang");
                editor->compute->init_shader(
                    editor->compute,
                    device_queue,
                    shader_context_sortpass,
                    &imgui_user_instance->compiler_settings);
            }
            if (!shader_context_prepass)
            {
                shader_context_prepass = editor->compute->create_shader_context("editor/nanovdb_prepass.slang");
                editor->compute->init_shader(
                    editor->compute,
                    device_queue,
                    shader_context_prepass,
                    &imgui_user_instance->compiler_settings);
            }
            if (dispatch_shader && editor->nanovdb_array)
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
                if (viewport_shader_params_array)
                {
                    editor->compute->destroy_array(viewport_shader_params_array);
                }
                viewport_shader_params_array =
                    imgui_user_instance->shader_params.get_compute_array_for_shader<ShaderParams>(
                        imgui_user_instance->shader_name.c_str(), editor->compute);
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

                if (editor->nanovdb_array != uploaded_nanovdb_array && nanovdb_buffer)
                {
                    compute_interface->destroy_buffer(compute_context, nanovdb_buffer);
                    nanovdb_buffer = nullptr;
                }

                if (shader_context_sortpass)
                {
                    editor->compute->dispatch_shader_on_nanovdb_array(
                        editor->compute,
                        device,
                        shader_context_sortpass,
                        editor->nanovdb_array,
                        image_width,
                        image_height,
                        background_image,
                        upload_transient,
                        shader_upload_transient,
                        &nanovdb_buffer,
                        &readback_transient
                    );
                }
                if (shader_context_prepass)
                {
                    editor->compute->dispatch_shader_on_nanovdb_array(
                        editor->compute,
                        device,
                        shader_context_prepass,
                        editor->nanovdb_array,
                        image_width,
                        image_height,
                        background_image,
                        upload_transient,
                        shader_upload_transient,
                        &nanovdb_buffer,
                        &readback_transient
                    );
                }

                editor->compute->dispatch_shader_on_nanovdb_array(
                    editor->compute, device, shader_context, editor->nanovdb_array, image_width, image_height,
                    background_image, upload_transient, shader_upload_transient, &nanovdb_buffer, &readback_transient);

                if (nanovdb_buffer)
                {
                    uploaded_nanovdb_array = editor->nanovdb_array;
                }
            }
            else
            {
                cleanup_background();
            }
        }
        else if (imgui_user_instance->viewport_option == imgui_instance_user::ViewportOption::Raster2D)
        {
            if (editor->gaussian_data && editor->raster_ctx)
            {
                if (editor->shader_params_data_type &&
                    pnanovdb_reflect_layout_compare(editor->shader_params_data_type, raster_shader_params_data_type) ==
                        PNANOVDB_TRUE)
                {
                    if (editor->editor_worker && editor->shader_params)
                    {
                        // syncing shader params
                        EditorWorker* worker = static_cast<EditorWorker*>(editor->editor_worker);
                        if (worker->set_params.load() > 0)
                        {
                            pnanovdb_raster_shader_params_t* raster_params =
                                (pnanovdb_raster_shader_params_t*)editor->shader_params;
                            if (raster_params->near_plane_override == 0.f)
                            {
                                raster_params->near_plane_override = imgui_user_settings->camera_config.near_plane;
                            }
                            if (raster_params->far_plane_override == 0.f)
                            {
                                raster_params->far_plane_override = imgui_user_settings->camera_config.far_plane;
                            }

                            // copy the editor shader params to imgui values
                            raster2d_shader_params_array = editor->compute->create_array(
                                raster_shader_params_data_type->element_size, 1u, editor->shader_params);
                            imgui_user_instance->shader_params.set_compute_array_for_shader(
                                raster2d_shader_name, raster2d_shader_params_array);

                            // update camera from editor shader params
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

                            worker->set_params.fetch_sub(1);
                        }
                        else
                        {
                            // update editor shader params from imgui values
                            raster2d_shader_params_array =
                                imgui_user_instance->shader_params
                                    .get_compute_array_for_shader<pnanovdb_raster_shader_params_t>(
                                        raster2d_shader_name, editor->compute);

                            // update imgui camera to imgui values
                            pnanovdb_raster_shader_params_t* raster_params =
                                (pnanovdb_raster_shader_params_t*)raster2d_shader_params_array->data;
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
                        }
                        if (worker->get_params.load() > 0)
                        {
                            if (raster2d_shader_params_array && raster2d_shader_params_array->data)
                            {
                                size_t data_size = raster2d_shader_params_array->element_count *
                                                   raster2d_shader_params_array->element_size;
                                std::memcpy(editor->shader_params, raster2d_shader_params_array->data, data_size);
                            }

                            worker->get_params.fetch_sub(1);
                        }
                    }
                    else if (editor->shader_params)
                    {
                        pnanovdb_raster_shader_params_t* raster_params =
                            (pnanovdb_raster_shader_params_t*)editor->shader_params;
                        if (raster_params->near_plane_override == 0.f)
                        {
                            raster_params->near_plane_override = imgui_user_settings->camera_config.near_plane;
                        }
                        if (raster_params->far_plane_override == 0.f)
                        {
                            raster_params->far_plane_override = imgui_user_settings->camera_config.far_plane;
                        }

                        // copy the editor params to imgui values
                        raster2d_shader_params_array = editor->compute->create_array(
                            raster_shader_params_data_type->element_size, 1u, editor->shader_params);
                        imgui_user_instance->shader_params.set_compute_array_for_shader(
                            raster2d_shader_name, raster2d_shader_params_array);

                        // value will be read from imgui from now on
                        editor->shader_params = nullptr;
                        editor->shader_params_data_type = nullptr;
                    }
                }
                else // don't have shader params with raster data type
                {
                    // destroy array created from imgui default values
                    editor->compute->destroy_array(raster2d_shader_params_array);

                    // update editor shader params from imgui values
                    raster2d_shader_params_array =
                        imgui_user_instance->shader_params.get_compute_array_for_shader<pnanovdb_raster_shader_params_t>(
                            raster2d_shader_name, editor->compute);

                    // update imgui camera to imgui values
                    pnanovdb_raster_shader_params_t* raster_params =
                        (pnanovdb_raster_shader_params_t*)raster2d_shader_params_array->data;
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
                }
                pnanovdb_raster_shader_params_t* raster_shader_params =
                    (pnanovdb_raster_shader_params_t*)raster2d_shader_params_array->data;
                raster.raster_gaussian_2d(raster.compute, device_queue, editor->raster_ctx, editor->gaussian_data,
                                          background_image, image_width, image_height, &view, &projection,
                                          raster_shader_params);

                editor->compute->destroy_array(raster2d_shader_params_array);
                raster2d_shader_params_array = nullptr;
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
        if (editor->camera && imgui_user_settings->sync_camera == PNANOVDB_FALSE)
        {
            imgui_window_iface->get_camera(imgui_window, &editor->camera->state, &editor->camera->config);
        }
#else
        // default to NanoVDB viewport if there is no imgui instance
        if (editor->nanovdb_array != uploaded_nanovdb_array)
        {
            shader_context = editor->compute->create_shader_context(s_default_shader);
            editor->compute->init_shader(editor->compute, device_queue, shader_context, &compile_settings);

            ShaderParams shader_params = {};
            EditorParams editor_params = {};
            editor_params.view_inv = pnanovdb_camera_mat_transpose(view_inv);
            editor_params.projection_inv = pnanovdb_camera_mat_transpose(projection_inv);
            editor_params.view = pnanovdb_camera_mat_transpose(view);
            editor_params.projection = pnanovdb_camera_mat_transpose(projection);
            editor_params.width = image_width;
            editor_params.height = image_height;

            editor->compute->dispatch_shader_on_nanovdb_array(
                compute_interface, compute_context, shader_context, editor->nanovdb_array, image_width, image_height,
                background_image, upload_transient, user_upload_transient, nanovdb_buffer, nullptr, );
            uploaded_nanovdb_array = editor->nanovdb_array;
        }
#endif
        imgui_window_iface->update_camera(imgui_window, imgui_user_settings);

        // update viewport image
        should_run = imgui_window_iface->update(
            editor->compute, device_queue,
            background_image ? compute_interface->register_texture_as_transient(compute_context, background_image) :
                               nullptr,
            &image_width, &image_height, imgui_window, imgui_user_settings, editor_get_external_active_count, editor);

        if (background_image)
        {
            compute_interface->destroy_texture(compute_context, background_image);
        }

        if (should_capture && readback_buffer)
        {
            editor->compute->device_interface.wait_idle(device_queue);

            float* mapped_data = (float*)compute_interface->map_buffer(compute_context, readback_buffer);
            save_image(capture_filename.c_str(), mapped_data, image_width, image_height);
            compute_interface->unmap_buffer(compute_context, readback_buffer);
        }
    }
    editor->compute->device_interface.wait_idle(device_queue);

    editor->compute->destroy_array(viewport_shader_params_array);
    editor->compute->destroy_array(raster2d_shader_params_array);

    if (editor->gaussian_data)
    {
        raster.destroy_gaussian_data(raster.compute, compute_queue, editor->gaussian_data);
        editor->gaussian_data = nullptr;
    }
    if (editor->raster_ctx)
    {
        raster.destroy_context(raster.compute, compute_queue, editor->raster_ctx);
        editor->raster_ctx = nullptr;
    }
    pnanovdb_raster_free(&raster);

    editor->compute->device_interface.disable_profiler(compute_context);

    editor->compute->destroy_shader(
        compute_interface, &editor->compute->shader_interface, compute_context, shader_context);
    editor->compute->destroy_shader(
        compute_interface, &editor->compute->shader_interface, compute_context, shader_context_sortpass);
    editor->compute->destroy_shader(
        compute_interface, &editor->compute->shader_interface, compute_context, shader_context_prepass);
    editor->compiler->destroy_instance(compiler_inst);

    pnanovdb_compute_upload_buffer_destroy(compute_context, &compute_upload_buffer);
    pnanovdb_compute_upload_buffer_destroy(compute_context, &shader_params_upload_buffer);

    imgui_window_iface->destroy(editor->compute, device_queue, imgui_window, imgui_user_settings);
}

void start(pnanovdb_editor_t* editor, pnanovdb_compute_device_t* device, pnanovdb_editor_config_t* config)
{
#if !defined(NANOVDB_EDITOR_USE_GLFW)
    if (editor->editor_worker)
    {
        return;
    }
    auto* editor_worker = new EditorWorker();
    editor_worker->thread = new std::thread([editor, device, config]() { editor->show(editor, device, config); });
    editor->editor_worker = (void*)editor_worker;
#else
    editor->show(editor, device, config);
#endif
}

void stop(pnanovdb_editor_t* editor)
{
#if !defined(NANOVDB_EDITOR_USE_GLFW)
    if (!editor->editor_worker)
    {
        return;
    }
    auto* editor_worker = static_cast<EditorWorker*>(editor->editor_worker);
    editor_worker->should_stop.store(true);
    editor_worker->thread->join();
    delete editor_worker->thread;
    delete editor_worker;
    editor->editor_worker = nullptr;
#endif
}

PNANOVDB_API pnanovdb_editor_t* pnanovdb_get_editor()
{
    static pnanovdb_editor_t editor = { PNANOVDB_REFLECT_INTERFACE_INIT(pnanovdb_editor_t) };

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
    editor.add_camera = add_camera;
    editor.add_camera_view = add_camera_view;

    return &editor;
}
}
