// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/server/Server.cpp

    \author Andrew Reidmeyer

    \brief  This file provides an implementation of a HTTP video streaming server.
*/

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif

#include "Server.h"
#include "EmbeddedFiles.h"
#include "nanovdb_editor/putil/Reflect.h"

#include <nlohmann/json.hpp>

#include <restinio/core.hpp>
#include <restinio/websocket/websocket.hpp>
#include <map>
#include <chrono>

#include <thread>
#include <mutex>

#include <imgui.h>

namespace rws = restinio::websocket::basic;
namespace restinio
{
namespace websocket
{
namespace basic
{
void activate(ws_t& ws);
}
}
}
using router_t = restinio::router::express_router_t<>;
using ws_registry_t = std::map<std::uint64_t, rws::ws_handle_t>;

using traits_t = restinio::traits_t<restinio::asio_timer_manager_t,
                                    // restinio::single_threaded_ostream_logger_t,
                                    restinio::null_logger_t,
                                    router_t>;

namespace
{
int button_to_imgui(int button);
void key_to_imgui(const std::string& key, int* key_imgui, uint32_t* unicode);
int code_to_imgui(const std::string& code);

static const uint32_t ring_buffer_size = 60u;

struct server_frame_metadata_t
{
    uint64_t frame_id;
    uint width;
    uint height;
};

struct server_instance_t
{
    std::shared_ptr<restinio::asio_ns::io_context> ioctx;
    restinio::running_server_handle_t<traits_t> server;

    std::string serveraddress;
    int port;
    pnanovdb_compute_log_print_t log_print;

