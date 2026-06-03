// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   editor/shaders/voxelbvh_common.h

    \author Andrew Reidmeyer

    \brief
*/

#ifndef VOXELBVH_COMMON_SLANG
#define VOXELBVH_COMMON_SLANG

PNANOVDB_FORCE_INLINE pnanovdb_uint32_t voxelbvh_get_active_level_mask(pnanovdb_uint64_t voxel_val_raw)
{
    return pnanovdb_uint32_t(voxel_val_raw) & 0x0FFF;
}

PNANOVDB_FORCE_INLINE pnanovdb_uint32_t voxelbvh_get_skip_level(pnanovdb_uint64_t voxel_val_raw)
{
    return (pnanovdb_uint32_t(voxel_val_raw) >> 12u) & 0x000F;
}

PNANOVDB_FORCE_INLINE pnanovdb_uint64_t voxelbvh_get_first_range_idx(pnanovdb_uint64_t voxel_val_raw)
{
    return voxel_val_raw >> 16u;
}

PNANOVDB_FORCE_INLINE pnanovdb_uint64_t voxelbvh_make_voxel_val(pnanovdb_uint32_t active_level_mask,
                                                                pnanovdb_uint32_t skip_level,
                                                                pnanovdb_uint64_t first_range_idx)
{
    active_level_mask = active_level_mask & 0x0FFF;
    skip_level = skip_level & 0x000F;
    return (first_range_idx << 16u) | pnanovdb_uint64_t((skip_level << 12u) | active_level_mask);
}

#endif
