// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/editor/Renderer.h

    \author Petra Hapalova

    \brief  Renderer class that manages rendering of different object types
*/

#ifndef NANOVDB_EDITOR_RENDERER_H_HAS_BEEN_INCLUDED
#define NANOVDB_EDITOR_RENDERER_H_HAS_BEEN_INCLUDED

#include "nanovdb_editor/putil/Editor.h"
#include "nanovdb_editor/putil/Raster.h"
#include "nanovdb_editor/putil/Compute.h"
#include "nanovdb_editor/putil/WorkerThread.hpp"
#include "../imgui/UploadBuffer.h"

#include <string>
#include <mutex>

namespace imgui_instance_user
{
struct Instance;
}

namespace pnanovdb_editor
{

// Forward declarations
class EditorScene;
class EditorSceneManager;

// Shader dispatch result
enum class ShaderDispatchResult
{
    Success, ///< Shader dispatched successfully
    CompilationFailed, ///< Shader compilation failed
    NoData, ///< No data to render
    Skipped ///< Dispatch was skipped
};

/*!
    \brief Render type for scene objects

    Each object can specify how it should be rendered.
*/
enum class RenderType
{
    None, ///< Not renderable
    NanoVDB, ///< Volume rendering using compute shader
    Raster2D, ///< 2D rasterization (e.g., Gaussian splatting)
};

// Forward declaration
enum class SceneObjectType;

/*!
    \brief Convert SceneObjectType to RenderType

    Maps scene object types to their corresponding render types.

    \param type The scene object type
    \return The corresponding render type
*/
RenderType get_render_type_from_scene_object_type(SceneObjectType type);

/*!
    \brief Configuration for the Renderer
*/
struct RendererConfig
{
    const pnanovdb_compute_t* compute = nullptr;
    pnanovdb_compute_device_t* device = nullptr;
    pnanovdb_compute_queue_t* device_queue = nullptr;
    pnanovdb_compute_queue_t* compute_queue = nullptr; // For NanoVDB rasterization
    pnanovdb_raster_t* raster = nullptr;
    pnanovdb_raster_context_t* raster_ctx = nullptr;
};

/*!
    \brief Manages rendering for different object types

    The Renderer class abstracts away the rendering logic from the main editor loop.
    It handles both NanoVDB volume rendering and 2D rasterization based on the object's RenderType.
*/
class Renderer
{
public:
    Renderer() = default;
    ~Renderer() = default;

    /*!
        \brief Initialize the renderer with configuration

        \param config Configuration structure containing compute/raster interfaces
    */
    void init(const RendererConfig& config);

    /*!
        \brief Render a NanoVDB volume

        \param nanovdb_array The NanoVDB data to render
        \param shader_context Compiled shader context
        \param background_image Output texture
        \param view Camera view matrix
        \param projection Camera projection matrix
        \param image_width Viewport width
        \param image_height Viewport height
        \param editor_params_buffer Constant buffer with camera parameters
        \param shader_params_buffer Constant buffer with shader parameters
        \param nanovdb_buffer Cached NanoVDB buffer (in/out)
        \param uploaded_nanovdb_array Track which array is uploaded (in/out)
        \return true if rendering succeeded
    */
    bool render_nanovdb(pnanovdb_compute_array_t* nanovdb_array,
                        pnanovdb_shader_context_t* shader_context,
                        pnanovdb_compute_texture_t* background_image,
                        const pnanovdb_camera_mat_t& view,
                        const pnanovdb_camera_mat_t& projection,
                        uint32_t image_width,
                        uint32_t image_height,
                        pnanovdb_compute_buffer_transient_t* editor_params_buffer,
                        pnanovdb_compute_buffer_transient_t* shader_params_buffer,
                        pnanovdb_compute_buffer_t** nanovdb_buffer,
                        pnanovdb_compute_array_t** uploaded_nanovdb_array);

    /*!
        \brief Render Gaussian splatting data

        \param gaussian_data The Gaussian data to render
        \param background_image Output texture
        \param view Camera view matrix
        \param projection Camera projection matrix
        \param image_width Viewport width
        \param image_height Viewport height
        \param raster_params Raster shader parameters
        \return true if rendering succeeded
    */
    bool render_gaussian(pnanovdb_raster_gaussian_data_t* gaussian_data,
                         pnanovdb_compute_texture_t* background_image,
                         const pnanovdb_camera_mat_t& view,
                         const pnanovdb_camera_mat_t& projection,
                         uint32_t image_width,
                         uint32_t image_height,
                         const pnanovdb_raster_shader_params_t* raster_params);

    /*!
        \brief Check if renderer is initialized

        \return true if init() was called with valid config
    */
    bool is_initialized() const
    {
        return m_initialized;
    }

