// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/editor/Renderer.cpp

    \author Petra Hapalova

    \brief  Implementation of Renderer class
*/

#include "Renderer.h"
#include "EditorScene.h"
#include "EditorSceneManager.h"
#include "ImguiInstance.h"
#include "Console.h"
#include "Profiler.h"

namespace pnanovdb_editor
{

RenderType get_render_type_from_scene_object_type(SceneObjectType type)
{
    switch (type)
    {
    case SceneObjectType::NanoVDB:
        return RenderType::NanoVDB;
    case SceneObjectType::GaussianData:
        return RenderType::Raster2D;
    case SceneObjectType::Array:
        return RenderType::NanoVDB;
    case SceneObjectType::Camera:
    default:
        return RenderType::None; // Cameras and unknown types are not renderable
    }
}

void Renderer::init(const RendererConfig& config)
{
    m_config = config;
    m_initialized = (config.compute != nullptr && config.device != nullptr && config.device_queue != nullptr);

    // Initialize upload buffers
    pnanovdb_compute_interface_t* compute_interface =
        m_config.compute->device_interface.get_compute_interface(m_config.device_queue);
    pnanovdb_compute_context_t* compute_context =
        m_config.compute->device_interface.get_compute_context(m_config.device_queue);

    if (compute_interface && compute_context)
    {
        pnanovdb_compute_upload_buffer_init(compute_interface, compute_context, &m_compute_upload_buffer,
                                            PNANOVDB_COMPUTE_BUFFER_USAGE_CONSTANT, PNANOVDB_COMPUTE_FORMAT_UNKNOWN, 0u);

        pnanovdb_compute_upload_buffer_init(compute_interface, compute_context, &m_shader_params_upload_buffer,
                                            PNANOVDB_COMPUTE_BUFFER_USAGE_CONSTANT, PNANOVDB_COMPUTE_FORMAT_UNKNOWN, 0u);
    }
}

void Renderer::cleanup()
{
    if (!m_initialized)
    {
        return;
    }

    pnanovdb_compute_interface_t* compute_interface =
        m_config.compute->device_interface.get_compute_interface(m_config.device_queue);
    pnanovdb_compute_context_t* compute_context =
        m_config.compute->device_interface.get_compute_context(m_config.device_queue);

    if (compute_interface && compute_context)
    {
        // Destroy shader context
        if (m_shader_context)
        {
            m_config.compute->destroy_shader(
                compute_interface, &m_config.compute->shader_interface, compute_context, m_shader_context);
            m_shader_context = nullptr;
        }

        // Destroy upload buffers
        pnanovdb_compute_upload_buffer_destroy(compute_context, &m_compute_upload_buffer);
        pnanovdb_compute_upload_buffer_destroy(compute_context, &m_shader_params_upload_buffer);

        // Destroy NanoVDB buffer
        if (m_nanovdb_buffer)
        {
            compute_interface->destroy_buffer(compute_context, m_nanovdb_buffer);
            m_nanovdb_buffer = nullptr;
        }
    }

    m_uploaded_nanovdb_array = nullptr;
    m_initialized = false;
}

bool Renderer::render_nanovdb(pnanovdb_compute_array_t* nanovdb_array,
                              pnanovdb_shader_context_t* shader_context,
                              pnanovdb_compute_texture_t* background_image,
                              const pnanovdb_camera_mat_t& view,
                              const pnanovdb_camera_mat_t& projection,
                              uint32_t image_width,
                              uint32_t image_height,
                              pnanovdb_compute_buffer_transient_t* editor_params_buffer,
                              pnanovdb_compute_buffer_transient_t* shader_params_buffer,
                              pnanovdb_compute_buffer_t** nanovdb_buffer,
                              pnanovdb_compute_array_t** uploaded_nanovdb_array)
{
    if (!m_initialized || !nanovdb_array || !shader_context || !background_image)
    {
        return false;
    }

    pnanovdb_compute_interface_t* compute_interface =
        m_config.compute->device_interface.get_compute_interface(m_config.device_queue);
    pnanovdb_compute_context_t* compute_context =
        m_config.compute->device_interface.get_compute_context(m_config.device_queue);

    if (!compute_interface || !compute_context)
    {
        return false;
    }

    // Check if we need to update the NanoVDB buffer
    if (nanovdb_array != *uploaded_nanovdb_array && *nanovdb_buffer)
    {
        compute_interface->destroy_buffer(compute_context, *nanovdb_buffer);
        *nanovdb_buffer = nullptr;
    }

    // Dispatch shader
    pnanovdb_compute_buffer_transient_t* readback_transient = nullptr;
    m_config.compute->dispatch_shader_on_nanovdb_array(m_config.compute, m_config.device, shader_context, nanovdb_array,
                                                       image_width, image_height, background_image, editor_params_buffer,
                                                       shader_params_buffer, nanovdb_buffer, &readback_transient);

    if (*nanovdb_buffer)
    {
        *uploaded_nanovdb_array = nanovdb_array;
    }

    return true;
}

bool Renderer::render_gaussian(pnanovdb_raster_gaussian_data_t* gaussian_data,
                               pnanovdb_compute_texture_t* background_image,
                               const pnanovdb_camera_mat_t& view,
                               const pnanovdb_camera_mat_t& projection,
                               uint32_t image_width,
                               uint32_t image_height,
                               const pnanovdb_raster_shader_params_t* raster_params)
{
    if (!m_initialized || !gaussian_data || !background_image || !m_config.raster || !m_config.raster_ctx)
    {
        return false;
    }

    m_config.raster->raster_gaussian_2d(m_config.raster->compute, m_config.device_queue, m_config.raster_ctx,
                                        gaussian_data, background_image, image_width, image_height, &view, &projection,
                                        raster_params);

    return true;
}

ShaderDispatchResult Renderer::dispatch_nanovdb_shader(pnanovdb_compute_array_t* nanovdb_array,
                                                       const char* shader_name,
                                                       pnanovdb_compute_texture_t* background_image,
                                                       const pnanovdb_camera_mat_t& view,
                                                       const pnanovdb_camera_mat_t& projection,
                                                       uint32_t image_width,
                                                       uint32_t image_height,
                                                       imgui_instance_user::Instance* imgui_instance,
                                                       EditorScene* editor_scene,
                                                       EditorSceneManager* scene_manager)
{
    if (!m_initialized || !nanovdb_array || !background_image || !shader_name)
    {
        return ShaderDispatchResult::NoData;
    }

    pnanovdb_compute_interface_t* compute_interface =
        m_config.compute->device_interface.get_compute_interface(m_config.device_queue);
    pnanovdb_compute_context_t* compute_context =
        m_config.compute->device_interface.get_compute_context(m_config.device_queue);

    if (!compute_interface || !compute_context)
    {
        return ShaderDispatchResult::NoData;
    }

    // Handle shader updates/compilation
    if (imgui_instance->pending.update_shader)
    {
        std::lock_guard<std::mutex> lock(imgui_instance->compiler_settings_mutex);

        imgui_instance->pending.update_shader = false;

        // Sync shader name from Code Editor to the selected scene object
        if (!imgui_instance->editor_shader_name.empty())
        {
            imgui_instance->editor_scene->set_selected_object_shader_name(imgui_instance->editor_shader_name);
            imgui_instance->editor_shader_name = "";
        }

        // Destroy old shader context and create new one
        m_config.compute->destroy_shader_context(m_config.compute, m_config.device_queue, m_shader_context);
        m_shader_context = m_config.compute->create_shader_context(shader_name);

        if (m_config.compute->init_shader(m_config.compute, m_config.device_queue, m_shader_context,
                                          &imgui_instance->compiler_settings) == PNANOVDB_FALSE)
        {
            // Compilation failed
            m_dispatch_shader = false;
            return ShaderDispatchResult::CompilationFailed;
        }
        else
        {
            // Shader compiled successfully - reload params
            editor_scene->reload_shader_params_for_current_view();

            // Refresh params for all views using this shader
            if (scene_manager)
            {
                scene_manager->refresh_params_for_shader(m_config.compute, shader_name);
            }

            m_dispatch_shader = true;
        }
    }

    // Skip dispatch if compilation failed previously
    if (!m_dispatch_shader)
    {
        return ShaderDispatchResult::Skipped;
    }

    // Setup editor/camera parameters
    pnanovdb_camera_mat_t view_inv = pnanovdb_camera_mat_inverse(view);
    pnanovdb_camera_mat_t projection_inv = pnanovdb_camera_mat_inverse(projection);

    EditorParams editor_params = {};
    editor_params.view_inv = pnanovdb_camera_mat_transpose(view_inv);
    editor_params.projection_inv = pnanovdb_camera_mat_transpose(projection_inv);
    editor_params.view = pnanovdb_camera_mat_transpose(view);
    editor_params.projection = pnanovdb_camera_mat_transpose(projection);
    editor_params.width = image_width;
    editor_params.height = image_height;

    // Upload editor parameters
    EditorParams* mapped = (EditorParams*)pnanovdb_compute_upload_buffer_map(
        compute_context, &m_compute_upload_buffer, sizeof(EditorParams));
    *mapped = editor_params;
    auto* upload_transient = pnanovdb_compute_upload_buffer_unmap(compute_context, &m_compute_upload_buffer);

    // Upload shader parameters
    void* shader_params_data = pnanovdb_compute_upload_buffer_map(
        compute_context, &m_shader_params_upload_buffer, PNANOVDB_COMPUTE_CONSTANT_BUFFER_MAX_SIZE);
    editor_scene->get_shader_params_for_current_view(shader_params_data);
    auto* shader_upload_transient = pnanovdb_compute_upload_buffer_unmap(compute_context, &m_shader_params_upload_buffer);

    // Render NanoVDB
    bool success =
        render_nanovdb(nanovdb_array, m_shader_context, background_image, view, projection, image_width, image_height,
                       upload_transient, shader_upload_transient, &m_nanovdb_buffer, &m_uploaded_nanovdb_array);

    return success ? ShaderDispatchResult::Success : ShaderDispatchResult::Skipped;
}

} // namespace pnanovdb_editor
