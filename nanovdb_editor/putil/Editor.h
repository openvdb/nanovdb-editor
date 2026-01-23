// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/putil/Editor.h

    \author Andrew Reidmeyer

    \brief  This file provides editor interface.
*/

#ifndef NANOVDB_PNANOVDBEDITOR_H_HAS_BEEN_INCLUDED
#define NANOVDB_PNANOVDBEDITOR_H_HAS_BEEN_INCLUDED

#include "nanovdb_editor/putil/Compute.h"
#include "nanovdb_editor/putil/Raster.h"

// ------------------------------------------------ Editor -----------------------------------------------------------

struct pnanovdb_editor_t;
typedef struct pnanovdb_editor_t pnanovdb_editor_t;

typedef struct pnanovdb_editor_config_t
{
    const char* ip_address;
    pnanovdb_int32_t port;
    pnanovdb_bool_t headless;
    pnanovdb_bool_t streaming;
    pnanovdb_bool_t stream_to_file;
    const char* ui_profile_name;
} pnanovdb_editor_config_t;

#define PNANOVDB_EDITOR_RESOLVED_PORT_UNRESOLVED -1
#define PNANOVDB_EDITOR_RESOLVED_PORT_PENDING -2

struct pnanovdb_editor_token_t
{
    pnanovdb_uint64_t id;
    const char* str;
};
typedef struct pnanovdb_editor_token_t pnanovdb_editor_token_t;

#define PNANOVDB_REFLECT_TYPE pnanovdb_editor_token_t
PNANOVDB_REFLECT_BEGIN()
PNANOVDB_REFLECT_VALUE(pnanovdb_uint64_t, id, 0, 0)
PNANOVDB_REFLECT_POINTER(char, str, 0, 0)
PNANOVDB_REFLECT_END(0)
#undef PNANOVDB_REFLECT_TYPE

typedef struct pnanovdb_editor_gaussian_data_desc_t
{
    pnanovdb_compute_array_t* means;
    pnanovdb_compute_array_t* opacities;
    pnanovdb_compute_array_t* quaternions;
    pnanovdb_compute_array_t* scales;
    pnanovdb_compute_array_t* sh_0;
    pnanovdb_compute_array_t* sh_n;
} pnanovdb_editor_gaussian_data_desc_t;

typedef struct pnanovdb_editor_shader_name_t
{
    pnanovdb_editor_token_t* shader_name;
} pnanovdb_editor_shader_name_t;

#define PNANOVDB_REFLECT_TYPE pnanovdb_editor_shader_name_t
PNANOVDB_REFLECT_BEGIN()
PNANOVDB_REFLECT_POINTER(pnanovdb_editor_token_t, shader_name, 0, 0)
PNANOVDB_REFLECT_END(0)
#undef PNANOVDB_REFLECT_TYPE

// ------------------------------------------------ Pipeline API -----------------------------------------------------

typedef enum pnanovdb_pipeline_type_t
{
    PNANOVDB_PIPELINE_TYPE_NULL = 0, ///< No-op pipeline (pass-through)
    PNANOVDB_PIPELINE_TYPE_RENDER = 1, ///< Rendering pipeline (renderable data -> 2D image)
    PNANOVDB_PIPELINE_TYPE_RASTER3D = 2, ///< 3D rasterization (Gaussian data -> NanoVDB)
    PNANOVDB_PIPELINE_TYPE_FILE_IMPORT = 3 ///< File import pipeline
} pnanovdb_pipeline_type_t;

typedef enum pnanovdb_pipeline_status_t
{
    PNANOVDB_PIPELINE_STATUS_NOT_RUN = 0, ///< Pipeline has never been run
    PNANOVDB_PIPELINE_STATUS_RUNNING = 1, ///< Pipeline is currently executing
    PNANOVDB_PIPELINE_STATUS_COMPLETED = 2, ///< Pipeline execution completed successfully
    PNANOVDB_PIPELINE_STATUS_FAILED = 3, ///< Pipeline execution failed
    PNANOVDB_PIPELINE_STATUS_DIRTY = 4 ///< Input or parameters changed, needs re-run
} pnanovdb_pipeline_status_t;

