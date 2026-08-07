// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/putil/Pipeline.h

    \author Andrew Reidmeyer

    \brief  This file provides pipeline interface.
*/

#ifndef NANOVDB_PUTILS_PIPELINE_H_HAS_BEEN_INCLUDED
#define NANOVDB_PUTILS_PIPELINE_H_HAS_BEEN_INCLUDED

#include "nanovdb_editor/putil/Compute.h"
#include "nanovdb_editor/putil/Reflect.h"
#include <nanovdb_editor/putil/Camera.h>

// ------------------------------------------------ Pipeline -----------------------------------------------------------

// This must align with Editor.h
#ifndef PNANOVDB_PIPELINE_ENUM_DEFINED
#    define PNANOVDB_PIPELINE_ENUM_DEFINED
enum pnanovdb_pipeline_stage_t
{
    pnanovdb_pipeline_stage_load = 0,
    pnanovdb_pipeline_stage_process = 1,
    pnanovdb_pipeline_stage_render = 2,
    pnanovdb_pipeline_stage_count
};
typedef pnanovdb_uint32_t pnanovdb_pipeline_type_t;
#endif

#define PNANOVDB_REFLECT_TYPE pnanovdb_compute_token_t
PNANOVDB_REFLECT_BEGIN()
PNANOVDB_REFLECT_VALUE(pnanovdb_uint64_t, id, 0, 0)
PNANOVDB_REFLECT_POINTER(char, str, 0, 0)
PNANOVDB_REFLECT_END(0)
#undef PNANOVDB_REFLECT_TYPE

struct pnanovdb_pipeline_load_input_t
{
    pnanovdb_uint64_t buffer_count;
    pnanovdb_compute_token_t** buffer_paths;
    pnanovdb_compute_array_t** buffer_arrays;
    pnanovdb_uint64_t* buffer_array_versions;
    pnanovdb_compute_token_t** buffer_names;

    const pnanovdb_reflect_data_type_t* params_data_type;
    const void* params_data;
};
typedef pnanovdb_pipeline_load_input_t pnanovdb_pipeline_load_input_t;

struct pnanovdb_pipeline_input_t
{
    pnanovdb_uint64_t buffer_count;
    pnanovdb_compute_buffer_t** buffer_buffers;
    pnanovdb_uint64_t* buffer_sizes;
    pnanovdb_uint64_t* buffer_versions;
    pnanovdb_compute_token_t** buffer_names;

    const pnanovdb_reflect_data_type_t* params_data_type;
    const void* params_data;
};
typedef pnanovdb_pipeline_input_t pnanovdb_pipeline_input_t;

struct pnanovdb_pipeline_output_t
{
    pnanovdb_uint64_t buffer_count;
    pnanovdb_compute_buffer_t** buffer_buffers;
    pnanovdb_uint64_t* buffer_sizes;
    pnanovdb_uint64_t* buffer_versions;
    pnanovdb_compute_token_t** buffer_names;
};
typedef pnanovdb_pipeline_output_t pnanovdb_pipeline_output_t;

struct pnanovdb_pipeline_render_target_t
{
    pnanovdb_compute_texture_t* color;
    pnanovdb_compute_texture_t* depth;
    pnanovdb_uint32_t width;
    pnanovdb_uint32_t height;

    pnanovdb_camera_mat_t view;
    pnanovdb_camera_mat_t projection;
    pnanovdb_camera_mat_t view_inv;
    pnanovdb_camera_mat_t projection_inv;
};
typedef pnanovdb_pipeline_render_target_t pnanovdb_pipeline_render_target_t;

struct pnanovdb_pipeline_registry_t;
typedef pnanovdb_pipeline_registry_t pnanovdb_pipeline_registry_t;

struct pnanovdb_pipeline_instance_t;
typedef pnanovdb_pipeline_instance_t pnanovdb_pipeline_instance_t;

struct pnanovdb_pipeline_requirements_params_t
{
    const pnanovdb_reflect_data_type_t* params_data_type;
    const void* params_data;
};
typedef pnanovdb_pipeline_requirements_params_t pnanovdb_pipeline_requirements_params_t;

struct pnanovdb_pipeline_requirements_output_t
{
    pnanovdb_uint64_t* p_name_count;
    const char** names;
};
typedef pnanovdb_pipeline_requirements_output_t pnanovdb_pipeline_requirements_output_t;

struct pnanovdb_pipeline_desc_t
{
    pnanovdb_compute_token_t* name;

