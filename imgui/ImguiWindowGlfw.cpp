// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   ImguiWindowGlfw.cpp

    \author Andrew Reidmeyer

    \brief  This file is part of the PNanoVDB Compute Vulkan reference implementation.
*/

#include <cstdlib>

#ifndef PNANOVDB_USE_GLFW

#    include "ImguiWindowGlfw.h"

namespace pnanovdb_imgui_window_default
{
struct WindowGlfw;
WindowGlfw* createWindowGlfw(Window* window_parent,
                             pnanovdb_compute_log_print_t log_print,
                             pnanovdb_uint32_t width,
                             pnanovdb_uint32_t height)
{
    return nullptr;
}
void destroyWindowGlfw(WindowGlfw* ptr)
{
    // NOP
}
void windowGlfwCreateSwapchain(WindowGlfw* ptr,
                               pnanovdb_compute_queue_t* queue,
                               pnanovdb_compute_device_interface_t* device_interface)
{
    // NOP
}
void windowGlfwPollEvents(WindowGlfw* ptr)
{
    // NOP
}
pnanovdb_bool_t windowGlfwShouldClose(WindowGlfw* ptr)
{
    return PNANOVDB_TRUE;
}
void windowGlfwResize(WindowGlfw* ptr, pnanovdb_uint32_t width, pnanovdb_uint32_t height)
{
    // NOP
}
float windowGlfwGetScale(WindowGlfw* ptr)
{
    return 1.f;
}
pnanovdb_compute_swapchain_t* windowGlfwGetSwapchain(WindowGlfw* ptr)
{
    return nullptr;
}
}

#else // PNANOVDB_USE_GLFW

#    if defined(__APPLE__)
#        define VK_USE_PLATFORM_MACOS_MVK 1
#        include <vulkan/vulkan.h>
#    endif

#    define GLFW_DLL

#    define GLFW_INCLUDE_VULKAN
#    include <GLFW/glfw3.h>

// Fix for X11 Status conflict with ImGui on Linux after upgrade to imgui v1.92.0
#    if defined(__linux__)
#        undef Status
#    endif

#    include "ImguiWindowGlfw.h"

#    define GLFW_PTR(X) decltype(&X) p_##X = nullptr

#    define GLFW_PTR_LOAD(X) ptr->p_##X = (decltype(&X))pnanovdb_get_proc_address(ptr->glfw_module, #    X)

namespace pnanovdb_imgui_window_default
{
GLFW_PTR(glfwGetWindowUserPointer);

void windowSizeCallback(GLFWwindow* win, int width, int height);
void keyboardCallback(GLFWwindow* win, int key, int scanCode, int action, int modifiers);
void charInputCallback(GLFWwindow* win, uint32_t input);
void mouseMoveCallback(GLFWwindow* win, double mouseX, double mouseY);
void mouseButtonCallback(GLFWwindow* win, int button, int action, int modifiers);
void mouseWheelCallback(GLFWwindow* win, double scrollX, double scrollY);

struct WindowGlfw
{
    Window* window_parent = nullptr;
    void* glfw_module;
    pnanovdb_compute_log_print_t log_print = nullptr;

    pnanovdb_compute_device_interface_t* device_interface = nullptr;

    pnanovdb_uint32_t width = 0;
    pnanovdb_uint32_t height = 0;
    float window_scale = 1.f;

    GLFWwindow* window = nullptr;
    pnanovdb_compute_swapchain_t* swapchain = nullptr;

