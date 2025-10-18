
// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   ImguiTLS.h

    \author Andrew Reidmeyer

    \brief  Shared Imgui header to configure imgui TLS.
*/

#ifndef NANOVDB_IMGUI_TLS_H_HAS_BEEN_INCLUDED
#define NANOVDB_IMGUI_TLS_H_HAS_BEEN_INCLUDED

struct ImGuiContext;
extern thread_local ImGuiContext* ImGuiTLS;
#define GImGui ImGuiTLS

#endif