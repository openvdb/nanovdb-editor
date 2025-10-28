
// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   ImguiWindowGlfw.h

    \author Andrew Reidmeyer

    \brief  This file is part of the PNanoVDB Compute Vulkan reference implementation.
*/

#pragma once

#include <imgui.h>

#include "ImguiRenderer.h"
#include "ImguiWindow.h"

#include "nanovdb_editor/putil/Camera.h"

namespace pnanovdb_imgui_window_default
{
// Generic Window
struct Window;
void keyboardWindow(Window* ptr, ImGuiKey key, int scanCode, bool is_pressed, bool alt, bool control, bool shift, bool super);
void charInputWindow(Window* ptr, uint32_t input);
void mouseMoveWindow(Window* ptr, double mouseX, double mouseY);
void mouseButtonWindow(Window* ptr, int button, bool is_pressed, int modifiers);
void mouseWheelWindow(Window* ptr, double scrollX, double scrollY);
void resizeWindow(Window* ptr, uint32_t width, uint32_t height);

// GLFW Window
struct WindowGlfw;
WindowGlfw* createWindowGlfw(Window* window_parent,
                             pnanovdb_compute_log_print_t log_print,
                             pnanovdb_uint32_t width,
                             pnanovdb_uint32_t height);
void destroyWindowGlfw(WindowGlfw* ptr);
void windowGlfwCreateSwapchain(WindowGlfw* ptr,
                               pnanovdb_compute_queue_t* queue,
                               pnanovdb_compute_device_interface_t* device_interface);
void windowGlfwPollEvents(WindowGlfw* ptr);
pnanovdb_bool_t windowGlfwShouldClose(WindowGlfw* ptr);
void windowGlfwResize(WindowGlfw* ptr, pnanovdb_uint32_t width, pnanovdb_uint32_t height);
float windowGlfwGetScale(WindowGlfw* ptr);
pnanovdb_compute_swapchain_t* windowGlfwGetSwapchain(WindowGlfw* ptr);

// Clipboard functions
void windowGlfwSetClipboard(WindowGlfw* ptr, const char* text);
const char* windowGlfwGetClipboard(WindowGlfw* ptr);
}