typedef struct pnanovdb_pipeline_param_t
{
    const char* name; ///< Parameter name (e.g., "voxel_size", "opacity_threshold")
    float value; ///< Parameter value
} pnanovdb_pipeline_param_t;

typedef struct pnanovdb_pipeline_settings_t
{
    // Shader configuration (applies to primary shader at index 0)
    const char* shader_path; ///< Path to shader file (NULL = don't change)
    const char* shader_entry_point; ///< Entry point function name (NULL = "main")

    // Named parameters (pipeline-agnostic)
    const pnanovdb_pipeline_param_t* params; ///< Array of named parameters (NULL = no params)
    pnanovdb_uint32_t param_count; ///< Number of parameters in params array

    // Reserved for future expansion
    pnanovdb_uint32_t flags; ///< Reserved flags (set to 0)
} pnanovdb_pipeline_settings_t;

struct pnanovdb_editor_impl_t;
typedef struct pnanovdb_editor_impl_t pnanovdb_editor_impl_t;
typedef struct pnanovdb_editor_t
{
    PNANOVDB_REFLECT_INTERFACE();

    void* module;
    struct pnanovdb_editor_impl_t* impl;

    void(PNANOVDB_ABI* init)(pnanovdb_editor_t* editor);
    pnanovdb_bool_t(PNANOVDB_ABI* init_impl)(pnanovdb_editor_t* editor,
                                             const pnanovdb_compute_t* compute,
                                             const pnanovdb_compiler_t* compiler);
    void(PNANOVDB_ABI* shutdown)(pnanovdb_editor_t* editor);
    void(PNANOVDB_ABI* show)(pnanovdb_editor_t* editor,
                             pnanovdb_compute_device_t* device,
                             pnanovdb_editor_config_t* config);
    void(PNANOVDB_ABI* start)(pnanovdb_editor_t* editor,
                              pnanovdb_compute_device_t* device,
                              pnanovdb_editor_config_t* config);
    void(PNANOVDB_ABI* stop)(pnanovdb_editor_t* editor);
    void(PNANOVDB_ABI* reset)(pnanovdb_editor_t* editor);
    void(PNANOVDB_ABI* wait_for_interrupt)(pnanovdb_editor_t* editor);
    void(PNANOVDB_ABI* add_nanovdb)(pnanovdb_editor_t* editor, pnanovdb_compute_array_t* array);
    void(PNANOVDB_ABI* add_array)(pnanovdb_editor_t* editor, pnanovdb_compute_array_t* array);
    void(PNANOVDB_ABI* add_gaussian_data)(pnanovdb_editor_t* editor,
                                          pnanovdb_raster_context_t* raster_ctx,
                                          pnanovdb_compute_queue_t* queue,
                                          pnanovdb_raster_gaussian_data_t* data);
    void(PNANOVDB_ABI* update_camera)(pnanovdb_editor_t* editor, pnanovdb_camera_t* camera);
    void(PNANOVDB_ABI* add_camera_view)(pnanovdb_editor_t* editor, pnanovdb_camera_view_t* camera);
    void(PNANOVDB_ABI* add_shader_params)(pnanovdb_editor_t* editor,
                                          void* params,
                                          const pnanovdb_reflect_data_type_t* data_type);
    void(PNANOVDB_ABI* sync_shader_params)(pnanovdb_editor_t* editor, void* shader_params, pnanovdb_bool_t set_data);
    pnanovdb_int32_t(PNANOVDB_ABI* get_resolved_port)(pnanovdb_editor_t* editor, pnanovdb_bool_t should_wait);

    // Token-based API for scene object management
    pnanovdb_camera_t*(PNANOVDB_ABI* get_camera)(pnanovdb_editor_t* editor, pnanovdb_editor_token_t* scene);
    pnanovdb_editor_token_t*(PNANOVDB_ABI* get_token)(const char* name);

    /*!
        \brief Add NanoVDB data to scene

        The editor duplicates the array internally, so caller can safely call
        destroy_array() on their copy after this returns.

        OPTIMIZATION: If the same array is added to multiple scenes, the editor
        detects this and shares ownership (no extra duplicate).

        Auto-creates default pipelines (Null conversion + Render).

        \param editor Editor instance
        \param scene Scene token
        \param name Object name token
        \param array NanoVDB array (will be duplicated internally)
    */
    void(PNANOVDB_ABI* add_nanovdb_2)(pnanovdb_editor_t* editor,
                                      pnanovdb_editor_token_t* scene,
                                      pnanovdb_editor_token_t* name,
                                      pnanovdb_compute_array_t* array);

    /*!
        \brief Add Gaussian splat data to scene (takes ownership)

        OWNERSHIP: This function takes ownership of the descriptor arrays.
        Caller must NOT call destroy on them after this.

        Auto-creates default pipelines (Raster3D conversion + Render).

        \param editor Editor instance
        \param scene Scene token
        \param name Object name token
        \param desc Gaussian data descriptor (ownership of arrays transferred)
    */
    void(PNANOVDB_ABI* add_gaussian_data_2)(pnanovdb_editor_t* editor,
                                            pnanovdb_editor_token_t* scene,
                                            pnanovdb_editor_token_t* name,
                                            const pnanovdb_editor_gaussian_data_desc_t* desc);
    void(PNANOVDB_ABI* add_camera_view_2)(pnanovdb_editor_t* editor,
                                          pnanovdb_editor_token_t* scene,
                                          pnanovdb_camera_view_t* camera);
    void(PNANOVDB_ABI* update_camera_2)(pnanovdb_editor_t* editor,
                                        pnanovdb_editor_token_t* scene,
                                        pnanovdb_camera_t* camera);

    void(PNANOVDB_ABI* remove)(pnanovdb_editor_t* editor, pnanovdb_editor_token_t* scene, pnanovdb_editor_token_t* name);

    // For any scene object, client can attempt to map parameters of a given type for read/write
    // It is the server's job to deal with binary layout compatbility, converting to client layout as needed
    void*(PNANOVDB_ABI* map_params)(pnanovdb_editor_t* editor,
                                    pnanovdb_editor_token_t* scene,
                                    pnanovdb_editor_token_t* name,
                                    const pnanovdb_reflect_data_type_t* data_type);

    // unmap allows us to flush any writes from the client to the server
    void(PNANOVDB_ABI* unmap_params)(pnanovdb_editor_t* editor,
                                     pnanovdb_editor_token_t* scene,
                                     pnanovdb_editor_token_t* name);

    // ========================================================================
    // Pipeline API
    // ========================================================================
    // Note: Pipelines are auto-created when data is added (add_nanovdb_2, add_gaussian_data_2).

    /*!
        \brief Get output NanoVDB from a scene object

        After a conversion pipeline runs (e.g. Raster3D), this retrieves
        the output NanoVDB. For Null pipeline (pass-through), returns the
        input NanoVDB directly (no copy, using ref counting).

        \param editor Editor instance
        \param scene Scene token
        \param name Object name token
        \return Output NanoVDB array or NULL
    */
    pnanovdb_compute_array_t*(PNANOVDB_ABI* get_output)(pnanovdb_editor_t* editor,
                                                        pnanovdb_editor_token_t* scene,
                                                        pnanovdb_editor_token_t* name);

    // ========================================================================
    // Named Component API
    // ========================================================================

    /*!
        \brief Add a named array component to a scene object

        Named arrays are generic arrays that require a name for identification.
        Multiple named arrays can exist per scene object.
        Pipelines can reference these arrays by name in their parameters.

        \param editor Editor instance
        \param scene Scene token
        \param object Object name token
        \param array_name Name for this array component
        \param array Array data
        \param description Optional description (can be NULL)
    */
    void(PNANOVDB_ABI* add_named_array)(pnanovdb_editor_t* editor,
                                        pnanovdb_editor_token_t* scene,
                                        pnanovdb_editor_token_t* object,
                                        pnanovdb_editor_token_t* array_name,
                                        pnanovdb_compute_array_t* array,
                                        const char* description);

    /*!
        \brief Get a named array component

        \param editor Editor instance
        \param scene Scene token
        \param object Object name token
        \param array_name Name of the array
        \return Pointer to array or NULL if not found
    */
    pnanovdb_compute_array_t*(PNANOVDB_ABI* get_named_array)(pnanovdb_editor_t* editor,
                                                             pnanovdb_editor_token_t* scene,
                                                             pnanovdb_editor_token_t* object,
                                                             pnanovdb_editor_token_t* array_name);

    /*!
        \brief Remove a named array component

        \param editor Editor instance
        \param scene Scene token
        \param object Object name token
        \param array_name Name of the array
        \return PNANOVDB_TRUE if found and removed, PNANOVDB_FALSE otherwise
    */
    pnanovdb_bool_t(PNANOVDB_ABI* remove_named_array)(pnanovdb_editor_t* editor,
                                                      pnanovdb_editor_token_t* scene,
                                                      pnanovdb_editor_token_t* object,
                                                      pnanovdb_editor_token_t* array_name);
} pnanovdb_editor_t;