    void(PNANOVDB_ABI* get_pipeline_params_type)(const pnanovdb_compute_t* compute,
                                                 const pnanovdb_compiler_t* compiler,
                                                 const pnanovdb_reflect_data_type_t** out_data_type,
                                                 const pnanovdb_reflect_data_type_t** out_metadata_type);

    pnanovdb_pipeline_instance_t*(PNANOVDB_ABI* create)(const pnanovdb_compute_t* compute,
                                                        pnanovdb_compute_queue_t* queue);

    pnanovdb_bool_t(PNANOVDB_ABI* get_requirements)(pnanovdb_pipeline_requirements_params_t* params,
                                                    pnanovdb_pipeline_requirements_output_t* output);

    void(PNANOVDB_ABI* destroy)(const pnanovdb_compute_t* compute,
                                pnanovdb_compute_queue_t* queue,
                                pnanovdb_pipeline_instance_t* instance);

    void(PNANOVDB_ABI* load)(pnanovdb_pipeline_instance_t* instance,
                             const pnanovdb_compute_t* compute,
                             pnanovdb_compute_queue_t* queue,
                             pnanovdb_pipeline_load_input_t* input,
                             pnanovdb_pipeline_output_t* output);

    void(PNANOVDB_ABI* process)(pnanovdb_pipeline_instance_t* instance,
                                const pnanovdb_compute_t* compute,
                                pnanovdb_compute_queue_t* queue,
                                pnanovdb_pipeline_input_t* input,
                                pnanovdb_pipeline_output_t* output);

    void(PNANOVDB_ABI* render)(pnanovdb_pipeline_instance_t* instance,
                               const pnanovdb_compute_t* compute,
                               pnanovdb_compute_queue_t* queue,
                               pnanovdb_pipeline_input_t* input,
                               pnanovdb_pipeline_render_target_t* target);
};
typedef pnanovdb_pipeline_desc_t pnanovdb_pipeline_desc_t;

struct pnanovdb_pipeline_metadesc_t
{
    pnanovdb_compute_token_t* name;
    pnanovdb_uint64_t stage_mask;
};
typedef pnanovdb_pipeline_metadesc_t pnanovdb_pipeline_metadesc_t;

struct pnanovdb_pipeline_config_t;
typedef pnanovdb_pipeline_config_t pnanovdb_pipeline_config_t;

struct pnanovdb_pipeline_runtime_t;
typedef pnanovdb_pipeline_runtime_t pnanovdb_pipeline_runtime_t;

struct pnanovdb_pipeline_runtime_target_t
{
    pnanovdb_compute_texture_t* color;
    pnanovdb_compute_texture_t* depth;
    pnanovdb_uint32_t width;
    pnanovdb_uint32_t height;
};
typedef pnanovdb_pipeline_runtime_target_t pnanovdb_pipeline_runtime_target_t;

struct pnanovdb_pipeline_scene_list_t
{
    pnanovdb_compute_token_t** scenes;
    pnanovdb_uint64_t scene_count;
};
typedef struct pnanovdb_pipeline_scene_list_t pnanovdb_pipeline_scene_list_t;

#define PNANOVDB_REFLECT_TYPE pnanovdb_pipeline_scene_list_t
PNANOVDB_REFLECT_BEGIN()
PNANOVDB_REFLECT_POINTER_ARRAY(pnanovdb_compute_token_t, scenes, scene_count, 0, 0)
PNANOVDB_REFLECT_END(0)
#undef PNANOVDB_REFLECT_TYPE

struct pnanovdb_pipeline_object_list_t
{
    pnanovdb_compute_token_t** objects;
    pnanovdb_uint64_t object_count;
};
typedef struct pnanovdb_pipeline_object_list_t pnanovdb_pipeline_object_list_t;

#define PNANOVDB_REFLECT_TYPE pnanovdb_pipeline_object_list_t
PNANOVDB_REFLECT_BEGIN()
PNANOVDB_REFLECT_POINTER_ARRAY(pnanovdb_compute_token_t, objects, object_count, 0, 0)
PNANOVDB_REFLECT_END(0)
#undef PNANOVDB_REFLECT_TYPE

struct pnanovdb_pipeline_object_mapped_pipeline_t
{
    pnanovdb_uint32_t pipeline_type;

    void* params_data;
    const pnanovdb_reflect_data_type_t* params_data_type;

    void* params_metadata;
    const pnanovdb_reflect_data_type_t* params_metadata_type;
};

