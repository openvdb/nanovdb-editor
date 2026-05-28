// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   gtests/EditorTestSupport.h

    \brief  Gtest-local helpers that reach into the editor's scene manager to
            install / observe per-object shader parameter buffers.

            These used to live in editor/Editor.cpp as PNANOVDB_API-exported
            test hooks. They were pulled into gtests/ because they only touch
            state that is already reachable through editor->impl->scene_manager
            (a public field on pnanovdb_editor_impl_t) and therefore do not
            need to be exported from the pnanovdbeditor shared library.

            Helpers that still *must* live inside the library because they
            observe anonymous-namespace state (map ref-counts, map-call stack
            depth) stayed in editor/EditorParamMapRegistry.{h,cpp}; include
            that header directly to call them.
*/

#ifndef NANOVDB_EDITOR_GTESTS_EDITOR_TEST_SUPPORT_H_HAS_BEEN_INCLUDED
#define NANOVDB_EDITOR_GTESTS_EDITOR_TEST_SUPPORT_H_HAS_BEEN_INCLUDED

#include <nanovdb_editor/putil/Editor.h>
#include <nanovdb_editor/putil/Reflect.h>

#include <stddef.h>

namespace pnanovdb_editor_test
{

pnanovdb_bool_t set_object_shader_params(pnanovdb_editor_t* editor,
                                         pnanovdb_editor_token_t* scene,
                                         pnanovdb_editor_token_t* name,
                                         void* shader_params,
                                         const pnanovdb_reflect_data_type_t* data_type);

size_t snapshot_object_shader_params(pnanovdb_editor_t* editor,
                                     pnanovdb_editor_token_t* scene,
                                     pnanovdb_editor_token_t* name,
                                     void* out_buf,
                                     size_t out_buf_size);

void* get_object_shader_params_ptr(pnanovdb_editor_t* editor,
                                   pnanovdb_editor_token_t* scene,
                                   pnanovdb_editor_token_t* name);

} // namespace pnanovdb_editor_test

#endif
