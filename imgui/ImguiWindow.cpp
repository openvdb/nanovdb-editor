// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   ImguiWindow.cpp

    \author Andrew Reidmeyer

    \brief  This file is part of the PNanoVDB Compute Vulkan reference implementation.
*/


#include "ImguiWindowGlfw.h"

#include <vector>
#include <thread>
#include <chrono>

#include "Socket.h"
#include <server/Server.h>

namespace pnanovdb_imgui_window_default
{

static inline void applyReverseZFarPlane(pnanovdb_camera_config_t* config)
{
    if (config->is_reverse_z && !config->is_orthographic)
    {
        config->far_plane = INFINITY;
    }
    else
    {
        config->far_plane = 10000.f;
    }
}

static pnanovdb_imgui_instance_interface_t* get_default_imgui_instance_interface();

void setStyle_NvidiaDark(ImGuiStyle& s);

struct Window;
void keyboardWindow(Window* ptr, ImGuiKey key, int scanCode, bool is_pressed, bool alt, bool control, bool shift, bool super);
void charInputWindow(Window* ptr, uint32_t input);
void mouseMoveWindow(Window* ptr, double mouseX, double mouseY);
void mouseButtonWindow(Window* ptr, int button, bool is_pressed, int modifiers);
void mouseWheelWindow(Window* ptr, double scrollX, double scrollY);
void resizeWindow(Window* ptr, uint32_t width, uint32_t height);

struct ImguiInstance
{
    pnanovdb_imgui_instance_interface_t instance_interface;
    pnanovdb_imgui_instance_t* instance;
    pnanovdb_imgui_renderer_interface_t renderer_interface;
    pnanovdb_imgui_renderer_t* renderer;
    void* userdata;
};

struct Window
{
    WindowGlfw* window_glfw;

    pnanovdb_compute_device_interface_t device_interface = {};
    pnanovdb_compute_interface_t compute_interface = {};
    pnanovdb_compute_log_print_t log_print = nullptr;

    pnanovdb_compute_encoder_t* encoder = nullptr;
    pnanovdb_socket_t* socket = nullptr;
    pnanovdb_server_instance_t* server = nullptr;

    std::vector<ImguiInstance> imgui_instances;
    bool enable_default_imgui = false;

    int mouse_x = 0;
    int mouse_y = 0;
    int mouse_y_inv = 0;
    pnanovdb_bool_t mouse_pressed[5u] = {};

    pnanovdb_uint32_t width = 0;
    pnanovdb_uint32_t height = 0;

    pnanovdb_camera_t camera = {};

    pnanovdb_bool_t prev_is_y_up = PNANOVDB_FALSE;
    pnanovdb_bool_t prev_is_upside_down = PNANOVDB_FALSE;
};

PNANOVDB_CAST_PAIR(pnanovdb_imgui_window_t, Window)

pnanovdb_imgui_window_t* create(const pnanovdb_compute_t* compute,
                                const pnanovdb_compute_device_t* device,
                                pnanovdb_int32_t width,
                                pnanovdb_int32_t height,
                                void** imgui_user_settings,
                                pnanovdb_bool_t enable_default_imgui,
                                pnanovdb_imgui_instance_interface_t** imgui_instance_interfaces,
                                void** imgui_instance_userdatas,
                                pnanovdb_uint64_t imgui_instance_instance_count,
                                pnanovdb_bool_t headless)
{
    pnanovdb_compute_queue_t* queue = compute->device_interface.get_device_queue(device);
    pnanovdb_compute_interface_t* compute_interface = compute->device_interface.get_compute_interface(queue);
    pnanovdb_compute_context_t* compute_context = compute->device_interface.get_compute_context(queue);

    const pnanovdb_compute_shader_interface_t* shader_interface = &compute->shader_interface;

    auto log_print = compute_interface->get_log_print(compute_context);

    auto ptr = new Window();

    ptr->width = width;
    ptr->height = height;

    // Only load GLFW if not in headless mode
    WindowGlfw* window_glfw = nullptr;
    if (!headless)
    {
        window_glfw = createWindowGlfw(ptr, log_print, width, height);
        if (!window_glfw)
        {
            headless = PNANOVDB_TRUE;
        }
    }
    if (headless)
    {
        if (log_print)
        {
            log_print(PNANOVDB_COMPUTE_LOG_LEVEL_INFO, "Running in headless mode - no GLFW window created");
        }
    }

    auto settings = new pnanovdb_imgui_settings_render_t();
    settings->data_type = PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_imgui_settings_render_t);
    pnanovdb_camera_config_default(&settings->camera_config);
    pnanovdb_camera_state_default(&settings->camera_state, PNANOVDB_FALSE);
    *imgui_user_settings = settings;

    ptr->prev_is_upside_down = settings->is_upside_down;
    ptr->prev_is_y_up = settings->is_y_up;
    ptr->window_glfw = window_glfw;
    ptr->log_print = log_print;

    pnanovdb_compute_device_interface_t_duplicate(&ptr->device_interface, &compute->device_interface);
    pnanovdb_compute_interface_t_duplicate(&ptr->compute_interface, compute_interface);

    // initialize swapchain (skip for headless mode)
    if (!headless && ptr->window_glfw)
    {
        windowGlfwCreateSwapchain(ptr->window_glfw, queue, &ptr->device_interface);
    }

