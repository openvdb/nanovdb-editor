
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
    pnanovdb_compute_texture_t* target_texture = nullptr;

    ISVCEncoder* encoder = nullptr;
};

pnanovdb_compute_encoder_t* create_encoder_cpu(pnanovdb_compute_queue_t* queue, const pnanovdb_compute_encoder_desc_t* desc)
{
    DeviceQueue* deviceQueue = cast(queue);
    Context* ctx = deviceQueue->context;

    auto ptr = new Encoder();
    ptr->encoderCPU = new EncoderCPU();

    ptr->deviceQueue = deviceQueue;

    ptr->desc = *desc;
    ptr->width = ptr->desc.width;
    ptr->height = ptr->desc.height;

    pnanovdb_compute_texture_desc_t tex_desc = {};
    tex_desc.texture_type = PNANOVDB_COMPUTE_TEXTURE_TYPE_2D;
    tex_desc.usage = PNANOVDB_COMPUTE_TEXTURE_USAGE_TEXTURE | PNANOVDB_COMPUTE_TEXTURE_USAGE_RW_TEXTURE;
    tex_desc.format = PNANOVDB_COMPUTE_FORMAT_R8G8B8A8_UNORM;
    tex_desc.width = desc->width;
    tex_desc.height = desc->height;
    tex_desc.depth = 1u;
    tex_desc.mip_levels = 1u;
    ptr->encoderCPU->target_texture = createTexture(cast(ctx), &tex_desc);

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
    DeviceQueue* deviceQueue = ptr->deviceQueue;
    Context* ctx = deviceQueue->context;

    destroyTexture(cast(ctx), ptr->encoderCPU->target_texture);

    delete ptr;
}

pnanovdb_compute_texture_t* get_encoder_front_texture_cpu(pnanovdb_compute_encoder_t* encoder)
{
    auto ptr = cast(encoder);

    return ptr->encoderCPU->target_texture;
}

void* map_encoder_data_cpu(pnanovdb_compute_encoder_t* encoder, pnanovdb_uint64_t* p_mapped_byte_count)
{
    auto ptr = cast(encoder);

    waitIdle(cast(ptr->deviceQueue));

    void* outData = nullptr;
    *p_mapped_byte_count = 0u;

    return outData;
}

void unmap_encoder_data_cpu(pnanovdb_compute_encoder_t* encoder)
{
    // nop
}

} // end namespace
