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

// ------------------------------------------------ Pipeline Types

enum pnanovdb_pipeline_stage_t
{
    pnanovdb_pipeline_stage_load = 0,
    pnanovdb_pipeline_stage_process = 1,
    pnanovdb_pipeline_stage_render = 2,
    pnanovdb_pipeline_stage_count
};

typedef pnanovdb_uint32_t pnanovdb_pipeline_type_t;

// ------------------------------------------------ Pipeline Parameters

typedef struct pnanovdb_pipeline_params_t
{
    void* data;
    pnanovdb_uint64_t size;
    const pnanovdb_reflect_data_type_t* type;
} pnanovdb_pipeline_params_t;

// ------------------------------------------------ Pipeline Descriptors

typedef struct pnanovdb_pipeline_shader_entry_t
{
    const char* shader_name;
    const char* shader_group;
    pnanovdb_bool_t overridable;
} pnanovdb_pipeline_shader_entry_t;

typedef struct pnanovdb_scene_object_t pnanovdb_scene_object_t;

typedef enum pnanovdb_pipeline_result_t
{
    pnanovdb_pipeline_result_success = 0,
    pnanovdb_pipeline_result_skipped = 1,
    pnanovdb_pipeline_result_no_data = 2,
    pnanovdb_pipeline_result_error = 3,
    pnanovdb_pipeline_result_pending = 4
} pnanovdb_pipeline_result_t;

typedef enum pnanovdb_pipeline_render_method_t
{
    pnanovdb_pipeline_render_method_none = 0,
    pnanovdb_pipeline_render_method_nanovdb = 1,
    pnanovdb_pipeline_render_method_raster2d = 2
} pnanovdb_pipeline_render_method_t;

typedef struct pnanovdb_pipeline_context_t pnanovdb_pipeline_context_t;

typedef void (*pnanovdb_pipeline_init_params_fn)(pnanovdb_pipeline_params_t* params);
typedef pnanovdb_pipeline_result_t (*pnanovdb_pipeline_execute_fn)(pnanovdb_scene_object_t* obj,
                                                                   pnanovdb_pipeline_context_t* ctx);
typedef pnanovdb_pipeline_render_method_t (*pnanovdb_pipeline_get_render_method_fn)(void);
typedef void* (*pnanovdb_pipeline_map_params_fn)(pnanovdb_scene_object_t* obj);

typedef struct pnanovdb_pipeline_param_field_t pnanovdb_pipeline_param_field_t;

typedef struct pnanovdb_pipeline_descriptor_t
{
    pnanovdb_pipeline_type_t type;
    pnanovdb_pipeline_stage_t stage;
    const char* name;

    const pnanovdb_pipeline_shader_entry_t* shaders;
    pnanovdb_uint32_t shader_count;

    pnanovdb_uint64_t params_size;
    const char* params_type_name;

    pnanovdb_pipeline_init_params_fn init_params;
    pnanovdb_pipeline_execute_fn execute;
    pnanovdb_pipeline_get_render_method_fn get_render_method;
    pnanovdb_pipeline_map_params_fn map_params;

    const pnanovdb_pipeline_param_field_t* param_fields;
    pnanovdb_uint32_t param_field_count;
} pnanovdb_pipeline_descriptor_t;

struct pnanovdb_shader_params_desc_t
{
    void* data;
    pnanovdb_uint64_t data_size;

    const char* shader_name;

    const char** element_names;
    const char** element_typenames;
    pnanovdb_uint64_t* element_offsets;
    pnanovdb_uint64_t element_count;
};
typedef struct pnanovdb_shader_params_desc_t pnanovdb_shader_params_desc_t;

// ------------------------------------------------ Shader Parameter Provider Callback Interface

typedef struct pnanovdb_shader_param_provider_ctx_t pnanovdb_shader_param_provider_ctx_t;

typedef struct pnanovdb_shader_param_value_t
{
    const void* data;
    pnanovdb_uint64_t size;
    pnanovdb_uint64_t element_count;
    pnanovdb_uint32_t type; // ImGuiDataType-compatible type
} pnanovdb_shader_param_value_t;

// Provider callback function type - called to retrieve a parameter value by name
typedef pnanovdb_bool_t (*pnanovdb_shader_param_provider_fn)(pnanovdb_shader_param_provider_ctx_t* ctx,
                                                             const char* shader_name,
                                                             const char* param_name,
                                                             pnanovdb_shader_param_value_t* out_value);

