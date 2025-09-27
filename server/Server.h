// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/server/Server.h

    \author Andrew Reidmeyer

    \brief  This file provides an interface to a HTTP video streaming server.
*/

#ifndef NANOVDB_EDITOR_SERVER_H_HAS_BEEN_INCLUDED
#define NANOVDB_EDITOR_SERVER_H_HAS_BEEN_INCLUDED

#include "nanovdb_editor/putil/Loader.h"
#include "nanovdb_editor/putil/Reflect.h"
#include <stdio.h>

/// ********************************* Server Interface ***************************************

struct pnanovdb_server_instance_t;
typedef struct pnanovdb_server_instance_t pnanovdb_server_instance_t;

#define PNANOVDB_SERVER_EVENT_UNKNOWN 0
#define PNANOVDB_SERVER_EVENT_MOUSEMOVE 1
#define PNANOVDB_SERVER_EVENT_MOUSEDOWN 2
#define PNANOVDB_SERVER_EVENT_MOUSEUP 3
#define PNANOVDB_SERVER_EVENT_MOUSESCROLL 4
#define PNANOVDB_SERVER_EVENT_KEYDOWN 5
#define PNANOVDB_SERVER_EVENT_KEYUP 6
#define PNANOVDB_SERVER_EVENT_INACTIVE 7

typedef struct pnanovdb_server_event_t
{
    pnanovdb_uint32_t type;
    pnanovdb_int32_t button;
    float x;
    float y;
    float delta_x;
    float delta_y;
    pnanovdb_int32_t key;
    pnanovdb_uint32_t unicode;
    pnanovdb_int32_t code;
    pnanovdb_bool_t alt_key;
    pnanovdb_bool_t ctrl_key;
    pnanovdb_bool_t shift_key;
    pnanovdb_bool_t meta_key;
} pnanovdb_server_event_t;

typedef struct pnanovdb_server_t
{
    PNANOVDB_REFLECT_INTERFACE();

    pnanovdb_server_instance_t*(PNANOVDB_ABI* create_instance)(const char* serveraddress, int port);

    void(PNANOVDB_ABI* push_h264)(pnanovdb_server_instance_t* instance, const void* data, pnanovdb_uint64_t data_size);

    pnanovdb_bool_t(PNANOVDB_ABI* pop_event)(pnanovdb_server_instance_t* instance, pnanovdb_server_event_t* out_event);

    void(PNANOVDB_ABI* wait_until_active)(pnanovdb_server_instance_t* instance);

    void(PNANOVDB_ABI* destroy_instance)(pnanovdb_server_instance_t* instance);

} pnanovdb_server_t;

#define PNANOVDB_REFLECT_TYPE pnanovdb_server_t
PNANOVDB_REFLECT_BEGIN()
PNANOVDB_REFLECT_FUNCTION_POINTER(create_instance, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(push_h264, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(pop_event, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(wait_until_active, 0, 0)
PNANOVDB_REFLECT_FUNCTION_POINTER(destroy_instance, 0, 0)
PNANOVDB_REFLECT_END(0)
PNANOVDB_REFLECT_INTERFACE_IMPL()
#undef PNANOVDB_REFLECT_TYPE

typedef pnanovdb_server_t*(PNANOVDB_ABI* PFN_pnanovdb_get_server)();

PNANOVDB_API pnanovdb_server_t* pnanovdb_get_server();

#endif