    // encoder creation is deferred
    ptr->encoder = nullptr;
    ptr->socket = nullptr;
    ptr->server = nullptr;

    // initialize imgui
    ptr->imgui_instances.resize(0u);

    if (enable_default_imgui)
    {
        ptr->enable_default_imgui = true;
        ImguiInstance instance = {};
        pnanovdb_imgui_instance_interface_t_duplicate(
            &instance.instance_interface, get_default_imgui_instance_interface());
        instance.userdata = ptr;
        ptr->imgui_instances.push_back(instance);
    }
    for (pnanovdb_uint64_t instance_idx = 0u; instance_idx < imgui_instance_instance_count; instance_idx++)
    {
        ImguiInstance instance = {};
        pnanovdb_imgui_instance_interface_t_duplicate(
            &instance.instance_interface, imgui_instance_interfaces[instance_idx]);
        instance.userdata = imgui_instance_userdatas[instance_idx];
        ptr->imgui_instances.push_back(instance);
    }

    for (pnanovdb_uint64_t instance_idx = 0u; instance_idx < ptr->imgui_instances.size(); instance_idx++)
    {
        auto& inst = ptr->imgui_instances[instance_idx];

        inst.instance = inst.instance_interface.create(inst.userdata, settings, settings->data_type);

        setStyle_NvidiaDark(*inst.instance_interface.get_style(inst.instance));

        ImGuiIO& io = *inst.instance_interface.get_io(inst.instance);

#if defined(__linux__)
        ImFont* font = io.Fonts->AddFontDefault();
        if (font)
        {
            font->Scale = ptr->window_glfw ? windowGlfwGetScale(ptr->window_glfw) : 1.f;
        }
#endif

        unsigned char* pixels = nullptr;
        int tex_width = 0;
        int tex_height = 0;
        inst.instance_interface.get_tex_data_as_rgba32(inst.instance, &pixels, &tex_width, &tex_height);

        pnanovdb_imgui_renderer_interface_t_duplicate(&inst.renderer_interface, pnanovdb_imgui_get_renderer_interface());

        inst.renderer = inst.renderer_interface.create(compute, queue, pixels, tex_width, tex_height);
    }

    pnanovdb_camera_init(&ptr->camera);

    return cast(ptr);
}

void destroy(const pnanovdb_compute_t* compute,
             pnanovdb_compute_queue_t* queue,
             pnanovdb_imgui_window_t* window,
             pnanovdb_imgui_settings_render_t* settings)
{
    auto ptr = cast(window);

    for (pnanovdb_uint64_t instance_idx = 0u; instance_idx < ptr->imgui_instances.size(); instance_idx++)
    {
        auto& inst = ptr->imgui_instances[instance_idx];

        inst.instance_interface.destroy(inst.instance);
        inst.instance = nullptr;

        inst.renderer_interface.destroy(compute, queue, inst.renderer);
        inst.renderer = nullptr;
    }

    if (ptr->encoder)
    {
        ptr->device_interface.destroy_encoder(ptr->encoder);
        ptr->encoder = nullptr;
        if (ptr->socket)
        {
            pnanovdb_socket_destroy(ptr->socket);
            ptr->socket = nullptr;
        }
        if (ptr->server)
        {
            pnanovdb_get_server()->destroy_instance(ptr->server);
            ptr->server = nullptr;
        }
    }

    if (ptr->window_glfw)
    {
        destroyWindowGlfw(ptr->window_glfw);
        ptr->window_glfw = nullptr;
    }

    delete ptr;
    delete settings;
}

