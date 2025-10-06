// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/editor/CameraFrustum.h

    \author Petra Hapalova

    \brief  Frustum rendering functions for the viewport
*/

#pragma once

namespace imgui_instance_user
{
struct Instance;

void drawCameraFrustums(Instance* ptr);

} // namespace imgui_instance_user