#define PNANOVDB_REFLECT_TYPE pnanovdb_pipeline_object_mapped_pipeline_t
PNANOVDB_REFLECT_BEGIN()
PNANOVDB_REFLECT_VALUE(pnanovdb_uint32_t, pipeline_type, 0, 0)
PNANOVDB_REFLECT_VOID_POINTER(params_data, 0, 0)
PNANOVDB_REFLECT_POINTER(pnanovdb_reflect_data_type_t, params_data_type, 0, 0)
PNANOVDB_REFLECT_VOID_POINTER(params_metadata, 0, 0)
PNANOVDB_REFLECT_POINTER(pnanovdb_reflect_data_type_t, params_metadata_type, 0, 0)
PNANOVDB_REFLECT_END(0)
#undef PNANOVDB_REFLECT_TYPE

struct pnanovdb_pipeline_object_mapped_stage_t
{
    pnanovdb_pipeline_object_mapped_pipeline_t* pipelines;
    pnanovdb_uint64_t pipeline_count;
};

#define PNANOVDB_REFLECT_TYPE pnanovdb_pipeline_object_mapped_stage_t
PNANOVDB_REFLECT_BEGIN()
PNANOVDB_REFLECT_ARRAY(pnanovdb_pipeline_object_mapped_pipeline_t, pipelines, pipeline_count, 0, 0)
PNANOVDB_REFLECT_END(0)
#undef PNANOVDB_REFLECT_TYPE

struct pnanovdb_pipeline_object_mapped_t
{
    pnanovdb_compute_array_t** buffer_arrays;
    pnanovdb_uint64_t* buffer_array_versions;
    pnanovdb_compute_token_t** buffer_paths;
    pnanovdb_compute_token_t** buffer_names;
    pnanovdb_uint64_t buffer_count;

    pnanovdb_pipeline_object_mapped_stage_t* stages;
    pnanovdb_uint64_t stage_count;

    pnanovdb_camera_t* camera_default;
    pnanovdb_camera_t* camera;
    pnanovdb_camera_view_t* camera_view;
    pnanovdb_bool_t* is_visible;
};
typedef struct pnanovdb_pipeline_object_mapped_t pnanovdb_pipeline_object_mapped_t;

PNANOVDB_REFLECT_STRUCT_OPAQUE_IMPL(pnanovdb_camera_t)
PNANOVDB_REFLECT_STRUCT_OPAQUE_IMPL(pnanovdb_camera_view_t)

#define PNANOVDB_REFLECT_TYPE pnanovdb_pipeline_object_mapped_t
PNANOVDB_REFLECT_BEGIN()
PNANOVDB_REFLECT_POINTER_ARRAY(pnanovdb_compute_array_t, buffer_arrays, buffer_count, 0, 0)
PNANOVDB_REFLECT_ARRAY(pnanovdb_uint64_t, buffer_array_versions, buffer_count, 0, 0)
PNANOVDB_REFLECT_POINTER_ARRAY(pnanovdb_compute_token_t, buffer_paths, buffer_count, 0, 0)
PNANOVDB_REFLECT_POINTER_ARRAY(pnanovdb_compute_token_t, buffer_names, buffer_count, 0, 0)
PNANOVDB_REFLECT_ARRAY(pnanovdb_pipeline_object_mapped_stage_t, stages, stage_count, 0, 0)
PNANOVDB_REFLECT_POINTER(pnanovdb_camera_t, camera_default, 0, 0)
PNANOVDB_REFLECT_POINTER(pnanovdb_camera_t, camera, 0, 0)
PNANOVDB_REFLECT_POINTER(pnanovdb_camera_view_t, camera_view, 0, 0)
PNANOVDB_REFLECT_END(0)
#undef PNANOVDB_REFLECT_TYPE

struct pnanovdb_pipeline_shader_t
{
    const char* shader_path;

    void* params_data;
    const pnanovdb_reflect_data_type_t* params_data_type;
    void* params_metadata;
    const pnanovdb_reflect_data_type_t* params_metadata_type;
};
typedef struct pnanovdb_pipeline_shader_t pnanovdb_pipeline_shader_t;

#define PNANOVDB_REFLECT_TYPE pnanovdb_pipeline_shader_t
PNANOVDB_REFLECT_BEGIN()
PNANOVDB_REFLECT_POINTER(char, shader_path, 0, 0)
PNANOVDB_REFLECT_VOID_POINTER(params_data, 0, 0)
PNANOVDB_REFLECT_POINTER(pnanovdb_reflect_data_type_t, params_data_type, 0, 0)
PNANOVDB_REFLECT_VOID_POINTER(params_metadata, 0, 0)
PNANOVDB_REFLECT_POINTER(pnanovdb_reflect_data_type_t, params_metadata_type, 0, 0)
PNANOVDB_REFLECT_END(0)
#undef PNANOVDB_REFLECT_TYPE