// Provider descriptor - can be registered to provide shader parameters
typedef struct pnanovdb_shader_param_provider_t
{
    pnanovdb_shader_param_provider_ctx_t* ctx; // User context passed to callback
    pnanovdb_shader_param_provider_fn get_value; // Get parameter value callback
} pnanovdb_shader_param_provider_t;

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
    void(PNANOVDB_ABI* add_nanovdb_2)(pnanovdb_editor_t* editor,
                                      pnanovdb_editor_token_t* scene,
                                      pnanovdb_editor_token_t* name,
                                      pnanovdb_compute_array_t* array);
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
    // Pass name=nullptr to map scene-level custom params using get_custom_scene_params_data_type()
    // It is the server's job to deal with binary layout compatbility, converting to client layout as needed
    void*(PNANOVDB_ABI* map_params)(pnanovdb_editor_t* editor,
                                    pnanovdb_editor_token_t* scene,
                                    pnanovdb_editor_token_t* name,
                                    const pnanovdb_reflect_data_type_t* data_type);

    // unmap allows us to flush any writes from the client to the server
    void(PNANOVDB_ABI* unmap_params)(pnanovdb_editor_t* editor,
                                     pnanovdb_editor_token_t* scene,
                                     pnanovdb_editor_token_t* name);

    void(PNANOVDB_ABI* set_pipeline)(pnanovdb_editor_t* editor,
                                     pnanovdb_editor_token_t* scene,
                                     pnanovdb_editor_token_t* name,
                                     pnanovdb_pipeline_stage_t stage,
                                     pnanovdb_pipeline_type_t type);

    pnanovdb_pipeline_type_t(PNANOVDB_ABI* get_pipeline)(pnanovdb_editor_t* editor,
                                                         pnanovdb_editor_token_t* scene,
                                                         pnanovdb_editor_token_t* name,
                                                         pnanovdb_pipeline_stage_t stage);

    void(PNANOVDB_ABI* mark_pipeline_dirty)(pnanovdb_editor_t* editor,
                                            pnanovdb_editor_token_t* scene,
                                            pnanovdb_editor_token_t* name);

    void(PNANOVDB_ABI* add_nanovdb_3)(pnanovdb_editor_t* editor,
                                      pnanovdb_editor_token_t* scene,
                                      pnanovdb_editor_token_t* name,
                                      pnanovdb_compute_array_t* array,
                                      pnanovdb_pipeline_type_t process_pipeline,
                                      pnanovdb_pipeline_type_t render_pipeline);
    void(PNANOVDB_ABI* add_gaussian_data_3)(pnanovdb_editor_t* editor,
                                            pnanovdb_editor_token_t* scene,
                                            pnanovdb_editor_token_t* name,
                                            const pnanovdb_editor_gaussian_data_desc_t* desc,
                                            pnanovdb_pipeline_type_t process_pipeline,
                                            pnanovdb_pipeline_type_t render_pipeline);

    void(PNANOVDB_ABI* set_visible)(pnanovdb_editor_t* editor,
                                    pnanovdb_editor_token_t* scene,
                                    pnanovdb_editor_token_t* name,
                                    pnanovdb_bool_t visible);
    pnanovdb_bool_t(PNANOVDB_ABI* get_visible)(pnanovdb_editor_t* editor,
                                               pnanovdb_editor_token_t* scene,
                                               pnanovdb_editor_token_t* name);

    // Named arrays - multiple arrays per scene object, identified by name
    void(PNANOVDB_ABI* add_named_array)(pnanovdb_editor_t* editor,
                                        pnanovdb_editor_token_t* scene,
                                        pnanovdb_editor_token_t* object_name,
                                        const char* array_name,
                                        pnanovdb_compute_array_t* array);
    pnanovdb_compute_array_t*(PNANOVDB_ABI* get_named_array)(pnanovdb_editor_t* editor,
                                                             pnanovdb_editor_token_t* scene,
                                                             pnanovdb_editor_token_t* object_name,
                                                             const char* array_name);

    // Per-stage pipeline parameters. Call unmap_pipeline_params after every map_pipeline_params (even if map returned
    // null).
    pnanovdb_pipeline_params_t*(PNANOVDB_ABI* map_pipeline_params)(pnanovdb_editor_t* editor,
                                                                   pnanovdb_editor_token_t* scene,
                                                                   pnanovdb_editor_token_t* name,
                                                                   pnanovdb_pipeline_stage_t stage);

    // Releases lock and marks stage dirty. Must be called after every map_pipeline_params.
    void(PNANOVDB_ABI* unmap_pipeline_params)(pnanovdb_editor_t* editor,
                                              pnanovdb_editor_token_t* scene,
                                              pnanovdb_editor_token_t* name,
                                              pnanovdb_pipeline_stage_t stage);

    // Load scene-level UI params from a JSON payload carried in `json->str`.
    // Returns PNANOVDB_TRUE on success; on failure writes a null-terminated error
    // into error_buf (pass NULL / 0 to ignore).
    pnanovdb_bool_t(PNANOVDB_ABI* set_custom_scene_params)(pnanovdb_editor_t* editor,
                                                           pnanovdb_editor_token_t* scene,
                                                           pnanovdb_editor_token_t* json,
                                                           char* error_buf,
                                                           pnanovdb_uint64_t error_buf_size);
    const pnanovdb_reflect_data_type_t*(PNANOVDB_ABI* get_custom_scene_params_data_type)(pnanovdb_editor_t* editor,
                                                                                         pnanovdb_editor_token_t* scene);
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
PNANOVDB_REFLECT_FUNCTION_POINTER(set_pipeline, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(get_pipeline, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(mark_pipeline_dirty, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(add_nanovdb_3, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(add_gaussian_data_3, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(set_visible, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(get_visible, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(add_named_array, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(get_named_array, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(map_pipeline_params, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(unmap_pipeline_params, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(set_custom_scene_params, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(get_custom_scene_params_data_type, 0, 0)
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
