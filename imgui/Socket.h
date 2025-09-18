
// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   Socket.h

    \author Andrew Reidmeyer

    \brief  Simple socket implemenation.
*/

#pragma once

#ifdef _WIN32

struct pnanovdb_socket_t
{
    int unused;
};

static pnanovdb_socket_t* pnanovdb_socket_create(int port)
{
    auto ptr = new pnanovdb_socket_t();
    return ptr;
}

static void pnanovdb_socket_send(pnanovdb_socket_t* ptr, const void* data, size_t data_size)
{
    // NOP
}

static void pnanovdb_socket_destroy(pnanovdb_socket_t* ptr)
{
    delete ptr;
}

#else

#    include <sys/types.h>
#    include <sys/socket.h>
#    include <netinet/in.h>
#    include <arpa/inet.h>
#    include <stdio.h>
#    include <stdlib.h>
#    include <string.h>
#    include <unistd.h>

struct pnanovdb_socket_t
{
    int fd = -1;
    int dst_fd = -1;
    struct sockaddr_in sa = {};
};

static pnanovdb_socket_t* pnanovdb_socket_create(int port)
{
    auto ptr = new pnanovdb_socket_t();

    ptr->fd = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ptr->fd == -1)
    {
        printf("Failed to create socket\n");
        return nullptr;
    }

    ptr->sa.sin_family = AF_INET;
    ptr->sa.sin_port = htons(port);
    ptr->sa.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(ptr->fd, (struct sockaddr*)&ptr->sa, sizeof(ptr->sa)) == -1)
    {
        printf("Failed to bind socket at port(%d)\n", port);
        return nullptr;
    }
    if (listen(ptr->fd, 10) == -1)
    {
        printf("Failed to listen on socket with port(%d)\n", port);
        return nullptr;
    }

    printf("Socket waiting to accept connection on port(%d)!!!\n", port);
    ptr->dst_fd = accept(ptr->fd, nullptr, nullptr);
    printf("Accepted socket connection! fd(%d) dst_fd(%d)\n", ptr->fd, ptr->dst_fd);
    if (ptr->dst_fd == -1)
    {
        printf("Failed to accept on socket with port(%d)\n", port);
        return nullptr;
    }
    return ptr;
}

static void pnanovdb_socket_send(pnanovdb_socket_t* ptr, const void* data, size_t data_size)
{
    write(ptr->dst_fd, data, data_size);

    char buf[4096u];
    ssize_t res = 0u;
    do
    {
        res = recv(ptr->dst_fd, buf, 4096u, MSG_DONTWAIT);
    } while (res != 0 && res != -1);
}

static void pnanovdb_socket_destroy(pnanovdb_socket_t* ptr)
{
    if (ptr->dst_fd != -1)
    {
        close(ptr->dst_fd);
    }
    if (ptr->fd != -1)
    {
        close(ptr->fd);
    }
    delete ptr;
}

#endif