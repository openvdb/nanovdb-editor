// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include "editor/PipelineRuntime.h"
#include "nanovdb_editor/putil/Editor.h"
#include "nanovdb_editor/putil/VoxelBVH.h"

#include <cstddef>

namespace
{

struct PreviousEditorConfig
{
    const char* ip_address;
    pnanovdb_int32_t port;
    pnanovdb_bool_t headless;
    pnanovdb_bool_t streaming;
    pnanovdb_bool_t stream_to_file;
    const char* ui_profile_name;
};

static_assert(offsetof(pnanovdb_editor_config_t, ui_profile_name) == offsetof(PreviousEditorConfig, ui_profile_name));
static_assert(sizeof(pnanovdb_editor_config_t) == sizeof(PreviousEditorConfig));

void PNANOVDB_ABI old_destroy_context(const pnanovdb_compute_t*, pnanovdb_compute_queue_t*, pnanovdb_voxelbvh_context_t*)
{
}

void PNANOVDB_ABI unavailable_set_cancel(pnanovdb_voxelbvh_context_t*, pnanovdb_voxelbvh_cancel_t, void*)
{
}

void PNANOVDB_ABI unavailable_set_progress(pnanovdb_voxelbvh_context_t*, pnanovdb_voxelbvh_progress_t, void*)
{
}

TEST(AbiCompatibility, VoxelBvhDuplicateClearsMembersMissingFromOlderInterface)
{
    pnanovdb_reflect_data_type_t old_type = *PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_voxelbvh_t);
    old_type.element_size = offsetof(pnanovdb_voxelbvh_t, context_set_cancel);
    old_type.child_reflect_data_count -= 2u;

    pnanovdb_voxelbvh_t old_interface = {};
    old_interface.interface_pnanovdb_reflect_data_type = &old_type;
    old_interface.destroy_context = old_destroy_context;

    pnanovdb_voxelbvh_t loaded = {};
    loaded.context_set_cancel = unavailable_set_cancel;
    loaded.context_set_progress = unavailable_set_progress;

    pnanovdb_voxelbvh_t_duplicate(&loaded, &old_interface);

    EXPECT_EQ(loaded.destroy_context, old_destroy_context);
    EXPECT_EQ(loaded.context_set_cancel, nullptr);
    EXPECT_EQ(loaded.context_set_progress, nullptr);
    EXPECT_FALSE(pnanovdb_editor::voxelbvh_interface_supports_user_cancel(&loaded));

    loaded.context_set_cancel = unavailable_set_cancel;
    EXPECT_TRUE(pnanovdb_editor::voxelbvh_interface_supports_user_cancel(&loaded));
}

} // namespace