typedef struct pnanovdb_pipeline_t
{
    PNANOVDB_REFLECT_INTERFACE();

    const pnanovdb_compute_t* compute;

    pnanovdb_pipeline_registry_t*(PNANOVDB_ABI* create_registry)(const pnanovdb_compute_t* compute,
                                                                 const pnanovdb_compiler_t* compiler);

    void(PNANOVDB_ABI* destroy_registry)(const pnanovdb_compute_t* compute,
                                         const pnanovdb_compiler_t* compiler,
                                         pnanovdb_pipeline_registry_t* registry);

    void(PNANOVDB_ABI* register_pipeline)(pnanovdb_pipeline_registry_t* registry, const pnanovdb_pipeline_desc_t* desc);

    pnanovdb_bool_t(PNANOVDB_ABI* get_pipeline_type)(pnanovdb_pipeline_registry_t* registry,
                                                     pnanovdb_compute_token_t* name,
                                                     pnanovdb_pipeline_type_t* out_type);

    pnanovdb_bool_t(PNANOVDB_ABI* get_pipeline_metadesc)(pnanovdb_pipeline_registry_t* registry,
                                                        pnanovdb_pipeline_type_t type,
                                                        pnanovdb_pipeline_metadesc_t* out_metadesc);

    pnanovdb_pipeline_config_t*(PNANOVDB_ABI* create_config)(pnanovdb_pipeline_registry_t* registry);

    void(PNANOVDB_ABI* destroy_config)(pnanovdb_pipeline_registry_t* registry, pnanovdb_pipeline_config_t* config);

    void(PNANOVDB_ABI* config_add)(pnanovdb_pipeline_config_t* config,
                                   pnanovdb_compute_token_t* scene,
                                   pnanovdb_compute_token_t* object);

    void(PNANOVDB_ABI* config_duplicate)(pnanovdb_pipeline_config_t* config,
                                         pnanovdb_compute_token_t* src_scene,
                                         pnanovdb_compute_token_t* src_object,
                                         pnanovdb_compute_token_t* dst_scene,
                                         pnanovdb_compute_token_t* dst_object);

    void(PNANOVDB_ABI* config_rename)(pnanovdb_pipeline_config_t* config,
                                      pnanovdb_compute_token_t* scene,
                                      pnanovdb_compute_token_t* old_name,
                                      pnanovdb_compute_token_t* new_name);

    void(PNANOVDB_ABI* config_remove)(pnanovdb_pipeline_config_t* config,
                                      pnanovdb_compute_token_t* scene,
                                      pnanovdb_compute_token_t* object);

    void(PNANOVDB_ABI* config_set_pipeline)(pnanovdb_pipeline_config_t* config,
                                            pnanovdb_compute_token_t* scene,
                                            pnanovdb_compute_token_t* object,
                                            pnanovdb_pipeline_stage_t stage,
                                            pnanovdb_uint32_t step_idx,
                                            pnanovdb_pipeline_type_t type);

    void(PNANOVDB_ABI* config_mark_pipeline_dirty)(pnanovdb_pipeline_config_t* config,
                                                   pnanovdb_compute_token_t* scene,
                                                   pnanovdb_compute_token_t* object);

    void(PNANOVDB_ABI* config_set_buffer_array)(pnanovdb_pipeline_config_t* config,
                                               pnanovdb_compute_token_t* scene,
                                               pnanovdb_compute_token_t* object,
                                               pnanovdb_compute_token_t* array_name,
                                               pnanovdb_compute_array_t* compute_array);

    void(PNANOVDB_ABI* config_set_buffer_path)(pnanovdb_pipeline_config_t* config,
                                              pnanovdb_compute_token_t* scene,
                                              pnanovdb_compute_token_t* object,
                                              pnanovdb_compute_token_t* array_name,
                                              pnanovdb_compute_token_t* path);

    void(PNANOVDB_ABI* config_set_camera)(pnanovdb_pipeline_config_t* config,
                                          pnanovdb_compute_token_t* scene,
                                          pnanovdb_compute_token_t* object,
                                          pnanovdb_camera_t* camera);

    void(PNANOVDB_ABI* config_set_camera_view)(pnanovdb_pipeline_config_t* config,
                                               pnanovdb_compute_token_t* scene,
                                               pnanovdb_compute_token_t* object,
                                               pnanovdb_camera_view_t* camera_view);

    pnanovdb_bool_t(PNANOVDB_ABI* config_map_params)(pnanovdb_pipeline_config_t* config,
                                                     pnanovdb_compute_token_t* scene,
                                                     pnanovdb_compute_token_t* object,
                                                     const pnanovdb_reflect_data_type_t* map_data_type,
                                                     void** mapped_data);

    void(PNANOVDB_ABI* config_unmap_params)(pnanovdb_pipeline_config_t* config,
                                            pnanovdb_compute_token_t* scene,
                                            pnanovdb_compute_token_t* object,
                                            const pnanovdb_reflect_data_type_t* map_data_type);

    void(PNANOVDB_ABI* config_set_active_camera)(pnanovdb_pipeline_config_t* config,
                                                 pnanovdb_compute_token_t* scene,
                                                 pnanovdb_compute_token_t* object);

    pnanovdb_compute_token_t*(PNANOVDB_ABI* config_get_active_camera)(pnanovdb_pipeline_config_t* config,
                                                                       pnanovdb_compute_token_t* scene);

    void(PNANOVDB_ABI* config_set_active_scene)(pnanovdb_pipeline_config_t* config, pnanovdb_compute_token_t* scene);

    pnanovdb_compute_token_t*(PNANOVDB_ABI* config_get_active_scene)(pnanovdb_pipeline_config_t* config);

    pnanovdb_pipeline_runtime_t*(PNANOVDB_ABI* create_runtime)(const pnanovdb_compute_t* compute,
                                                               pnanovdb_compute_queue_t* queue);

    void(PNANOVDB_ABI* destroy_runtime)(const pnanovdb_compute_t* compute,
                                        pnanovdb_compute_queue_t* queue,
                                        pnanovdb_pipeline_runtime_t* runtime);

    void(PNANOVDB_ABI* update_runtime_main)(const pnanovdb_compute_t* compute,
                                            pnanovdb_compute_queue_t* queue,
                                            pnanovdb_pipeline_runtime_t* runtime,
                                            pnanovdb_pipeline_config_t* config,
                                            pnanovdb_pipeline_runtime_target_t* target);

    void(PNANOVDB_ABI* update_runtime_background)(const pnanovdb_compute_t* compute,
                                                  pnanovdb_compute_queue_t* queue,
                                                  pnanovdb_pipeline_runtime_t* runtime,
                                                  pnanovdb_pipeline_config_t* config);

} pnanovdb_pipeline_t;