pnanovdb_bool_t update(const pnanovdb_compute_t* compute,
                       pnanovdb_compute_queue_t* compute_queue,
                       pnanovdb_compute_texture_transient_t* background,
                       pnanovdb_int32_t* out_width,
                       pnanovdb_int32_t* out_height,
                       pnanovdb_imgui_window_t* window,
                       pnanovdb_imgui_settings_render_t* user_settings,
                       pnanovdb_int32_t (*get_external_active_count)(void* external_active_count),
                       void* external_active_count)
{
    auto ptr = cast(window);

    pnanovdb_compute_context_t* context = compute->device_interface.get_compute_context(compute_queue);
    auto log_print = ptr->compute_interface.get_log_print(context);

    // encoder resize
    if (ptr->encoder && user_settings->encode_width > 0 && user_settings->encode_height > 0 &&
        (user_settings->encode_width != ptr->width || user_settings->encode_height != ptr->height))
    {
        compute->device_interface.wait_idle(compute_queue);

        compute->device_interface.destroy_encoder(ptr->encoder);
        ptr->encoder = nullptr;

        if (ptr->window_glfw)
        {
            windowGlfwResize(ptr->window_glfw, user_settings->encode_width, user_settings->encode_height);
        }
        else
        {
            resizeWindow(ptr, user_settings->encode_width, user_settings->encode_height);
        }
    }

    float delta_time = 1.f / 60.f;

    pnanovdb_camera_animation_tick(&ptr->camera, delta_time);

    for (pnanovdb_uint64_t instance_idx = 0u; instance_idx < ptr->imgui_instances.size(); instance_idx++)
    {
        auto& inst = ptr->imgui_instances[instance_idx];

        ImGuiIO& io = *inst.instance_interface.get_io(inst.instance);

        io.DisplaySize = ImVec2(float(ptr->width), float(ptr->height));
        io.DeltaTime = delta_time;
        for (int i = 0; i < IM_ARRAYSIZE(io.MouseDown); i++)
        {
            io.MouseDown[i] = ptr->mouse_pressed[i] != PNANOVDB_FALSE;
        }
        io.MousePos.x = (float)ptr->mouse_x;
        io.MousePos.y = (float)ptr->mouse_y;

        inst.instance_interface.update(inst.instance);
    }

    if (!ptr->encoder && user_settings->enable_encoder)
    {
        pnanovdb_compute_encoder_desc_t encoder_desc = {};
        encoder_desc.width = ptr->width;
        encoder_desc.height = ptr->height;
        encoder_desc.fps = 30;

        ptr->encoder = ptr->device_interface.create_encoder(compute_queue, &encoder_desc);
#if 0
            if (!ptr->socket)
            {
                ptr->socket = pnanovdb_socket_create(9999);
            }
#else
        if (!ptr->server)
        {
            ptr->server =
                pnanovdb_get_server()->create_instance(user_settings->server_address, user_settings->server_port, log_print);
            if (!ptr->server)
            {
                if (log_print)
                {
                    log_print(PNANOVDB_COMPUTE_LOG_LEVEL_ERROR, "Failed to create server");
                }
                ptr->device_interface.destroy_encoder(ptr->encoder);
                ptr->encoder = nullptr;
                return PNANOVDB_FALSE;
            }
        }
#endif
    }

    pnanovdb_compute_texture_t* swapchain_texture = nullptr;
    pnanovdb_compute_swapchain_t* swapchain = nullptr;
    if (ptr->window_glfw)
    {
        swapchain = windowGlfwGetSwapchain(ptr->window_glfw);
    }
    if (swapchain)
    {
        swapchain_texture = ptr->device_interface.get_swapchain_front_texture(swapchain);
    }
    pnanovdb_compute_texture_t* encoder_texture = nullptr;
    if (ptr->encoder)
    {
        encoder_texture = ptr->device_interface.get_encoder_front_texture(ptr->encoder);
    }
    if (!swapchain && !ptr->encoder)
    {
        // In headless mode without encoder, we might still want to process frames
        // but won't present them anywhere
    }

    pnanovdb_compute_texture_transient_t* swapchain_transient = nullptr;
    if (swapchain_texture)
    {
        swapchain_transient = ptr->compute_interface.register_texture_as_transient(context, swapchain_texture);
    }

    pnanovdb_compute_texture_transient_t* encoder_plane0 = nullptr;
    pnanovdb_compute_texture_transient_t* encoder_plane1 = nullptr;
    if (ptr->encoder)
    {
        pnanovdb_compute_texture_transient_t* encoder_transient =
            ptr->compute_interface.register_texture_as_transient(context, encoder_texture);
        encoder_plane0 = ptr->compute_interface.alias_texture_transient(
            context, encoder_transient, PNANOVDB_COMPUTE_FORMAT_R8_UNORM, PNANOVDB_COMPUTE_TEXTURE_ASPECT_PLANE_0);
        encoder_plane1 = ptr->compute_interface.alias_texture_transient(
            context, encoder_transient, PNANOVDB_COMPUTE_FORMAT_R8G8_UNORM, PNANOVDB_COMPUTE_TEXTURE_ASPECT_PLANE_1);
    }

    pnanovdb_compute_texture_transient_t* front_texture = background;
    for (pnanovdb_uint64_t instance_idx = 0u; instance_idx < ptr->imgui_instances.size(); instance_idx++)
    {
        auto& inst = ptr->imgui_instances[instance_idx];

        ImDrawData* draw_data = inst.instance_interface.get_draw_data(inst.instance);

        pnanovdb_compute_texture_desc_t tex_desc = {};
        tex_desc.texture_type = PNANOVDB_COMPUTE_TEXTURE_TYPE_2D;
        tex_desc.usage = PNANOVDB_COMPUTE_TEXTURE_USAGE_TEXTURE | PNANOVDB_COMPUTE_TEXTURE_USAGE_RW_TEXTURE;
        tex_desc.format = PNANOVDB_COMPUTE_FORMAT_R8G8B8A8_UNORM;
        tex_desc.width = ptr->width;
        tex_desc.height = ptr->height;
        tex_desc.depth = 1u;
        tex_desc.mip_levels = 1u;

        pnanovdb_compute_texture_transient_t* back_texture =
            ptr->compute_interface.get_texture_transient(context, &tex_desc);

        inst.renderer_interface.render(
            compute, context, inst.renderer, draw_data, ptr->width, ptr->height, front_texture, back_texture);

        // update front_texture
        front_texture = back_texture;
    }
    // copy final texture to swapchain
    if (ptr->imgui_instances.size() != 0u && swapchain_transient)
    {
        auto& inst = ptr->imgui_instances[0u];
        inst.renderer_interface.copy_texture(
            compute, context, inst.renderer, ptr->width, ptr->height, front_texture, swapchain_transient);
    }
    // copy to encoder
    if (ptr->encoder && ptr->imgui_instances.size() != 0u)
    {
        auto& inst = ptr->imgui_instances[0u];
        inst.renderer_interface.copy_texture_yuv(compute, context, inst.renderer, ptr->width, ptr->height,
                                                 front_texture, encoder_plane0, encoder_plane1, nullptr);
    }

    // encode frame
    if (ptr->encoder)
    {
        pnanovdb_uint64_t encoder_flushed_frame = 0llu;
        ptr->device_interface.present_encoder(ptr->encoder, &encoder_flushed_frame);
        pnanovdb_uint64_t encoder_data_size = 0llu;
        void* encoder_data = ptr->device_interface.map_encoder_data(ptr->encoder, &encoder_data_size);
        if (ptr->socket)
        {
            pnanovdb_socket_send(ptr->socket, encoder_data, encoder_data_size);
        }
        if (ptr->server)
        {
            pnanovdb_get_server()->push_h264(ptr->server, encoder_data, encoder_data_size);
        }
        ptr->device_interface.unmap_encoder_data(ptr->encoder);
    }

    // no encoder, no swapchain, need flush
    if (!swapchain && !ptr->encoder)
    {
        pnanovdb_uint64_t flushed_frame = 0llu;
        ptr->device_interface.flush(compute_queue, &flushed_frame, nullptr, nullptr);
    }

    // present frame
    if (swapchain)
    {
        pnanovdb_uint64_t flushed_frame = 0llu;
        ptr->device_interface.present_swapchain(swapchain, user_settings->vsync, &flushed_frame);
    }
    else
    {
        // with no present, need something to pace frame
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    if (ptr->window_glfw)
    {
        windowGlfwPollEvents(ptr->window_glfw);
    }
    {
        if (ptr->server)
        {
            pnanovdb_server_event_t event = {};
            while (pnanovdb_get_server()->pop_event(ptr->server, &event))
            {
                if (event.type == PNANOVDB_SERVER_EVENT_MOUSEMOVE)
                {
                    mouseMoveWindow(ptr, event.x * float(ptr->width), event.y * float(ptr->height));
                }
                else if (event.type == PNANOVDB_SERVER_EVENT_MOUSEDOWN)
                {
                    mouseButtonWindow(ptr, event.button, true, 0);
                }
                else if (event.type == PNANOVDB_SERVER_EVENT_MOUSEUP)
                {
                    mouseButtonWindow(ptr, event.button, false, 0);
                }
                else if (event.type == PNANOVDB_SERVER_EVENT_MOUSESCROLL)
                {
                    mouseWheelWindow(ptr, event.delta_x, event.delta_y);
                }
                else if (event.type == PNANOVDB_SERVER_EVENT_KEYDOWN)
                {
                    keyboardWindow(ptr, (ImGuiKey)event.key, event.code, true, event.alt_key, event.ctrl_key,
                                   event.shift_key, event.meta_key);
                    if (event.unicode != 0u && !event.alt_key && !event.ctrl_key && !event.meta_key)
                    {
                        charInputWindow(ptr, event.unicode);
                    }
                }
                else if (event.type == PNANOVDB_SERVER_EVENT_KEYUP)
                {
                    keyboardWindow(ptr, (ImGuiKey)event.key, event.code, false, event.alt_key, event.ctrl_key,
                                   event.shift_key, event.meta_key);
                }
                else if (event.type == PNANOVDB_SERVER_EVENT_INACTIVE)
                {
                    if (!swapchain) // swapchain means local viewer, so don't wait in that case
                    {
                        pnanovdb_get_server()->wait_until_active(
                            ptr->server, get_external_active_count, external_active_count);
                    }
                    break;
                }
            }
        }
    }

    if (out_width)
    {
        *out_width = ptr->width;
    }
    if (out_height)
    {
        *out_height = ptr->height;
    }

    if (ptr->window_glfw && windowGlfwShouldClose(ptr->window_glfw))
    {
        if (log_print)
        {
            log_print(PNANOVDB_COMPUTE_LOG_LEVEL_INFO, "GLFW Close Window.");
        }
        return PNANOVDB_FALSE;
    }

    return PNANOVDB_TRUE;
}

void get_camera_view_proj(pnanovdb_imgui_window_t* window,
                          pnanovdb_int32_t* out_width,
                          pnanovdb_int32_t* out_height,
                          pnanovdb_camera_mat_t* out_view,
                          pnanovdb_camera_mat_t* out_projection)
{
    auto ptr = cast(window);

    if (out_width)
    {
        *out_width = ptr->width;
    }
    if (out_height)
    {
        *out_height = ptr->height;
    }
    if (out_view)
    {
        pnanovdb_camera_get_view(&ptr->camera, out_view);
    }
    if (out_projection)
    {
        pnanovdb_camera_get_projection(&ptr->camera, out_projection, (float)ptr->width, (float)ptr->height);
    }
}

void get_camera(pnanovdb_imgui_window_t* window,
                pnanovdb_camera_state_t* out_camera_state,
                pnanovdb_camera_config_t* out_camera_config)
{
    auto ptr = cast(window);

    if (out_camera_state)
    {
        *out_camera_state = ptr->camera.state;
    }
    if (out_camera_config)
    {
        *out_camera_config = ptr->camera.config;
    }
}

void update_camera(pnanovdb_imgui_window_t* window, pnanovdb_imgui_settings_render_t* user_settings)
{
    auto ptr = cast(window);

    if (user_settings->sync_camera)
    {
        // apply imgui settings
        ptr->camera.state = user_settings->camera_state;
        ptr->camera.config = user_settings->camera_config;
        user_settings->sync_camera = PNANOVDB_FALSE;

        ptr->prev_is_y_up = user_settings->is_y_up;
        ptr->prev_is_upside_down = user_settings->is_upside_down;
    }

    if (user_settings->is_projection_rh != ptr->camera.config.is_projection_rh)
    {
        ptr->camera.config.is_projection_rh = user_settings->is_projection_rh;
    }
    if (user_settings->is_reverse_z != ptr->camera.config.is_reverse_z)
    {
        ptr->camera.config.is_reverse_z = user_settings->is_reverse_z;
        applyReverseZFarPlane(&ptr->camera.config);
    }
    if (user_settings->is_orthographic != ptr->camera.config.is_orthographic)
    {
        ptr->camera.config.is_orthographic = user_settings->is_orthographic;
        applyReverseZFarPlane(&ptr->camera.config);
    }

    if (user_settings->is_y_up != ptr->prev_is_y_up)
    {
        const float sign = ptr->prev_is_upside_down ? -1.f : 1.f;

        ptr->camera.state.eye_direction.x = 0.f;
        ptr->camera.state.eye_direction.y = user_settings->is_y_up ? 0.f : 1.f;
        ptr->camera.state.eye_direction.z = user_settings->is_y_up ? 1.f : 0.f;
        ptr->camera.state.eye_up.x = 0.f;
        ptr->camera.state.eye_up.y = user_settings->is_y_up ? sign : 0.f;
        ptr->camera.state.eye_up.z = user_settings->is_y_up ? 0.f : sign;

        ptr->prev_is_y_up = user_settings->is_y_up;
    }
    if (user_settings->is_upside_down != ptr->prev_is_upside_down)
    {
        ptr->camera.state.eye_up.y = -ptr->camera.state.eye_up.y;
        ptr->camera.state.eye_up.z = -ptr->camera.state.eye_up.z;

        ptr->prev_is_upside_down = user_settings->is_upside_down;
    }
    if (user_settings->sync_camera)
    {
        user_settings->sync_camera = PNANOVDB_FALSE;
    }
    else
    {
        user_settings->camera_state = ptr->camera.state;
        user_settings->camera_config = ptr->camera.config;
    }
}

struct Instance
{
    Window* window = nullptr;
    pnanovdb_imgui_settings_render_t* settings = nullptr;
};

PNANOVDB_CAST_PAIR(pnanovdb_imgui_instance_t, Instance)

pnanovdb_imgui_instance_t* imgui_create(void* userdata,
                                        void* user_settings,
                                        const pnanovdb_reflect_data_type_t* user_settings_data_type)
{
    auto ptr = new Instance();

    ptr->window = (Window*)userdata;

    if (pnanovdb_reflect_layout_compare(
            user_settings_data_type, PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_imgui_settings_render_t)))
    {
        ptr->settings = (pnanovdb_imgui_settings_render_t*)user_settings;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    return cast(ptr);
}

void imgui_destroy(pnanovdb_imgui_instance_t* instance)
{
    auto ptr = cast(instance);

    ImGui::DestroyContext();

    delete ptr;
}

void imgui_update(pnanovdb_imgui_instance_t* instance)
{
    auto inst = cast(instance);
    auto ptr = (Window*)inst->window;

    ImGui::NewFrame();

    if (inst->settings->is_projection_rh != ptr->camera.config.is_projection_rh)
    {
        ptr->camera.config.is_projection_rh = inst->settings->is_projection_rh;
    }
    if (inst->settings->is_orthographic != ptr->camera.config.is_orthographic)
    {
        ptr->camera.config.is_orthographic = inst->settings->is_orthographic;
        applyReverseZFarPlane(&ptr->camera.config);
    }
    if (inst->settings->is_reverse_z != ptr->camera.config.is_reverse_z)
    {
        ptr->camera.config.is_reverse_z = inst->settings->is_reverse_z;
        applyReverseZFarPlane(&ptr->camera.config);
    }

    ImGui::Render();
}

ImGuiStyle* imgui_get_style(pnanovdb_imgui_instance_t* instance)
{
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();

    return &s;
}

ImGuiIO* imgui_get_io(pnanovdb_imgui_instance_t* instance)
{
    ImGuiIO& io = ImGui::GetIO();

    return &io;
}

void imgui_get_tex_data_as_rgba32(pnanovdb_imgui_instance_t* instance,
                                  unsigned char** out_pixels,
                                  int* out_width,
                                  int* out_height)
{
    ImGuiIO& io = ImGui::GetIO();

    io.Fonts->GetTexDataAsRGBA32(out_pixels, out_width, out_height);
}

ImDrawData* imgui_get_draw_data(pnanovdb_imgui_instance_t* instance)
{
    return ImGui::GetDrawData();
}

static pnanovdb_imgui_instance_interface_t* get_default_imgui_instance_interface()
{
    static pnanovdb_imgui_instance_interface_t iface = { PNANOVDB_REFLECT_INTERFACE_INIT(
        pnanovdb_imgui_instance_interface_t) };
    iface.create = imgui_create;
    iface.destroy = imgui_destroy;
    iface.update = imgui_update;
    iface.get_style = imgui_get_style;
    iface.get_io = imgui_get_io;
    iface.get_tex_data_as_rgba32 = imgui_get_tex_data_as_rgba32;
    iface.get_draw_data = imgui_get_draw_data;
    return &iface;
}

void keyboardWindow(Window* ptr, ImGuiKey key, int scanCode, bool is_pressed, bool alt, bool control, bool shift, bool super)
{
    bool zeroWantCaptureKeyboard = true;
    bool zeroWantCaptureMouse = true;
    for (pnanovdb_uint64_t instance_idx = 0u; instance_idx < ptr->imgui_instances.size(); instance_idx++)
    {
        auto& inst = ptr->imgui_instances[instance_idx];

        ImGuiIO& io = *inst.instance_interface.get_io(inst.instance);

        if (io.WantCaptureKeyboard)
        {
            zeroWantCaptureKeyboard = false;
        }
        if (io.WantCaptureMouse)
        {
            zeroWantCaptureMouse = false;
        }
        // imgui always captures
        {
            io.AddKeyEvent(ImGuiMod_Ctrl, control);
            io.AddKeyEvent(ImGuiMod_Shift, shift);
            io.AddKeyEvent(ImGuiMod_Alt, alt);
            io.AddKeyEvent(ImGuiMod_Super, super);
            io.AddKeyEvent(key, is_pressed);
        }
    }
    // always report key release, conditional on key down
    if ((zeroWantCaptureKeyboard && zeroWantCaptureMouse) || !is_pressed)
    {
        pnanovdb_camera_action_t p_action = PNANOVDB_CAMERA_ACTION_UNKNOWN;
        if (is_pressed)
        {
            p_action = PNANOVDB_CAMERA_ACTION_DOWN;
        }
        else
        {
            p_action = PNANOVDB_CAMERA_ACTION_UP;
        }
        pnanovdb_camera_key_t p_key = PNANOVDB_CAMERA_KEY_UNKNOWN;
        if (key == ImGuiKey_UpArrow || key == ImGuiKey_W)
        {
            p_key = PNANOVDB_CAMERA_KEY_UP;
        }
        else if (key == ImGuiKey_DownArrow || key == ImGuiKey_S)
        {
            p_key = PNANOVDB_CAMERA_KEY_DOWN;
        }
        else if (key == ImGuiKey_LeftArrow || key == ImGuiKey_A)
        {
            p_key = PNANOVDB_CAMERA_KEY_LEFT;
        }
        else if (key == ImGuiKey_RightArrow || key == ImGuiKey_D)
        {
            p_key = PNANOVDB_CAMERA_KEY_RIGHT;
        }
        pnanovdb_camera_key_update(&ptr->camera, p_key, p_action);
    }
}

void charInputWindow(Window* ptr, uint32_t input)
{
    for (pnanovdb_uint64_t instance_idx = 0u; instance_idx < ptr->imgui_instances.size(); instance_idx++)
    {
        auto& inst = ptr->imgui_instances[instance_idx];

        ImGuiIO& io = *inst.instance_interface.get_io(inst.instance);
        // imgui always captures
        {
            io.AddInputCharacter(input);
        }
    }
}

void mouseMoveWindow(Window* ptr, double mouseX, double mouseY)
{
    int x = int(mouseX);
    int y = int(mouseY);

    ptr->mouse_x = x;
    ptr->mouse_y = y;
    ptr->mouse_y_inv = ptr->height - 1 - y;

    bool zeroWantCaptureMouse = true;
    for (pnanovdb_uint64_t instance_idx = 0u; instance_idx < ptr->imgui_instances.size(); instance_idx++)
    {
        auto& inst = ptr->imgui_instances[instance_idx];

        ImGuiIO& io = *inst.instance_interface.get_io(inst.instance);

        if (io.WantCaptureMouse)
        {
            zeroWantCaptureMouse = false;
        }
    }
    if (zeroWantCaptureMouse)
    {
        pnanovdb_camera_mouse_update(&ptr->camera, PNANOVDB_CAMERA_MOUSE_BUTTON_UNKNOWN, PNANOVDB_CAMERA_ACTION_UNKNOWN,
                                     ptr->mouse_x, ptr->mouse_y, (int)ptr->width, (int)ptr->height);
    }
}

void mouseButtonWindow(Window* ptr, int button, bool is_pressed, int modifiers)
{
    bool zeroWantCaptureMouse = true;
    for (pnanovdb_uint64_t instance_idx = 0u; instance_idx < ptr->imgui_instances.size(); instance_idx++)
    {
        auto& inst = ptr->imgui_instances[instance_idx];

        ImGuiIO& io = *inst.instance_interface.get_io(inst.instance);

        if (io.WantCaptureMouse)
        {
            zeroWantCaptureMouse = false;
        }

        // imgui
        if (button >= 0 && button < 5)
        {
            if (is_pressed)
            {
                ptr->mouse_pressed[button] = PNANOVDB_TRUE;
            }
            else
            {
                ptr->mouse_pressed[button] = PNANOVDB_FALSE;
            }
        }
    }
    if (zeroWantCaptureMouse)
    {
        pnanovdb_camera_action_t p_action = PNANOVDB_CAMERA_ACTION_UNKNOWN;
        if (is_pressed)
        {
            p_action = PNANOVDB_CAMERA_ACTION_DOWN;
        }
        else
        {
            p_action = PNANOVDB_CAMERA_ACTION_UP;
        }
        pnanovdb_camera_mouse_button_t p_mouse = PNANOVDB_CAMERA_MOUSE_BUTTON_UNKNOWN;
        if (button == 0)
        {
            p_mouse = PNANOVDB_CAMERA_MOUSE_BUTTON_LEFT;
        }
        else if (button == 1)
        {
            p_mouse = PNANOVDB_CAMERA_MOUSE_BUTTON_RIGHT;
        }
        else if (button == 2)
        {
            p_mouse = PNANOVDB_CAMERA_MOUSE_BUTTON_MIDDLE;
        }
        pnanovdb_camera_mouse_update(
            &ptr->camera, p_mouse, p_action, ptr->mouse_x, ptr->mouse_y, (int)ptr->width, (int)ptr->height);
    }
}

void mouseWheelWindow(Window* ptr, double scrollX, double scrollY)
{
    bool zeroWantCaptureMouse = true;
    for (pnanovdb_uint64_t instance_idx = 0u; instance_idx < ptr->imgui_instances.size(); instance_idx++)
    {
        auto& inst = ptr->imgui_instances[instance_idx];

        ImGuiIO& io = *inst.instance_interface.get_io(inst.instance);

        if (io.WantCaptureMouse)
        {
            zeroWantCaptureMouse = false;
        }

        io.MouseWheelH += (float)scrollX;
        io.MouseWheel += (float)scrollY;
    }
    if (zeroWantCaptureMouse)
    {
        pnanovdb_camera_mouse_wheel_update(&ptr->camera, (float)scrollX, (float)scrollY);
    }
}

void resizeWindow(Window* ptr, uint32_t width, uint32_t height)
{
    ptr->width = width;
    ptr->height = height;
}

void setStyle_NvidiaDark(ImGuiStyle& s)
{
    s.FrameRounding = 4.0f;

    // Settings
    s.WindowPadding = ImVec2(8.0f, 8.0f);
    s.PopupRounding = 4.0f;
    s.FramePadding = ImVec2(8.0f, 4.0f);
    s.ItemSpacing = ImVec2(6.0f, 6.0f);
    s.ItemInnerSpacing = ImVec2(4.0f, 4.0f);
    s.TouchExtraPadding = ImVec2(0.0f, 0.0f);
    s.IndentSpacing = 21.0f;
    s.ScrollbarSize = 16.0f;
    s.GrabMinSize = 8.0f;

    // BorderSize
    s.WindowBorderSize = 1.0f;
    s.ChildBorderSize = 1.0f;
    s.PopupBorderSize = 1.0f;
    s.FrameBorderSize = 0.0f;
    s.TabBorderSize = 0.0f;

    // Rounding
    s.WindowRounding = 4.0f;
    s.ChildRounding = 4.0f;
    s.FrameRounding = 4.0f;
    s.ScrollbarRounding = 4.0f;
    s.GrabRounding = 4.0f;
    s.TabRounding = 4.0f;

    // Alignment
    s.WindowTitleAlign = ImVec2(0.5f, 0.5f);
    s.ButtonTextAlign = ImVec2(0.48f, 0.5f);

    s.DisplaySafeAreaPadding = ImVec2(3.0f, 3.0f);

    // Colors
    s.Colors[::ImGuiCol_Text] = ImVec4(0.89f, 0.89f, 0.89f, 1.00f);
    s.Colors[::ImGuiCol_Text] = ImVec4(0.89f, 0.89f, 0.89f, 1.00f);
    s.Colors[::ImGuiCol_TextDisabled] = ImVec4(0.43f, 0.43f, 0.43f, 1.00f);
    s.Colors[::ImGuiCol_WindowBg] = ImVec4(0.26f, 0.26f, 0.26f, 1.00f);
    s.Colors[::ImGuiCol_ChildBg] = ImVec4(0.25f, 0.25f, 0.25f, 1.00f);
    s.Colors[::ImGuiCol_PopupBg] = ImVec4(0.25f, 0.25f, 0.25f, 1.00f);
    s.Colors[::ImGuiCol_Border] = ImVec4(0.29f, 0.29f, 0.29f, 1.00f);
    s.Colors[::ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 1.00f);
    s.Colors[::ImGuiCol_FrameBg] = ImVec4(0.14f, 0.14f, 0.14f, 1.00f);
    s.Colors[::ImGuiCol_FrameBgHovered] = ImVec4(0.29f, 0.29f, 0.29f, 1.00f);
    s.Colors[::ImGuiCol_FrameBgActive] = ImVec4(0.16f, 0.16f, 0.16f, 1.00f);
    s.Colors[::ImGuiCol_TitleBg] = ImVec4(0.14f, 0.14f, 0.14f, 1.00f);
    s.Colors[::ImGuiCol_TitleBgActive] = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
    s.Colors[::ImGuiCol_TitleBgCollapsed] = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
    s.Colors[::ImGuiCol_MenuBarBg] = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
    s.Colors[::ImGuiCol_ScrollbarBg] = ImVec4(0.16f, 0.16f, 0.16f, 1.00f);
    s.Colors[::ImGuiCol_ScrollbarGrab] = ImVec4(0.51f, 0.50f, 0.50f, 1.00f);
    s.Colors[::ImGuiCol_ScrollbarGrabHovered] = ImVec4(1.00f, 0.99f, 0.99f, 0.58f);
    s.Colors[::ImGuiCol_ScrollbarGrabActive] = ImVec4(0.47f, 0.53f, 0.54f, 0.76f);
    s.Colors[::ImGuiCol_CheckMark] = ImVec4(0.89f, 0.89f, 0.89f, 1.00f);
    s.Colors[::ImGuiCol_SliderGrab] = ImVec4(0.59f, 0.59f, 0.59f, 1.00f);
    s.Colors[::ImGuiCol_SliderGrabActive] = ImVec4(0.47f, 0.53f, 0.54f, 0.76f);
    s.Colors[::ImGuiCol_Button] = ImVec4(0.16f, 0.16f, 0.16f, 1.00f);
    s.Colors[::ImGuiCol_ButtonHovered] = ImVec4(0.59f, 0.59f, 0.59f, 1.00f);
    s.Colors[::ImGuiCol_ButtonActive] = ImVec4(0.47f, 0.53f, 0.54f, 0.76f);
    s.Colors[::ImGuiCol_Header] = ImVec4(0.16f, 0.16f, 0.16f, 1.00f);
    s.Colors[::ImGuiCol_HeaderHovered] = ImVec4(0.22f, 0.22f, 0.22f, 1.00f);
    s.Colors[::ImGuiCol_HeaderActive] = ImVec4(0.30f, 0.30f, 0.30f, 1.00f);
    s.Colors[::ImGuiCol_Separator] = ImVec4(0.16f, 0.16f, 0.16f, 1.00f);
    s.Colors[::ImGuiCol_SeparatorHovered] = ImVec4(0.23f, 0.44f, 0.69f, 1.00f);
    s.Colors[::ImGuiCol_SeparatorActive] = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
    s.Colors[::ImGuiCol_ResizeGrip] = ImVec4(0.16f, 0.16f, 0.16f, 1.00f);
    s.Colors[::ImGuiCol_ResizeGripHovered] = ImVec4(0.23f, 0.44f, 0.69f, 1.00f);
    s.Colors[::ImGuiCol_ResizeGripActive] = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
    s.Colors[::ImGuiCol_Tab] = ImVec4(0.16f, 0.16f, 0.16f, 1.00f);
    s.Colors[::ImGuiCol_TabHovered] = ImVec4(0.6f, 0.6f, 0.6f, 0.58f);
    s.Colors[::ImGuiCol_TabActive] = ImVec4(0.35f, 0.35f, 0.35f, 1.00f);
    s.Colors[::ImGuiCol_TabUnfocused] = ImVec4(0.16f, 0.16f, 0.16f, 1.00f);
    s.Colors[::ImGuiCol_TabUnfocusedActive] = ImVec4(0.25f, 0.25f, 0.25f, 1.00f);
    // s.Colors[::ImGuiCol_DockingPreview] = ImVec4(0.26f, 0.59f, 0.98f, 0.70f);
    // s.Colors[::ImGuiCol_DockingEmptyBg] = ImVec4(0.25f, 0.25f, 0.25f, 1.00f);
    s.Colors[::ImGuiCol_PlotLines] = ImVec4(0.61f, 0.61f, 0.61f, 1.00f);
    s.Colors[::ImGuiCol_PlotLinesHovered] = ImVec4(1.00f, 0.43f, 0.35f, 1.00f);
    s.Colors[::ImGuiCol_PlotHistogram] = ImVec4(0.90f, 0.70f, 0.00f, 1.00f);
    s.Colors[::ImGuiCol_PlotHistogramHovered] = ImVec4(1.00f, 0.60f, 0.00f, 1.00f);
    s.Colors[::ImGuiCol_TextSelectedBg] = ImVec4(0.97f, 0.97f, 0.97f, 0.19f);
    s.Colors[::ImGuiCol_DragDropTarget] = ImVec4(0.38f, 0.62f, 0.80f, 1.0f);
    s.Colors[::ImGuiCol_NavHighlight] = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
    s.Colors[::ImGuiCol_NavWindowingHighlight] = ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
    s.Colors[::ImGuiCol_NavWindowingDimBg] = ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
    s.Colors[::ImGuiCol_ModalWindowDimBg] = ImVec4(0.80f, 0.80f, 0.80f, 0.35f);
}
}

pnanovdb_imgui_window_interface_t* pnanovdb_imgui_get_window_interface()
{
    using namespace pnanovdb_imgui_window_default;
    static pnanovdb_imgui_window_interface_t iface = { PNANOVDB_REFLECT_INTERFACE_INIT(pnanovdb_imgui_window_interface_t) };
    iface.create = create;
    iface.destroy = destroy;
    iface.update = update;
    iface.get_camera_view_proj = get_camera_view_proj;
    iface.get_camera = get_camera;
    iface.update_camera = update_camera;
    return &iface;
}