    std::vector<std::vector<char>> buffers;
    std::vector<server_frame_metadata_t> frame_metadatas;
    std::map<uint64_t, uint64_t> client_ring_buffer_idx;
    uint32_t ring_buffer_idx = 0u;
    uint64_t frame_id_counter = 0llu;
    std::vector<pnanovdb_server_event_t> events;
};

PNANOVDB_CAST_PAIR(pnanovdb_server_instance_t, server_instance_t)

ws_registry_t ws_registry;
std::mutex g_mutex;
server_instance_t* g_server_instance = nullptr;
restinio::asio_ns::io_context* g_ioctx;
std::shared_ptr<restinio::asio_ns::steady_timer> g_timer;

void send_video(const asio::error_code& ec)
{
    if (!ec)
    {
        std::lock_guard<std::mutex> guard(g_mutex);

        if (g_server_instance && g_server_instance->buffers.size() != 0u && ws_registry.size() != 0u)
        {
            for (auto wsh_itr = ws_registry.begin(); wsh_itr != ws_registry.end(); wsh_itr++)
            {
                uint64_t connection_id = wsh_itr->first;
                auto ring_buf_it = g_server_instance->client_ring_buffer_idx.find(connection_id);
                if (ring_buf_it == g_server_instance->client_ring_buffer_idx.end())
                {
                    continue;
                }
                if (ring_buf_it->second == ~0u && g_server_instance->ring_buffer_idx == 1u)
                {
                    ring_buf_it->second = 0u;
                }
                while (ring_buf_it->second != ~0u && ring_buf_it->second != g_server_instance->ring_buffer_idx)
                {
                    auto& front = g_server_instance->buffers[ring_buf_it->second];
                    server_frame_metadata_t metadata = g_server_instance->frame_metadatas[ring_buf_it->second];
                    ring_buf_it->second = (ring_buf_it->second + 1) % ring_buffer_size;

                    // printf("Sending %zu bytes of video\n", front.size());

                    auto& wsh = wsh_itr->second;

                    nlohmann::json msg = {
                        {"type", "event"},
                        {"eventType", "frameid"},
                        {"frameid", metadata.frame_id},
                        {"width", metadata.width},
                        {"height", metadata.height}
                    };

                    wsh->send_message(rws::final_frame_flag_t::final_frame, rws::opcode_t::text_frame,
                                    restinio::writable_item_t(msg.dump()));
                    wsh->send_message(rws::final_frame_flag_t::final_frame, rws::opcode_t::binary_frame,
                                      restinio::writable_item_t(front));
                }
            }
        }

        g_timer = std::make_shared<restinio::asio_ns::steady_timer>(*g_ioctx);
        g_timer->expires_after(std::chrono::milliseconds(5));
        g_timer->async_wait(send_video);
    }
};

std::unique_ptr<router_t> server_handler(restinio::asio_ns::io_context& ioctx)
{
    auto router = std::make_unique<router_t>();

    router->http_get("/",
                     [](auto req, auto params)
                     {
                         // printf("/index.html !!!!\n");

                         return req->create_response()
                             .append_header(restinio::http_field::server, "NanoVDB Editor Server")
                             .append_header_date_field()
                             .append_header(restinio::http_field::content_type, "text/html")
                             .set_body(INDEX_HTML)
                             .done();
                     });

    router->http_get("/jmuxer.min.js",
                     [](auto req, auto params)
                     {
                         // printf("/jmuxer.min.js !!!!\n");

                         return req->create_response()
                             .append_header(restinio::http_field::server, "NanoVDB Editor Server")
                             .append_header_date_field()
                             .append_header(restinio::http_field::content_type, "text/javascript")
                             .set_body(JMUXER_JS)
                             .done();
                     });

    router->http_get("/ws",
                     [&ioctx](auto req, auto params)
                     {
                         // printf("/ws !!!!\n");
                         if (req->header().connection() == restinio::http_connection_header_t::upgrade)
                         {
                             // printf("WebSocket upgrade requested\n");

                             auto wsh = rws::upgrade<traits_t>(
                                 *req, rws::activation_t::immediate,
                                 [](auto wsh, auto m)
                                 {
                                     // printf("WebSocket handler!\n");
                                     if (m->opcode() == rws::opcode_t::text_frame ||
                                         m->opcode() == rws::opcode_t::binary_frame ||
                                         m->opcode() == rws::opcode_t::continuation_frame)
                                     {
                                         // printf("WebSocket text/binary/continuation\n");
                                         const auto& str = m->payload();
                                         // printf("Websocket recv: %s\n", str.c_str());

                                         nlohmann::json msg = nlohmann::json::parse(str);
                                         if (msg["type"] == "event")
                                         {
                                             std::lock_guard<std::mutex> guard(g_mutex);
                                             pnanovdb_server_event_t event = {};
                                             const auto eventType = msg["eventType"];
                                             if (eventType == "mousemove")
                                             {
                                                 event.type = PNANOVDB_SERVER_EVENT_MOUSEMOVE;
                                                 event.x = msg["x"];
                                                 event.y = msg["y"];
                                             }
                                             else if (eventType == "mousedown")
                                             {
                                                 event.type = PNANOVDB_SERVER_EVENT_MOUSEDOWN;
                                                 event.button = button_to_imgui(msg["button"]);
                                             }
                                             else if (eventType == "mouseup")
                                             {
                                                 event.type = PNANOVDB_SERVER_EVENT_MOUSEUP;
                                                 event.button = button_to_imgui(msg["button"]);
                                             }
                                             else if (eventType == "mousewheel")
                                             {
                                                 event.type = PNANOVDB_SERVER_EVENT_MOUSESCROLL;
                                                 event.delta_x = msg["deltaX"];
                                                 event.delta_y = msg["deltaY"];
                                                 event.delta_x *= (1.f / 120.f);
                                                 event.delta_y *= (-1.f / 120.f);
                                             }
                                             else if (eventType == "keydown")
                                             {
                                                 auto key = msg["key"].get<std::string>();
                                                 auto code = msg["code"].get<std::string>();
                                                 event.type = PNANOVDB_SERVER_EVENT_KEYDOWN;
                                                 key_to_imgui(key, &event.key, &event.unicode);
                                                 event.code = code_to_imgui(code);
                                                 event.alt_key = msg["altKey"];
                                                 event.ctrl_key = msg["ctrlKey"];
                                                 event.shift_key = msg["shiftKey"];
                                                 event.meta_key = msg["metaKey"];
                                             }
                                             else if (eventType == "keyup")
                                             {
                                                 auto key = msg["key"].get<std::string>();
                                                 auto code = msg["code"].get<std::string>();
                                                 event.type = PNANOVDB_SERVER_EVENT_KEYUP;
                                                 key_to_imgui(key, &event.key, &event.unicode);
                                                 event.code = code_to_imgui(code);
                                                 event.alt_key = msg["altKey"];
                                                 event.ctrl_key = msg["ctrlKey"];
                                                 event.shift_key = msg["shiftKey"];
                                                 event.meta_key = msg["metaKey"];
                                             }
                                             else if (eventType == "frameid")
                                             {
                                                uint64_t frame_id = msg["frameid"].get<uint64_t>();
                                                // TODO: compute latency
                                                //if (g_server_instance && g_server_instance->log_print)
                                                //{
                                                //    g_server_instance->log_print(PNANOVDB_COMPUTE_LOG_LEVEL_INFO, "frame_id(%zu)", frame_id);
                                                //}
                                             }
                                             else if (eventType == "resize")
                                             {
                                                event.type = PNANOVDB_SERVER_EVENT_RESIZE;
                                                event.width = msg["width"];
                                                event.height = msg["height"];
                                             }

                                             if (g_server_instance)
                                             {
                                                 g_server_instance->events.push_back(event);
                                             }
                                         }

                                         // printf("Websocket recv end: %s\n", str.c_str());
                                         // wsh->send_message(*m);
                                     }
                                     else if (m->opcode() == rws::opcode_t::ping_frame)
                                     {
                                         // printf("WebSocket ping/pong\n");
                                         auto resp = *m;
                                         resp.set_opcode(rws::opcode_t::pong_frame);
                                         wsh->send_message(resp);
                                     }
                                     else if (m->opcode() == rws::opcode_t::connection_close_frame)
                                     {
                                         // printf("WebSocket connection close\n");
                                         ws_registry.erase(wsh->connection_id());
                                         if (g_server_instance)
                                         {
                                             g_server_instance->client_ring_buffer_idx.erase(wsh->connection_id());
                                         }
                                     }
                                 });
                             // printf("WebSocket upgrade complete!\n");
                             ws_registry.emplace(wsh->connection_id(), wsh);
                             if (g_server_instance)
                             {
                                 g_server_instance->client_ring_buffer_idx.emplace(wsh->connection_id(), ~0u);
                             }

                             if (g_timer == nullptr)
                             {
                                 g_timer = std::make_shared<restinio::asio_ns::steady_timer>(ioctx);
                                 g_timer->expires_after(std::chrono::milliseconds(5));
                                 g_timer->async_wait(send_video);
                             }

                             // printf("WebSocket connection id(%llu)\n", (long long unsigned int)wsh->connection_id());

                             return restinio::request_accepted();
                         }
                         else
                         {
                             return restinio::request_rejected();
                         }
                     });

    return router;
}

pnanovdb_server_instance_t* create_instance(const char* serveraddress, int port, int max_attempts, pnanovdb_compute_log_print_t log_print)
{
    auto ptr = new server_instance_t();

    ptr->buffers.resize(ring_buffer_size);
    ptr->frame_metadatas.resize(ring_buffer_size);

    ptr->serveraddress = serveraddress;
    ptr->port = port;
    ptr->log_print = log_print;

    const char* restinio_address = "127.0.0.1";
    if (!ptr->serveraddress.empty())
    {
        restinio_address = ptr->serveraddress.c_str();
    }

    // always try at least once
    if (max_attempts < 1)
    {
        max_attempts = 1;
    }
    // there can only be 65535 ports anyways
    if (max_attempts > 65535)
    {
        max_attempts = 65535;
    }

    int attempt = 0;
    for (; attempt < max_attempts; attempt++)
    {
        std::lock_guard<std::mutex> guard(g_mutex);

        using namespace std::chrono;

        ptr->ioctx = std::make_shared<restinio::asio_ns::io_context>();

        g_ioctx = ptr->ioctx.get();
        bool success = true;
        try
        {
            ptr->server = restinio::run_async<traits_t>(ptr->ioctx,
                                                        restinio::server_settings_t<traits_t>{}
                                                            .port(ptr->port)
                                                            .address(restinio_address)
                                                            .request_handler(server_handler(*(ptr->ioctx.get())))
                                                            //.read_next_http_message_timelimit(10s)
                                                            //.write_http_response_timelimit(1s)
                                                            //.handle_request_timeout(1s)
                                                            .cleanup_func([&]() { ws_registry.clear(); }),
                                                        1u);
        }
        catch (const std::system_error& e)
        {
            if (ptr->log_print)
            {
                ptr->log_print(PNANOVDB_COMPUTE_LOG_LEVEL_ERROR, "Error starting server - %s", e.what());
            }
            success = false;
        }
        if (success)
        {
            g_server_instance = ptr;
            break;
        }
        ptr->port++;
    }
    if (attempt == max_attempts)
    {
        ptr->log_print(PNANOVDB_COMPUTE_LOG_LEVEL_ERROR, "Server create failed after %d attempts", max_attempts);
        delete ptr;
        return nullptr;
    }
    ptr->log_print(PNANOVDB_COMPUTE_LOG_LEVEL_INFO, "Server created on port(%d)", ptr->port);

    return cast(ptr);
}

void push_h264(pnanovdb_server_instance_t* instance,
               const void* data,
               pnanovdb_uint64_t data_size,
               pnanovdb_uint32_t width,
               pnanovdb_uint32_t height)
{
    auto ptr = cast(instance);

    std::lock_guard<std::mutex> guard(g_mutex);

    const char* data_char = (const char*)data;

    server_frame_metadata_t metadata = {};
    metadata.frame_id = ptr->frame_id_counter;
    metadata.width = width;
    metadata.height = height;

    ptr->buffers[ptr->ring_buffer_idx].assign(data_char, data_char + data_size);
    ptr->frame_metadatas[ptr->ring_buffer_idx] = metadata;

    ptr->ring_buffer_idx = (ptr->ring_buffer_idx + 1) % ring_buffer_size;
    ptr->frame_id_counter++;
}

pnanovdb_bool_t pop_event(pnanovdb_server_instance_t* instance, pnanovdb_server_event_t* out_event)
{
    auto ptr = cast(instance);

    std::lock_guard<std::mutex> guard(g_mutex);

    if (ptr->events.size() == 0 && ptr->client_ring_buffer_idx.empty())
    {
        pnanovdb_server_event_t inactive_event = {};
        inactive_event.type = PNANOVDB_SERVER_EVENT_INACTIVE;
        *out_event = inactive_event;
        return PNANOVDB_TRUE;
    }

    if (ptr->events.size() == 0)
    {
        return PNANOVDB_FALSE;
    }

    *out_event = ptr->events.front();
    for (size_t idx = 1u; idx < g_server_instance->events.size(); idx++)
    {
        g_server_instance->events[idx - 1u] = g_server_instance->events[idx];
    }
    g_server_instance->events.pop_back();

    return PNANOVDB_TRUE;
}

void wait_until_active(pnanovdb_server_instance_t* instance,
                       pnanovdb_int32_t (*get_external_active_count)(void* external_active_count),
                       void* external_active_count)
{
    auto ptr = cast(instance);

    if (ptr->log_print)
    {
        ptr->log_print(PNANOVDB_COMPUTE_LOG_LEVEL_DEBUG, "Server stream going inactive.");
    }
    bool is_active = false;
    while (!is_active)
    {
        {
            std::lock_guard<std::mutex> guard(g_mutex);
            if (!ptr->client_ring_buffer_idx.empty())
            {
                is_active = true;
            }
        }
        if (get_external_active_count)
        {
            if (get_external_active_count(external_active_count) != 0)
            {
                is_active = true;
            }
        }
        if (is_active)
        {
            if (ptr->log_print)
            {
                ptr->log_print(PNANOVDB_COMPUTE_LOG_LEVEL_DEBUG, "Server stream going active.");
            }
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void destroy_instance(pnanovdb_server_instance_t* instance)
{
    auto ptr = cast(instance);

    std::lock_guard<std::mutex> guard(g_mutex);

    ptr->server->stop();
    ptr->server->wait();

    delete ptr;
}

struct key_map_t
{
    int key;
    const char* lower;
    const char* upper;
    uint32_t lower_unicode;
    uint32_t upper_unicode;
};
static const key_map_t mappings[] = {
    { ImGuiKey_Tab, "Tab", "Tab", 0x00, 0x00 },
    { ImGuiKey_LeftArrow, "ArrowLeft", "ArrowLeft", 0x00, 0x00 },
    { ImGuiKey_RightArrow, "ArrowRight", "ArrowRight", 0x00, 0x00 },
    { ImGuiKey_UpArrow, "ArrowUp", "ArrowUp", 0x00, 0x00 },
    { ImGuiKey_DownArrow, "ArrowDown", "ArrowDown", 0x00, 0x00 },
    { ImGuiKey_PageUp, "PageUp", "PageUp", 0x00, 0x00 },
    { ImGuiKey_PageDown, "PageDown", "PageDown", 0x00, 0x00 },
    { ImGuiKey_Home, "Home", "Home", 0x00, 0x00 },
    { ImGuiKey_End, "End", "End", 0x00, 0x00 },
    { ImGuiKey_Insert, "Insert", "Insert", 0x00, 0x00 },
    { ImGuiKey_Delete, "Delete", "Delete", 0x00, 0x00 },
    { ImGuiKey_Backspace, "Backspace", "Backspace", 0x00, 0x00 },
    { ImGuiKey_Space, " ", " ", 0x20, 0x20 },
    { ImGuiKey_Enter, "Enter", "Enter", 0x00, 0x00 },
    { ImGuiKey_Escape, "Escape", "Escape", 0x00, 0x00 },
    { ImGuiKey_Apostrophe, "'", "\"", 0x27, 0x22 },
    { ImGuiKey_Comma, ",", "<", 0x2C, 0x3C },
    { ImGuiKey_Minus, "-", "_", 0x2D, 0x5F },
    { ImGuiKey_Period, ".", ">", 0x2E, 0x3E },
    { ImGuiKey_Slash, "/", "?", 0x2F, 0x3F },
    { ImGuiKey_Semicolon, ";", ":", 0x3B, 0x3A },
    { ImGuiKey_Equal, "=", "+", 0x3D, 0x2B },
    { ImGuiKey_LeftBracket, "[", "{", 0x5B, 0x7B },
    { ImGuiKey_Backslash, "\\", "|", 0x5C, 0x7C },
    { ImGuiKey_RightBracket, "]", "}", 0x5D, 0x7D },
    { ImGuiKey_GraveAccent, "`", "~", 0x60, 0x7E },
    { ImGuiKey_CapsLock, "CapsLock", "CapsLock", 0x00, 0x00 },
    { ImGuiKey_ScrollLock, "ScrollLock", "ScrollLock", 0x00, 0x00 },
    { ImGuiKey_NumLock, "NumLock", "NumLock", 0x00, 0x00 },
    { ImGuiKey_PrintScreen, "PrintScreen", "PrintScreen", 0x00, 0x00 },
    { ImGuiKey_Pause, "Pause", "Pause", 0x00, 0x00 },
    { ImGuiKey_Keypad0, "0", "0", 0x30, 0x30 },
    { ImGuiKey_Keypad1, "1", "1", 0x31, 0x31 },
    { ImGuiKey_Keypad2, "2", "2", 0x32, 0x32 },
    { ImGuiKey_Keypad3, "3", "3", 0x33, 0x33 },
    { ImGuiKey_Keypad4, "4", "4", 0x34, 0x34 },
    { ImGuiKey_Keypad5, "5", "5", 0x35, 0x35 },
    { ImGuiKey_Keypad6, "6", "6", 0x36, 0x36 },
    { ImGuiKey_Keypad7, "7", "7", 0x37, 0x37 },
    { ImGuiKey_Keypad8, "8", "8", 0x38, 0x38 },
    { ImGuiKey_Keypad9, "9", "9", 0x39, 0x39 },
    { ImGuiKey_KeypadDecimal, ".", "Decimal", 0x2E, 0x2E },
    { ImGuiKey_KeypadDivide, "/", "Divide", 0x2F, 0x2F },
    { ImGuiKey_KeypadMultiply, "*", "Multiply", 0x2A, 0x2A },
    { ImGuiKey_KeypadSubtract, "-", "Subtract", 0x2D, 0x2D },
    { ImGuiKey_KeypadAdd, "+", "Add", 0x2B, 0x2B },
    { ImGuiKey_KeypadEnter, "Enter", "Enter", 0x00, 0x00 },
    { ImGuiKey_KeypadEqual, "=", "=", 0x3D, 0x3D },
    { ImGuiKey_LeftShift, "Shift", "Shift", 0x00, 0x00 },
    { ImGuiKey_LeftCtrl, "Control", "Control", 0x00, 0x00 },
    { ImGuiKey_LeftAlt, "Alt", "Alt", 0x00, 0x00 },
    { ImGuiKey_LeftSuper, "Meta", "Meta", 0x00, 0x00 },
    { ImGuiKey_RightShift, "Shift", "Shift", 0x00, 0x00 },
    { ImGuiKey_RightCtrl, "Control", "Control", 0x00, 0x00 },
    { ImGuiKey_RightAlt, "Alt", "Alt", 0x00, 0x00 },
    { ImGuiKey_RightSuper, "Meta", "Meta", 0x00, 0x00 },
    { ImGuiKey_Menu, "ContextMenu", "ContextMenu", 0x00, 0x00 },
    { ImGuiKey_0, "0", ")", 0x30, 0x29 },
    { ImGuiKey_1, "1", "!", 0x31, 0x21 },
    { ImGuiKey_2, "2", "@", 0x32, 0x40 },
    { ImGuiKey_3, "3", "#", 0x33, 0x23 },
    { ImGuiKey_4, "4", "$", 0x34, 0x24 },
    { ImGuiKey_5, "5", "%", 0x35, 0x25 },
    { ImGuiKey_6, "6", "^", 0x36, 0x5E },
    { ImGuiKey_7, "7", "&", 0x37, 0x26 },
    { ImGuiKey_8, "8", "*", 0x38, 0x2A },
    { ImGuiKey_9, "9", "(", 0x39, 0x28 },
    { ImGuiKey_A, "a", "A", 0x61, 0x41 },
    { ImGuiKey_B, "b", "B", 0x62, 0x42 },
    { ImGuiKey_C, "c", "C", 0x63, 0x43 },
    { ImGuiKey_D, "d", "D", 0x64, 0x44 },
    { ImGuiKey_E, "e", "E", 0x65, 0x45 },
    { ImGuiKey_F, "f", "F", 0x66, 0x46 },
    { ImGuiKey_G, "g", "G", 0x67, 0x47 },
    { ImGuiKey_H, "h", "H", 0x68, 0x48 },
    { ImGuiKey_I, "i", "I", 0x69, 0x49 },
    { ImGuiKey_J, "j", "J", 0x6A, 0x4A },
    { ImGuiKey_K, "k", "K", 0x6B, 0x4B },
    { ImGuiKey_L, "l", "L", 0x6C, 0x4C },
    { ImGuiKey_M, "m", "M", 0x6D, 0x4D },
    { ImGuiKey_N, "n", "N", 0x6E, 0x4E },
    { ImGuiKey_O, "o", "O", 0x6F, 0x4F },
    { ImGuiKey_P, "p", "P", 0x70, 0x50 },
    { ImGuiKey_Q, "q", "Q", 0x71, 0x51 },
    { ImGuiKey_R, "r", "R", 0x72, 0x52 },
    { ImGuiKey_S, "s", "S", 0x73, 0x53 },
    { ImGuiKey_T, "t", "T", 0x74, 0x54 },
    { ImGuiKey_U, "u", "U", 0x75, 0x55 },
    { ImGuiKey_V, "v", "V", 0x76, 0x56 },
    { ImGuiKey_W, "w", "W", 0x77, 0x57 },
    { ImGuiKey_X, "x", "X", 0x78, 0x58 },
    { ImGuiKey_Y, "y", "Y", 0x79, 0x59 },
    { ImGuiKey_Z, "z", "Z", 0x7A, 0x5A },
    { ImGuiKey_F1, "F1", "F1", 0x00, 0x00 },
    { ImGuiKey_F2, "F2", "F2", 0x00, 0x00 },
    { ImGuiKey_F3, "F3", "F3", 0x00, 0x00 },
    { ImGuiKey_F4, "F4", "F4", 0x00, 0x00 },
    { ImGuiKey_F5, "F5", "F5", 0x00, 0x00 },
    { ImGuiKey_F6, "F6", "F6", 0x00, 0x00 },
    { ImGuiKey_F7, "F7", "F7", 0x00, 0x00 },
    { ImGuiKey_F8, "F8", "F8", 0x00, 0x00 },
    { ImGuiKey_F9, "F9", "F9", 0x00, 0x00 },
    { ImGuiKey_F10, "F10", "F10", 0x00, 0x00 },
    { ImGuiKey_F11, "F11", "F11", 0x00, 0x00 },
    { ImGuiKey_F12, "F12", "F12", 0x00, 0x00 },
    { ImGuiKey_F13, "F13", "F13", 0x00, 0x00 },
    { ImGuiKey_F14, "F14", "F14", 0x00, 0x00 },
    { ImGuiKey_F15, "F15", "F15", 0x00, 0x00 },
    { ImGuiKey_F16, "F16", "F16", 0x00, 0x00 },
    { ImGuiKey_F17, "F17", "F17", 0x00, 0x00 },
    { ImGuiKey_F18, "F18", "F18", 0x00, 0x00 },
    { ImGuiKey_F19, "F19", "F19", 0x00, 0x00 },
    { ImGuiKey_F20, "F20", "F20", 0x00, 0x00 },
    { ImGuiKey_F21, "F21", "F21", 0x00, 0x00 },
    { ImGuiKey_F22, "F22", "F22", 0x00, 0x00 },
    { ImGuiKey_F23, "F23", "F23", 0x00, 0x00 },
    { ImGuiKey_F24, "F24", "F24", 0x00, 0x00 },
    { ImGuiKey_None, "", "", 0x00, 0x00 },
};

int button_to_imgui(int button)
{
    switch (button)
    {
    case 0:
        return 0;
    case 1:
        return 2;
    case 2:
        return 1;
    default:
        return button;
    }
}

void key_to_imgui(const std::string& key, int* key_imgui, uint32_t* unicode)
{
    key_map_t keymap = mappings[0u];
    uint32_t idx = 0u;
    while (keymap.key != ImGuiKey_None)
    {
        if (strcmp(keymap.lower, key.c_str()) == 0 || strcmp(keymap.upper, key.c_str()) == 0)
        {
            break;
        }
        idx++;
        keymap = mappings[idx];
    }
    // printf("key(%s) resolved to %d idx(%d)\n", key.c_str(), keymap.key, idx);
    *key_imgui = keymap.key;
    if (strcmp(keymap.upper, key.c_str()) == 0)
    {
        *unicode = keymap.upper_unicode;
    }
    else
    {
        *unicode = keymap.lower_unicode;
    }
}

int code_to_imgui(const std::string& code)
{
    return ImGuiKey_None;
}
}

pnanovdb_server_t* pnanovdb_get_server()
{
    static pnanovdb_server_t iface = { PNANOVDB_REFLECT_INTERFACE_INIT(pnanovdb_server_t) };

    iface.create_instance = create_instance;
    iface.push_h264 = push_h264;
    iface.pop_event = pop_event;
    iface.wait_until_active = wait_until_active;
    iface.destroy_instance = destroy_instance;

    return &iface;
}
