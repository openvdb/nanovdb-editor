// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/putil/Editor.h

    \author Andrew Reidmeyer

    \brief  This file provides a GPU compute abstraction.
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
    int port;
    pnanovdb_bool_t headless;
    pnanovdb_bool_t streaming;
    pnanovdb_bool_t stream_to_file;
} pnanovdb_editor_config_t;

typedef struct pnanovdb_editor_t
{
    PNANOVDB_REFLECT_INTERFACE();

    const pnanovdb_compiler_t* compiler;
    const pnanovdb_compute_t* compute;

    void(PNANOVDB_ABI* init)(pnanovdb_editor_t* editor);

    void(PNANOVDB_ABI* shutdown)(pnanovdb_editor_t* editor);

    void(PNANOVDB_ABI* show)(pnanovdb_editor_t* editor,
                             pnanovdb_compute_device_t* device,
                             pnanovdb_editor_config_t* config);

    void(PNANOVDB_ABI* start)(pnanovdb_editor_t* editor,
                              pnanovdb_compute_device_t* device,
                              pnanovdb_editor_config_t* config);

    void(PNANOVDB_ABI* stop)(pnanovdb_editor_t* editor);

    void(PNANOVDB_ABI* add_nanovdb)(pnanovdb_editor_t* editor, pnanovdb_compute_array_t* array);

    void(PNANOVDB_ABI* add_array)(pnanovdb_editor_t* editor, pnanovdb_compute_array_t* array);

    void(PNANOVDB_ABI* add_gaussian_data)(pnanovdb_editor_t* editor,
                                          pnanovdb_raster_t* raster,
                                          pnanovdb_compute_queue_t* queue,
                                          pnanovdb_raster_gaussian_data_t* data);

    void(PNANOVDB_ABI* add_camera)(pnanovdb_editor_t* editor, pnanovdb_camera_t* camera);

    void(PNANOVDB_ABI* add_camera_view)(pnanovdb_editor_t* editor, pnanovdb_camera_view_t* camera);

    void(PNANOVDB_ABI* add_shader_params)(pnanovdb_editor_t* editor,
                                          void* params,
                                          const pnanovdb_reflect_data_type_t* data_type);

    void(PNANOVDB_ABI* sync_shader_params)(pnanovdb_editor_t* editor,
                                           const pnanovdb_reflect_data_type_t* data_type,
                                           pnanovdb_bool_t set_data);

    void* module;
    void* editor_worker;
    pnanovdb_compute_array_t* nanovdb_array;
    pnanovdb_compute_array_t* data_array;
    pnanovdb_raster_gaussian_data_t* gaussian_data;
    pnanovdb_camera_t* camera;
    pnanovdb_raster_context_t* raster_ctx;
    void* shader_params;
    const pnanovdb_reflect_data_type_t* shader_params_data_type;
    void* views;


} pnanovdb_editor_t;

#define PNANOVDB_REFLECT_TYPE pnanovdb_editor_t
PNANOVDB_REFLECT_BEGIN()
PNANOVDB_REFLECT_POINTER(pnanovdb_compiler_t, compiler, 0, 0)
PNANOVDB_REFLECT_POINTER(pnanovdb_compute_t, compute, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(init, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(shutdown, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(show, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(start, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(stop, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(add_nanovdb, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(add_array, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(add_gaussian_data, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(add_camera, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(add_camera_view, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(add_shader_params, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(sync_shader_params, 0, 0)
PNANOVDB_REFLECT_VOID_POINTER(module, 0, 0)
PNANOVDB_REFLECT_VOID_POINTER(editor_worker, 0, 0)
PNANOVDB_REFLECT_POINTER(pnanovdb_compute_array_t, nanovdb_array, 0, 0)
PNANOVDB_REFLECT_POINTER(pnanovdb_compute_array_t, data_array, 0, 0)
PNANOVDB_REFLECT_VOID_POINTER(shader_params, 0, 0)
PNANOVDB_REFLECT_POINTER(pnanovdb_reflect_data_type_t, shader_params_data_type, 0, 0)
PNANOVDB_REFLECT_VOID_POINTER(views, 0, 0)
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
    editor->compute = compute;
    editor->compiler = compiler;
    editor->editor_worker = NULL;
    editor->gaussian_data = NULL;
    editor->nanovdb_array = NULL;
    editor->data_array = NULL;
    editor->camera = NULL;
    editor->raster_ctx = NULL;
    editor->shader_params = NULL;
    editor->shader_params_data_type = NULL;
    editor->views = NULL;
    editor->init(editor);
}

static inline void pnanovdb_editor_free(pnanovdb_editor_t* editor)
{
    if (!editor)
    {
        return;
    }
    editor->shutdown(editor);

    pnanovdb_free_library(editor->module);
}

#endif