#define PNANOVDB_REFLECT_TYPE pnanovdb_editor_t
PNANOVDB_REFLECT_BEGIN()
PNANOVDB_REFLECT_VOID_POINTER(module, 0, 0)
PNANOVDB_REFLECT_VOID_POINTER(impl, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(init, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(init_impl, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(shutdown, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(show, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(start, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(stop, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(reset, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(wait_for_interrupt, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(add_nanovdb, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(add_array, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(add_gaussian_data, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(update_camera, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(add_camera_view, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(add_shader_params, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(sync_shader_params, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(get_resolved_port, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(get_camera, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(get_token, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(add_nanovdb_2, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(add_gaussian_data_2, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(add_camera_view_2, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(update_camera_2, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(remove, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(map_params, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(unmap_params, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(get_output, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(add_named_array, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(get_named_array, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(remove_named_array, 0, 0)
PNANOVDB_REFLECT_END(0)
PNANOVDB_REFLECT_INTERFACE_IMPL()
#undef PNANOVDB_REFLECT_TYPE

typedef pnanovdb_editor_t*(PNANOVDB_ABI* PFN_pnanovdb_get_editor)();

PNANOVDB_API pnanovdb_editor_t* pnanovdb_get_editor();

static inline void pnanovdb_editor_load(pnanovdb_editor_t* editor,
                                        const pnanovdb_compute_t* compute,
                                        const pnanovdb_compiler_t* compiler)
{
    void* editor_module = pnanovdb_load_library("pnanovdbeditor.dll", "libpnanovdbeditor.so", "libpnanovdbeditor.dylib");
    if (!editor_module)
    {
#if defined(_WIN32)
        printf("Error: Editor module failed to load\n");
#else
        printf("Error: Editor module failed to load: %s\n", dlerror());
#endif
        return;
    }

    PFN_pnanovdb_get_editor get_editor =
        (PFN_pnanovdb_get_editor)pnanovdb_get_proc_address(editor_module, "pnanovdb_get_editor");
    if (!get_editor)
    {
        printf("Error: Failed to acquire editor getter\n");
        return;
    }

    pnanovdb_editor_t_duplicate(editor, get_editor());
    if (!editor)
    {
        printf("Error: Failed to acquire editor\n");
        return;
    }

    editor->module = editor_module;

    if (editor->init_impl(editor, compute, compiler))
    {
        editor->init(editor);
    }
}

static inline void pnanovdb_editor_free(pnanovdb_editor_t* editor)
{
    if (!editor || !editor->impl)
    {
        return;
    }

    editor->shutdown(editor);

    pnanovdb_free_library(editor->module);
}

#endif
