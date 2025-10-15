// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/editor/Viewport.h

    \author Petra Hapalova

    \brief
*/

#include <imgui/ImguiWindow.h>

namespace pnanovdb_editor
{
class Viewport
{
public:
    static Viewport& getInstance()
    {
        static Viewport instance;
        return instance;
    }

    void setup();
    void render(const char* title);

private:
    Viewport();
    ~Viewport() = default;

    Viewport(const Viewport&) = delete;
    Viewport& operator=(const Viewport&) = delete;
    Viewport(Viewport&&) = delete;
    Viewport& operator=(Viewport&&) = delete;
};
}
