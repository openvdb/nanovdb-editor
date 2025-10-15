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
}

namespace pnanovdb_editor
{
class CameraFrustum
{
public:
    static CameraFrustum& getInstance()
    {
        static CameraFrustum instance;
        return instance;
    }

    void render(imgui_instance_user::Instance* ptr);

private:
    CameraFrustum() = default;
    ~CameraFrustum() = default;

    CameraFrustum(const CameraFrustum&) = delete;
    CameraFrustum& operator=(const CameraFrustum&) = delete;
    CameraFrustum(CameraFrustum&&) = delete;
    CameraFrustum& operator=(CameraFrustum&&) = delete;
};
}
