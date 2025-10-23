
// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   EncodeVulkanCPU.cpp

    \author Andrew Reidmeyer

    \brief  This file is part of the PNanoVDB Compute Vulkan reference implementation.
*/

#include "CommonVulkan.h"
#include "nanovdb_editor/putil/Compute.h"

#if defined(_WIN32)

#else
#    include <unistd.h>
#endif

#include <string.h>
#include <vector>
#include <stdio.h>

#include <wels/codec_api.h>
#include <wels/codec_app_def.h>
#include <wels/codec_def.h>

namespace pnanovdb_vulkan
{

struct EncoderCPU
{
    ISVCEncoder* encoder = nullptr;
};

pnanovdb_compute_encoder_t* create_encoder_cpu(pnanovdb_compute_queue_t* queue, const pnanovdb_compute_encoder_desc_t* desc)
{
    DeviceQueue* deviceQueue = cast(queue);
    Device* device = deviceQueue->device;
    Context* ctx = deviceQueue->context;

    auto ptr = new Encoder();
    ptr->encoderCPU = new EncoderCPU();

    printf("Created CPU Encoder!");

    return cast(ptr);
}

int present_encoder_cpu(pnanovdb_compute_encoder_t* encoder, pnanovdb_uint64_t* flushedFrameID)
{
    auto ptr = cast(encoder);

    int deviceReset = flushStepA(ptr->deviceQueue, nullptr, nullptr);

    flushStepB(ptr->deviceQueue);

    return deviceReset;
}

void destroy_encoder_cpu(pnanovdb_compute_encoder_t* encoder)
{
    auto ptr = cast(encoder);
    Device* device = ptr->deviceQueue->device;
    auto loader = &device->loader;

    delete ptr;
}

pnanovdb_compute_texture_t* get_encoder_front_texture_cpu(pnanovdb_compute_encoder_t* encoder)
{
    auto ptr = cast(encoder);

    return ptr->srcTexture;
}

void* map_encoder_data_cpu(pnanovdb_compute_encoder_t* encoder, pnanovdb_uint64_t* p_mapped_byte_count)
{
    auto ptr = cast(encoder);
    Device* device = ptr->deviceQueue->device;
    auto loader = &device->loader;

    loader->vkWaitForFences(device->vulkanDevice, 1u, &ptr->encodeFinishedFence, VK_TRUE, ~0llu);

    loader->vkResetCommandBuffer(ptr->commandBuffer, 0u);

    void* outData = nullptr;
    *p_mapped_byte_count = 0u;

    return outData;
}

void unmap_encoder_data_cpu(pnanovdb_compute_encoder_t* encoder)
{
    // nop
}

} // end namespace