    /*!
        \brief Cleanup and destroy renderer resources

        Should be called before destroying the renderer to properly clean up
        shader contexts, buffers, and other resources.
    */
    void cleanup();

    /*!
        \brief Dispatch NanoVDB shader rendering

        Handles shader compilation, parameter setup, and dispatch for NanoVDB rendering.

        \param nanovdb_array The NanoVDB data to render
        \param shader_name Name of the shader to use
        \param background_image Output texture
        \param view Camera view matrix
        \param projection Camera projection matrix
        \param image_width Viewport width
        \param image_height Viewport height
        \param imgui_instance ImGui instance for settings and state
        \param editor_scene Scene manager for shader params
        \param scene_manager Scene manager for shader refresh
        \return Result of shader dispatch operation
    */
    ShaderDispatchResult dispatch_nanovdb_shader(pnanovdb_compute_array_t* nanovdb_array,
                                                 const char* shader_name,
                                                 pnanovdb_compute_texture_t* background_image,
                                                 const pnanovdb_camera_mat_t& view,
                                                 const pnanovdb_camera_mat_t& projection,
                                                 uint32_t image_width,
                                                 uint32_t image_height,
                                                 imgui_instance_user::Instance* imgui_instance,
                                                 EditorScene* editor_scene,
                                                 EditorSceneManager* scene_manager);

    /*!
        \brief Start rasterization task

        \param raster_filepath Path to file to rasterize
        \param voxels_per_unit Voxels per unit for rasterization
        \param rasterize_to_nanovdb Whether to rasterize to NanoVDB or Gaussian
        \param editor_scene Scene manager for handling results
        \param scene_manager Scene manager for shader params
        \param compute_queue Compute queue for NanoVDB rasterization
        \param device_queue Device queue for Gaussian rasterization
        \return true if rasterization was started successfully
    */
    bool start_rasterization(const char* raster_filepath,
                             float voxels_per_unit,
                             bool rasterize_to_nanovdb,
                             EditorScene* editor_scene,
                             class EditorSceneManager* scene_manager);

    /*!
        \brief Check if rasterization is in progress

        \return true if a rasterization task is running
    */
    bool is_rasterizing();

    /*!
        \brief Get rasterization progress

        \param progress_text Output text describing progress
        \param progress_value Output progress value (0.0 to 1.0)
        \return true if rasterization is in progress
    */
    bool get_rasterization_progress(std::string& progress_text, float& progress_value);

    /*!
        \brief Check and handle rasterization completion

        \param editor_scene Scene manager for handling results
        \param old_gaussian_data_ptr Output parameter for old gaussian data (for deferred destruction)
        \return true if a task completed this frame (successful or not)
    */
    bool handle_rasterization_completion(EditorScene* editor_scene,
                                         std::shared_ptr<pnanovdb_raster_gaussian_data_t>& old_gaussian_data_ptr);

private:
    // Internal structure for camera/editor parameters (mirrored from shader)
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

    bool m_initialized = false;
    RendererConfig m_config;

    // Shader state
    pnanovdb_shader_context_t* m_shader_context = nullptr;
    pnanovdb_compute_buffer_t* m_nanovdb_buffer = nullptr;
    pnanovdb_compute_array_t* m_uploaded_nanovdb_array = nullptr;
    pnanovdb_compute_upload_buffer_t m_compute_upload_buffer;
    pnanovdb_compute_upload_buffer_t m_shader_params_upload_buffer;
    bool m_dispatch_shader = true;

    // Rasterization worker thread
    pnanovdb_util::WorkerThread m_raster_worker;
    pnanovdb_util::WorkerThread::TaskId m_raster_task_id = pnanovdb_util::WorkerThread::invalidTaskId();

    // Rasterization state
    std::string m_pending_raster_filepath;
    float m_pending_voxel_size = 1.f / 128.f;
    pnanovdb_raster_gaussian_data_t* m_pending_gaussian_data = nullptr;
    pnanovdb_raster_context_t* m_pending_raster_ctx = nullptr;
    pnanovdb_raster_shader_params_t* m_pending_raster_params = nullptr;
    pnanovdb_compute_array_t* m_pending_nanovdb_array = nullptr;
    pnanovdb_compute_array_t* m_pending_shader_params_arrays[pnanovdb_raster::shader_param_count];
    pnanovdb_raster_shader_params_t m_init_raster_shader_params;
    const pnanovdb_reflect_data_type_t* m_raster_shader_params_data_type = nullptr;
};

} // namespace pnanovdb_editor

#endif // NANOVDB_EDITOR_RENDERER_H_HAS_BEEN_INCLUDED