#define PNANOVDB_REFLECT_TYPE pnanovdb_pipeline_t
PNANOVDB_REFLECT_BEGIN()
PNANOVDB_REFLECT_FUNCTION_POINTER(create_registry, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(destroy_registry, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(register_pipeline, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(get_pipeline_type, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(get_pipeline_metadesc, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(create_config, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(destroy_config, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(config_add, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(config_duplicate, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(config_rename, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(config_remove, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(config_set_pipeline, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(config_mark_pipeline_dirty, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(config_set_buffer_array, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(config_set_buffer_path, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(config_set_camera, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(config_set_camera_view, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(config_map_params, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(config_unmap_params, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(config_set_active_camera, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(config_get_active_camera, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(config_set_active_scene, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(config_get_active_scene, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(create_runtime, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(destroy_runtime, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(update_runtime_main, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(update_runtime_background, 0, 0)
PNANOVDB_REFLECT_END(0)
PNANOVDB_REFLECT_INTERFACE_IMPL()
#undef PNANOVDB_REFLECT_TYPE

typedef pnanovdb_pipeline_t*(PNANOVDB_ABI* PFN_pnanovdb_get_pipeline)();

PNANOVDB_API pnanovdb_pipeline_t* pnanovdb_get_pipeline();

static inline void pnanovdb_pipeline_load(pnanovdb_pipeline_t* pipeline, const pnanovdb_compute_t* compute)
{
    auto get_pipeline = (PFN_pnanovdb_get_pipeline)pnanovdb_get_proc_address(compute->module, "pnanovdb_get_pipeline");
    if (!get_pipeline)
    {
        printf("Error: Failed to acquire pipeline\n");
        return;
    }
    *pipeline = *get_pipeline();

    pipeline->compute = compute;
}

static inline void pnanovdb_pipeline_free(pnanovdb_pipeline_t* pipeline)
{
    // NOP for now
}

#endif