    GLFW_PTR(glfwInit);
    GLFW_PTR(glfwWindowHint);
    GLFW_PTR(glfwCreateWindow);
    GLFW_PTR(glfwGetPrimaryMonitor);
    GLFW_PTR(glfwGetVideoMode);
    GLFW_PTR(glfwSetWindowUserPointer);
    GLFW_PTR(glfwSetWindowPos);
    GLFW_PTR(glfwSetWindowSize);
    GLFW_PTR(glfwSetWindowSizeCallback);
    GLFW_PTR(glfwSetKeyCallback);
    GLFW_PTR(glfwSetCharCallback);
    GLFW_PTR(glfwSetMouseButtonCallback);
    GLFW_PTR(glfwSetCursorPosCallback);
    GLFW_PTR(glfwSetScrollCallback);
    GLFW_PTR(glfwCreateWindowSurface);
    GLFW_PTR(glfwGetRequiredInstanceExtensions);
    GLFW_PTR(glfwDestroyWindow);
    GLFW_PTR(glfwTerminate);
    GLFW_PTR(glfwPollEvents);
    GLFW_PTR(glfwWindowShouldClose);
    GLFW_PTR(glfwGetWindowUserPointer);
    GLFW_PTR(glfwSetWindowMonitor);
    GLFW_PTR(glfwGetMouseButton);
    GLFW_PTR(glfwGetFramebufferSize);
    GLFW_PTR(glfwGetKey);
    GLFW_PTR(glfwGetWindowContentScale);
};

WindowGlfw* createWindowGlfw(Window* window_parent,
                             pnanovdb_compute_log_print_t log_print,
                             pnanovdb_uint32_t width,
                             pnanovdb_uint32_t height)
{
    void* glfw_module = nullptr;

    glfw_module = pnanovdb_load_library("glfw3.dll", "libglfw.so.3", "libglfw.3.dylib");
    if (!glfw_module)
    {
        if (log_print)
        {
            log_print(PNANOVDB_COMPUTE_LOG_LEVEL_WARNING, "Failed to load GLFW, attempting typical absolute path.");

#    if defined(_WIN32)
            // Print PATH environment variable
            char* path = std::getenv("PATH");
            if (path)
            {
                log_print(PNANOVDB_COMPUTE_LOG_LEVEL_INFO, "\nPATH environment variable search paths:");
                char* context = nullptr;
                char* token = strtok_s(path, ";", &context);
                while (token)
                {
                    log_print(PNANOVDB_COMPUTE_LOG_LEVEL_INFO, token);
                    token = strtok_s(nullptr, ";", &context);
                }
            }

            // Print application directory
            WCHAR buffer[32768];
            DWORD result = GetModuleFileNameW(NULL, buffer, 32768);
            if (result > 0)
            {
                WCHAR* lastSlash = wcsrchr(buffer, L'\\');
                if (lastSlash)
                {
                    *lastSlash = L'\0';
                    log_print(PNANOVDB_COMPUTE_LOG_LEVEL_INFO, "\nApplication directory:");
                    log_print(PNANOVDB_COMPUTE_LOG_LEVEL_INFO, "%ls", buffer);
                }
            }

            // Print system directory
            if (GetSystemDirectoryW(buffer, 32768) > 0)
            {
                log_print(PNANOVDB_COMPUTE_LOG_LEVEL_INFO, "\nSystem directory:");
                log_print(PNANOVDB_COMPUTE_LOG_LEVEL_INFO, "%ls", buffer);
            }

            // Print Windows directory
            if (GetWindowsDirectoryW(buffer, 32768) > 0)
            {
                log_print(PNANOVDB_COMPUTE_LOG_LEVEL_INFO, "\nWindows directory:");
                log_print(PNANOVDB_COMPUTE_LOG_LEVEL_INFO, "%ls", buffer);
            }
#    endif
        }
        glfw_module = pnanovdb_load_library("glfw3.dll", "/usr/lib/libglfw.so.3", "/opt/homebrew/lib/libglfw.3.dylib");
    }
    if (!glfw_module)
    {
        if (log_print)
        {
            log_print(PNANOVDB_COMPUTE_LOG_LEVEL_ERROR, "Failed to load GLFW");
            return nullptr;
        }
    }

    auto ptr = new WindowGlfw();
    ptr->window_parent = window_parent;
    ptr->glfw_module = glfw_module;
    ptr->log_print = log_print;

    ptr->width = width;
    ptr->height = height;
    ptr->window_scale = 1.f;

    GLFW_PTR_LOAD(glfwInit);
    GLFW_PTR_LOAD(glfwWindowHint);
    GLFW_PTR_LOAD(glfwCreateWindow);
    GLFW_PTR_LOAD(glfwGetPrimaryMonitor);
    GLFW_PTR_LOAD(glfwGetVideoMode);
    GLFW_PTR_LOAD(glfwSetWindowUserPointer);
    GLFW_PTR_LOAD(glfwSetWindowPos);
    GLFW_PTR_LOAD(glfwSetWindowSize);
    GLFW_PTR_LOAD(glfwSetWindowSizeCallback);
    GLFW_PTR_LOAD(glfwSetKeyCallback);
    GLFW_PTR_LOAD(glfwSetCharCallback);
    GLFW_PTR_LOAD(glfwSetMouseButtonCallback);
    GLFW_PTR_LOAD(glfwSetCursorPosCallback);
    GLFW_PTR_LOAD(glfwSetScrollCallback);
    GLFW_PTR_LOAD(glfwCreateWindowSurface);
    GLFW_PTR_LOAD(glfwGetRequiredInstanceExtensions);
    GLFW_PTR_LOAD(glfwDestroyWindow);
    GLFW_PTR_LOAD(glfwTerminate);
    GLFW_PTR_LOAD(glfwPollEvents);
    GLFW_PTR_LOAD(glfwWindowShouldClose);
    GLFW_PTR_LOAD(glfwGetWindowUserPointer);
    GLFW_PTR_LOAD(glfwSetWindowMonitor);
    GLFW_PTR_LOAD(glfwGetMouseButton);
    GLFW_PTR_LOAD(glfwGetFramebufferSize);
    GLFW_PTR_LOAD(glfwGetKey);
    GLFW_PTR_LOAD(glfwGetWindowContentScale);

    // need global access on this one
    p_glfwGetWindowUserPointer = ptr->p_glfwGetWindowUserPointer;

    if (!ptr->p_glfwInit() && log_print)
    {
        log_print(PNANOVDB_COMPUTE_LOG_LEVEL_ERROR, "Failed to initialize GLFW");
    }

    ptr->p_glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    // ptr->p_glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);

    const char* window_name = "NanoVDB Editor";

    ptr->window = ptr->p_glfwCreateWindow(ptr->width, ptr->height, window_name, nullptr, nullptr);
    if (!ptr->window && log_print)
    {
        log_print(PNANOVDB_COMPUTE_LOG_LEVEL_ERROR, "Failed to create GLFW window");
    }

#    if defined(__linux__)
    float xscale = 1.0f, yscale = 1.0f;
    ptr->p_glfwGetWindowContentScale(ptr->window, &xscale, &yscale);
    if (log_print)
    {
        log_print(PNANOVDB_COMPUTE_LOG_LEVEL_INFO, "GLFW window content scale: %f, %f", xscale, yscale);
    }

    ptr->window_scale = xscale;
    ptr->width = (pnanovdb_uint32_t)(width * ptr->window_scale);
    ptr->height = (pnanovdb_uint32_t)(height * ptr->window_scale);
    resizeWindow(ptr->window_parent, ptr->width, ptr->height);
    ptr->p_glfwSetWindowSize(ptr->window, ptr->width, ptr->height);
#    endif

    GLFWmonitor* monitor = ptr->p_glfwGetPrimaryMonitor();
    const GLFWvidmode* mode = ptr->p_glfwGetVideoMode(monitor);

    ptr->p_glfwSetWindowUserPointer(ptr->window, ptr);

    ptr->p_glfwSetWindowPos(ptr->window, mode->width / 2 - ptr->width / 2, mode->height / 2 - ptr->height / 2);

    ptr->p_glfwSetWindowSizeCallback(ptr->window, windowSizeCallback);
    ptr->p_glfwSetKeyCallback(ptr->window, keyboardCallback);
    ptr->p_glfwSetCharCallback(ptr->window, charInputCallback);
    ptr->p_glfwSetMouseButtonCallback(ptr->window, mouseButtonCallback);
    ptr->p_glfwSetCursorPosCallback(ptr->window, mouseMoveCallback);
    ptr->p_glfwSetScrollCallback(ptr->window, mouseWheelCallback);

    return ptr;
}

void destroyWindowGlfw(WindowGlfw* ptr)
{
    if (!ptr)
    {
        return;
    }
    if (ptr->swapchain)
    {
        ptr->device_interface->destroy_swapchain(ptr->swapchain);
        ptr->swapchain = nullptr;
    }
    if (ptr->window)
    {
        ptr->p_glfwDestroyWindow(ptr->window);
    }
    if (ptr->glfw_module)
    {
        ptr->p_glfwTerminate();
        pnanovdb_free_library(ptr->glfw_module);
    }
    delete ptr;
}

void windowGlfwCreateSwapchain(WindowGlfw* ptr,
                               pnanovdb_compute_queue_t* queue,
                               pnanovdb_compute_device_interface_t* device_interface)
{
    ptr->device_interface = device_interface;

    pnanovdb_compute_swapchain_desc_t swapchain_desc = {};
    swapchain_desc.format = PNANOVDB_COMPUTE_FORMAT_B8G8R8A8_UNORM;
    auto get_framebuffer_size = [](void* window_userdata, int* width, int* height)
    {
        auto ptr = (WindowGlfw*)window_userdata;
        ptr->p_glfwGetFramebufferSize(ptr->window, width, height);
    };
    swapchain_desc.get_framebuffer_size = get_framebuffer_size;
    swapchain_desc.window_userdata = ptr;
    auto create_surface = [](void* window_userdata, void* vkinstance, void** out_surface)
    {
        auto ptr = (WindowGlfw*)window_userdata;
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        VkResult result = ptr->p_glfwCreateWindowSurface((VkInstance)vkinstance, ptr->window, nullptr, &surface);
        *out_surface = surface;
        if (result != VK_SUCCESS)
        {
            ptr->log_print(PNANOVDB_COMPUTE_LOG_LEVEL_ERROR, "glfwCreateWindowSurface() failed with result %d", result);
            ptr->log_print(PNANOVDB_COMPUTE_LOG_LEVEL_ERROR, "Required extensions:");
            uint32_t count = 0u;
            const char** list = ptr->p_glfwGetRequiredInstanceExtensions(&count);
            for (uint32_t idx = 0u; idx < count; idx++)
            {
                ptr->log_print(PNANOVDB_COMPUTE_LOG_LEVEL_ERROR, "ext[%d] name(%s)", idx, list[idx]);
            }
        }
    };
    swapchain_desc.create_surface = create_surface;

    ptr->swapchain = ptr->device_interface->create_swapchain(queue, &swapchain_desc);
}

void windowGlfwPollEvents(WindowGlfw* ptr)
{
    ptr->p_glfwPollEvents();
}

pnanovdb_bool_t windowGlfwShouldClose(WindowGlfw* ptr)
{
    if (ptr->window && ptr->p_glfwWindowShouldClose(ptr->window))
    {
        return PNANOVDB_TRUE;
    }
    return PNANOVDB_FALSE;
}

void windowGlfwResize(WindowGlfw* ptr, pnanovdb_uint32_t width, pnanovdb_uint32_t height)
{
    if (ptr->window)
    {
        windowSizeCallback(ptr->window, width, height);
    }
}

float windowGlfwGetScale(WindowGlfw* ptr)
{
    return ptr->window_scale;
}

pnanovdb_compute_swapchain_t* windowGlfwGetSwapchain(WindowGlfw* ptr)
{
    return ptr->swapchain;
}

void windowSizeCallback(GLFWwindow* win, int width, int height)
{
    auto ptr = (WindowGlfw*)p_glfwGetWindowUserPointer(win);

    if (!ptr || !ptr->swapchain)
    {
        return;
    }

    // resize swapchain
    ptr->device_interface->resize_swapchain(ptr->swapchain, (pnanovdb_uint32_t)width, (pnanovdb_uint32_t)height);

    if (width == 0 || height == 0)
    {
        return;
    }

    ptr->width = width;
    ptr->height = height;
    resizeWindow(ptr->window_parent, ptr->width, ptr->height);
}

ImGuiKey keyToImguiKey(int keycode);

void keyboardCallback(GLFWwindow* win, int key, int scanCode, int action, int modifiers)
{
    auto ptr = (WindowGlfw*)p_glfwGetWindowUserPointer(win);

    // see https://github.com/glfw/glfw/issues/1630
    bool control = (ptr->p_glfwGetKey(ptr->window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS) ||
                   (ptr->p_glfwGetKey(ptr->window, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS);
    bool shift = (ptr->p_glfwGetKey(ptr->window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS) ||
                 (ptr->p_glfwGetKey(ptr->window, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS);
    bool alt = (ptr->p_glfwGetKey(ptr->window, GLFW_KEY_LEFT_ALT) == GLFW_PRESS) ||
               (ptr->p_glfwGetKey(ptr->window, GLFW_KEY_RIGHT_ALT) == GLFW_PRESS);
    bool super = (ptr->p_glfwGetKey(ptr->window, GLFW_KEY_LEFT_SUPER) == GLFW_PRESS) ||
                 (ptr->p_glfwGetKey(ptr->window, GLFW_KEY_RIGHT_SUPER) == GLFW_PRESS);

    if (action == GLFW_PRESS || action == GLFW_RELEASE)
    {
        keyboardWindow(
            ptr->window_parent, keyToImguiKey(key), scanCode, (action == GLFW_PRESS), alt, control, shift, super);
    }
}

void charInputCallback(GLFWwindow* win, uint32_t input)
{
    auto ptr = (WindowGlfw*)p_glfwGetWindowUserPointer(win);

    charInputWindow(ptr->window_parent, input);
}

void mouseMoveCallback(GLFWwindow* win, double mouseX, double mouseY)
{
    auto ptr = (WindowGlfw*)p_glfwGetWindowUserPointer(win);

    mouseMoveWindow(ptr->window_parent, mouseX, mouseY);
}

void mouseButtonCallback(GLFWwindow* win, int button, int action, int modifiers)
{
    auto ptr = (WindowGlfw*)p_glfwGetWindowUserPointer(win);

    if (action == GLFW_PRESS || action == GLFW_RELEASE)
    {
        mouseButtonWindow(ptr->window_parent, button, (action == GLFW_PRESS), modifiers);
    }
}

void mouseWheelCallback(GLFWwindow* win, double scrollX, double scrollY)
{
    auto ptr = (WindowGlfw*)p_glfwGetWindowUserPointer(win);

    mouseWheelWindow(ptr->window_parent, scrollX, scrollY);
}

ImGuiKey keyToImguiKey(int keycode)
{
    switch (keycode)
    {
    case GLFW_KEY_TAB:
        return ImGuiKey_Tab;
    case GLFW_KEY_LEFT:
        return ImGuiKey_LeftArrow;
    case GLFW_KEY_RIGHT:
        return ImGuiKey_RightArrow;
    case GLFW_KEY_UP:
        return ImGuiKey_UpArrow;
    case GLFW_KEY_DOWN:
        return ImGuiKey_DownArrow;
    case GLFW_KEY_PAGE_UP:
        return ImGuiKey_PageUp;
    case GLFW_KEY_PAGE_DOWN:
        return ImGuiKey_PageDown;
    case GLFW_KEY_HOME:
        return ImGuiKey_Home;
    case GLFW_KEY_END:
        return ImGuiKey_End;
    case GLFW_KEY_INSERT:
        return ImGuiKey_Insert;
    case GLFW_KEY_DELETE:
        return ImGuiKey_Delete;
    case GLFW_KEY_BACKSPACE:
        return ImGuiKey_Backspace;
    case GLFW_KEY_SPACE:
        return ImGuiKey_Space;
    case GLFW_KEY_ENTER:
        return ImGuiKey_Enter;
    case GLFW_KEY_ESCAPE:
        return ImGuiKey_Escape;
    case GLFW_KEY_APOSTROPHE:
        return ImGuiKey_Apostrophe;
    case GLFW_KEY_COMMA:
        return ImGuiKey_Comma;
    case GLFW_KEY_MINUS:
        return ImGuiKey_Minus;
    case GLFW_KEY_PERIOD:
        return ImGuiKey_Period;
    case GLFW_KEY_SLASH:
        return ImGuiKey_Slash;
    case GLFW_KEY_SEMICOLON:
        return ImGuiKey_Semicolon;
    case GLFW_KEY_EQUAL:
        return ImGuiKey_Equal;
    case GLFW_KEY_LEFT_BRACKET:
        return ImGuiKey_LeftBracket;
    case GLFW_KEY_BACKSLASH:
        return ImGuiKey_Backslash;
    case GLFW_KEY_RIGHT_BRACKET:
        return ImGuiKey_RightBracket;
    case GLFW_KEY_GRAVE_ACCENT:
        return ImGuiKey_GraveAccent;
    case GLFW_KEY_CAPS_LOCK:
        return ImGuiKey_CapsLock;
    case GLFW_KEY_SCROLL_LOCK:
        return ImGuiKey_ScrollLock;
    case GLFW_KEY_NUM_LOCK:
        return ImGuiKey_NumLock;
    case GLFW_KEY_PRINT_SCREEN:
        return ImGuiKey_PrintScreen;
    case GLFW_KEY_PAUSE:
        return ImGuiKey_Pause;
    case GLFW_KEY_KP_0:
        return ImGuiKey_Keypad0;
    case GLFW_KEY_KP_1:
        return ImGuiKey_Keypad1;
    case GLFW_KEY_KP_2:
        return ImGuiKey_Keypad2;
    case GLFW_KEY_KP_3:
        return ImGuiKey_Keypad3;
    case GLFW_KEY_KP_4:
        return ImGuiKey_Keypad4;
    case GLFW_KEY_KP_5:
        return ImGuiKey_Keypad5;
    case GLFW_KEY_KP_6:
        return ImGuiKey_Keypad6;
    case GLFW_KEY_KP_7:
        return ImGuiKey_Keypad7;
    case GLFW_KEY_KP_8:
        return ImGuiKey_Keypad8;
    case GLFW_KEY_KP_9:
        return ImGuiKey_Keypad9;
    case GLFW_KEY_KP_DECIMAL:
        return ImGuiKey_KeypadDecimal;
    case GLFW_KEY_KP_DIVIDE:
        return ImGuiKey_KeypadDivide;
    case GLFW_KEY_KP_MULTIPLY:
        return ImGuiKey_KeypadMultiply;
    case GLFW_KEY_KP_SUBTRACT:
        return ImGuiKey_KeypadSubtract;
    case GLFW_KEY_KP_ADD:
        return ImGuiKey_KeypadAdd;
    case GLFW_KEY_KP_ENTER:
        return ImGuiKey_KeypadEnter;
    case GLFW_KEY_KP_EQUAL:
        return ImGuiKey_KeypadEqual;
    case GLFW_KEY_LEFT_SHIFT:
        return ImGuiKey_LeftShift;
    case GLFW_KEY_LEFT_CONTROL:
        return ImGuiKey_LeftCtrl;
    case GLFW_KEY_LEFT_ALT:
        return ImGuiKey_LeftAlt;
    case GLFW_KEY_LEFT_SUPER:
        return ImGuiKey_LeftSuper;
    case GLFW_KEY_RIGHT_SHIFT:
        return ImGuiKey_RightShift;
    case GLFW_KEY_RIGHT_CONTROL:
        return ImGuiKey_RightCtrl;
    case GLFW_KEY_RIGHT_ALT:
        return ImGuiKey_RightAlt;
    case GLFW_KEY_RIGHT_SUPER:
        return ImGuiKey_RightSuper;
    case GLFW_KEY_MENU:
        return ImGuiKey_Menu;
    case GLFW_KEY_0:
        return ImGuiKey_0;
    case GLFW_KEY_1:
        return ImGuiKey_1;
    case GLFW_KEY_2:
        return ImGuiKey_2;
    case GLFW_KEY_3:
        return ImGuiKey_3;
    case GLFW_KEY_4:
        return ImGuiKey_4;
    case GLFW_KEY_5:
        return ImGuiKey_5;
    case GLFW_KEY_6:
        return ImGuiKey_6;
    case GLFW_KEY_7:
        return ImGuiKey_7;
    case GLFW_KEY_8:
        return ImGuiKey_8;
    case GLFW_KEY_9:
        return ImGuiKey_9;
    case GLFW_KEY_A:
        return ImGuiKey_A;
    case GLFW_KEY_B:
        return ImGuiKey_B;
    case GLFW_KEY_C:
        return ImGuiKey_C;
    case GLFW_KEY_D:
        return ImGuiKey_D;
    case GLFW_KEY_E:
        return ImGuiKey_E;
    case GLFW_KEY_F:
        return ImGuiKey_F;
    case GLFW_KEY_G:
        return ImGuiKey_G;
    case GLFW_KEY_H:
        return ImGuiKey_H;
    case GLFW_KEY_I:
        return ImGuiKey_I;
    case GLFW_KEY_J:
        return ImGuiKey_J;
    case GLFW_KEY_K:
        return ImGuiKey_K;
    case GLFW_KEY_L:
        return ImGuiKey_L;
    case GLFW_KEY_M:
        return ImGuiKey_M;
    case GLFW_KEY_N:
        return ImGuiKey_N;
    case GLFW_KEY_O:
        return ImGuiKey_O;
    case GLFW_KEY_P:
        return ImGuiKey_P;
    case GLFW_KEY_Q:
        return ImGuiKey_Q;
    case GLFW_KEY_R:
        return ImGuiKey_R;
    case GLFW_KEY_S:
        return ImGuiKey_S;
    case GLFW_KEY_T:
        return ImGuiKey_T;
    case GLFW_KEY_U:
        return ImGuiKey_U;
    case GLFW_KEY_V:
        return ImGuiKey_V;
    case GLFW_KEY_W:
        return ImGuiKey_W;
    case GLFW_KEY_X:
        return ImGuiKey_X;
    case GLFW_KEY_Y:
        return ImGuiKey_Y;
    case GLFW_KEY_Z:
        return ImGuiKey_Z;
    case GLFW_KEY_F1:
        return ImGuiKey_F1;
    case GLFW_KEY_F2:
        return ImGuiKey_F2;
    case GLFW_KEY_F3:
        return ImGuiKey_F3;
    case GLFW_KEY_F4:
        return ImGuiKey_F4;
    case GLFW_KEY_F5:
        return ImGuiKey_F5;
    case GLFW_KEY_F6:
        return ImGuiKey_F6;
    case GLFW_KEY_F7:
        return ImGuiKey_F7;
    case GLFW_KEY_F8:
        return ImGuiKey_F8;
    case GLFW_KEY_F9:
        return ImGuiKey_F9;
    case GLFW_KEY_F10:
        return ImGuiKey_F10;
    case GLFW_KEY_F11:
        return ImGuiKey_F11;
    case GLFW_KEY_F12:
        return ImGuiKey_F12;
    case GLFW_KEY_F13:
        return ImGuiKey_F13;
    case GLFW_KEY_F14:
        return ImGuiKey_F14;
    case GLFW_KEY_F15:
        return ImGuiKey_F15;
    case GLFW_KEY_F16:
        return ImGuiKey_F16;
    case GLFW_KEY_F17:
        return ImGuiKey_F17;
    case GLFW_KEY_F18:
        return ImGuiKey_F18;
    case GLFW_KEY_F19:
        return ImGuiKey_F19;
    case GLFW_KEY_F20:
        return ImGuiKey_F20;
    case GLFW_KEY_F21:
        return ImGuiKey_F21;
    case GLFW_KEY_F22:
        return ImGuiKey_F22;
    case GLFW_KEY_F23:
        return ImGuiKey_F23;
    case GLFW_KEY_F24:
        return ImGuiKey_F24;
    default:
        return ImGuiKey_None;
    }
}

}

#endif