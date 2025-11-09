
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
    pnanovdb_compute_buffer_t* constant_buffer = nullptr;
    pnanovdb_compute_buffer_t* device_buffer = nullptr;
    pnanovdb_compute_buffer_t* readback_buffer = nullptr;

    pnanovdb_compiler_t compiler = {};
    pnanovdb_compute_t compute = {};

    pnanovdb_shader_context_t* shader_context = nullptr;

    ISVCEncoder* openh264_encoder = nullptr;

    std::vector<uint8_t> bitstream;
};

pnanovdb_compute_encoder_t* create_encoder_cpu(pnanovdb_compute_queue_t* queue, const pnanovdb_compute_encoder_desc_t* desc)
{
    DeviceQueue* deviceQueue = cast(queue);
    Context* ctx = deviceQueue->context;

    Device* device = deviceQueue->device;
    auto loader = &device->loader;

    auto ptr = new Encoder();
    ptr->encoderCPU = new EncoderCPU();

    ptr->deviceQueue = deviceQueue;

    ptr->desc = *desc;
    ptr->width = ptr->desc.width;
    ptr->height = ptr->desc.height;

    {
        VkImageCreateInfo createInfo = {};
        createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        createInfo.imageType = VK_IMAGE_TYPE_2D;
        createInfo.format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
        createInfo.extent = { ptr->width, ptr->height, 1u };
        createInfo.mipLevels = 1u;
        createInfo.arrayLayers = 1u;
        createInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        createInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        createInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
        createInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        loader->vkCreateImage(device->vulkanDevice, &createInfo, nullptr, &ptr->srcImage);

        VkMemoryRequirements texMemReq = {};
        loader->vkGetImageMemoryRequirements(device->vulkanDevice, ptr->srcImage, &texMemReq);
        uint32_t texMemType_device =
            context_getMemoryType(ctx, texMemReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VkMemoryAllocateInfo texMemAllocInfo = {};
        texMemAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        texMemAllocInfo.allocationSize = texMemReq.size;
        texMemAllocInfo.memoryTypeIndex = texMemType_device;

        loader->vkAllocateMemory(device->vulkanDevice, &texMemAllocInfo, nullptr, &ptr->srcMemory);
        loader->vkBindImageMemory(device->vulkanDevice, ptr->srcImage, ptr->srcMemory, 0u);

        pnanovdb_compute_texture_desc_t texDesc = {};
        texDesc.texture_type = PNANOVDB_COMPUTE_TEXTURE_TYPE_2D;
        texDesc.usage = PNANOVDB_COMPUTE_TEXTURE_USAGE_RW_TEXTURE | PNANOVDB_COMPUTE_TEXTURE_USAGE_TEXTURE;
        texDesc.format = PNANOVDB_COMPUTE_FORMAT_G8_B8R8_2PLANE_420_UNORM;
        texDesc.width = ptr->width;
        texDesc.height = ptr->height;

        ptr->srcTexture = createTextureExternal(cast(ctx), &texDesc, ptr->srcImage, VK_IMAGE_LAYOUT_GENERAL);
    }

    struct constants_t
    {
        pnanovdb_uint32_t width;
        pnanovdb_uint32_t height;
        pnanovdb_uint32_t pad2;
        pnanovdb_uint32_t pad1;
    };
    constants_t constants = {};
    constants.width = desc->width;
    constants.height = desc->height;

    pnanovdb_compute_buffer_desc_t buf_desc = {};

    buf_desc.usage = PNANOVDB_COMPUTE_BUFFER_USAGE_CONSTANT;
    buf_desc.format = PNANOVDB_COMPUTE_FORMAT_UNKNOWN;
    buf_desc.structure_stride = 0u;
    buf_desc.size_in_bytes = sizeof(constants_t);
    ptr->encoderCPU->constant_buffer = createBuffer(cast(ctx), PNANOVDB_COMPUTE_MEMORY_TYPE_UPLOAD, &buf_desc);

    void* mapped_constants = mapBuffer(cast(ctx), ptr->encoderCPU->constant_buffer);
    memcpy(mapped_constants, &constants, sizeof(constants_t));
    unmapBuffer(cast(ctx), ptr->encoderCPU->constant_buffer);

    pnanovdb_uint64_t buf_size = ptr->width * ptr->height +
        2u * (ptr->width / 2u) * (ptr->height / 2u);

    buf_desc.usage = PNANOVDB_COMPUTE_BUFFER_USAGE_RW_STRUCTURED | PNANOVDB_COMPUTE_BUFFER_USAGE_COPY_SRC;
    buf_desc.format = PNANOVDB_COMPUTE_FORMAT_UNKNOWN;
    buf_desc.structure_stride = 4u;
    buf_desc.size_in_bytes = buf_size;
    ptr->encoderCPU->device_buffer = createBuffer(cast(ctx), PNANOVDB_COMPUTE_MEMORY_TYPE_DEVICE, &buf_desc);

    buf_desc.usage = PNANOVDB_COMPUTE_BUFFER_USAGE_COPY_DST;
    buf_desc.format = PNANOVDB_COMPUTE_FORMAT_UNKNOWN;
    buf_desc.structure_stride = 4u;
    buf_desc.size_in_bytes = buf_size;
    ptr->encoderCPU->readback_buffer = createBuffer(cast(ctx), PNANOVDB_COMPUTE_MEMORY_TYPE_READBACK, &buf_desc);

    // for now, load compiler and compute to do dynamic compilation
    pnanovdb_compiler_load(&ptr->encoderCPU->compiler);
    pnanovdb_compute_load(&ptr->encoderCPU->compute, &ptr->encoderCPU->compiler);

    pnanovdb_compiler_settings_t compile_settings = {};
    pnanovdb_compiler_settings_init(&compile_settings);

    ptr->encoderCPU->shader_context = ptr->encoderCPU->compute.create_shader_context("raster/copy_texture_to_buffer.slang");
    ptr->encoderCPU->compute.init_shader(&ptr->encoderCPU->compute, queue, ptr->encoderCPU->shader_context, &compile_settings);

    // Create encoder
    ISVCEncoder* openh264_encoder = nullptr;
    int rv = WelsCreateSVCEncoder(&openh264_encoder);
    if (rv != 0 || !openh264_encoder)
    {
        printf("Error: Failed to create OpenH264 encoder\n");
        return nullptr;
    }

    // scale bitrate with resolution
    uint64_t pixel_count = ptr->width * ptr->height;

    uint64_t ave_bits_per_pixel = 5; // 5 Mbps to 20 Mbps at 1440x720
    if (pixel_count >= 3840 * 2160) // 25 Mbps to 83 Mbps
    {
        ave_bits_per_pixel = 3;
    }
    else if (pixel_count >= 2560 * 1440) // 15 Mbps to 52 Mbps
    {
        ave_bits_per_pixel = 4;
    }
    else if (pixel_count >= 1920 * 1080) // 10 Mbps to 35 Mbps
    {
        ave_bits_per_pixel = 5;
    }

    uint64_t ave_bitrate = ave_bits_per_pixel * pixel_count;

    // Set encoding parameters
    SEncParamExt param;
    memset(&param, 0, sizeof(SEncParamExt));
    openh264_encoder->GetDefaultParams(&param);

    param.iUsageType = CAMERA_VIDEO_REAL_TIME;
    param.fMaxFrameRate = 30.0f;
    param.iPicWidth = ptr->width;
    param.iPicHeight = ptr->height;
    param.iTargetBitrate = ave_bitrate;
    param.iRCMode = RC_QUALITY_MODE;
    param.iTemporalLayerNum = 1;
    param.iSpatialLayerNum = 1;
    param.bEnableDenoise = false;
    param.bEnableBackgroundDetection = true;
    param.bEnableAdaptiveQuant = true;
    param.bEnableFrameSkip = true;
    param.bEnableLongTermReference = false;
    param.iLtrMarkPeriod = 30;
    param.uiIntraPeriod = 320;
    param.eSpsPpsIdStrategy = INCREASING_ID;
    param.bPrefixNalAddingCtrl = false;
    param.iComplexityMode = LOW_COMPLEXITY;
    param.bSimulcastAVC = false;

    param.sSpatialLayers[0].iVideoWidth = param.iPicWidth;
    param.sSpatialLayers[0].iVideoHeight = param.iPicHeight;
    param.sSpatialLayers[0].fFrameRate = param.fMaxFrameRate;
    param.sSpatialLayers[0].iSpatialBitrate = param.iTargetBitrate;
    param.sSpatialLayers[0].iMaxSpatialBitrate = param.iTargetBitrate;
    param.sSpatialLayers[0].uiProfileIdc = PRO_BASELINE;
    param.sSpatialLayers[0].uiLevelIdc = LEVEL_5_1;
    param.sSpatialLayers[0].iDLayerQp = 24;
    param.sSpatialLayers[0].sSliceArgument.uiSliceMode = SM_SINGLE_SLICE;

    // Initialize encoder
    rv = openh264_encoder->InitializeExt(&param);
    if (rv != cmResultSuccess)
    {
        printf("Error: Failed to initialize OpenH264 encoder (code: %d)\n", rv);
        WelsDestroySVCEncoder(openh264_encoder);
        return nullptr;
    }

    ptr->encoderCPU->openh264_encoder = openh264_encoder;

    return cast(ptr);
}

int present_encoder_cpu(pnanovdb_compute_encoder_t* encoder, pnanovdb_uint64_t* flushedFrameID)
{
    auto ptr = cast(encoder);

    {
        pnanovdb_compute_context_t* context = cast(ptr->deviceQueue->context);
        pnanovdb_compute_interface_t* context_iface = getContextInterface(cast(ptr->deviceQueue));

        pnanovdb_compute_texture_transient_t* tex_transient = registerTextureAsTransient(context, ptr->srcTexture);
        pnanovdb_compute_texture_transient_t* tex_plane0 = aliasTextureTransient(
            context, tex_transient, PNANOVDB_COMPUTE_FORMAT_R8_UNORM, PNANOVDB_COMPUTE_TEXTURE_ASPECT_PLANE_0);
        pnanovdb_compute_texture_transient_t* tex_plane1 = aliasTextureTransient(
            context, tex_transient, PNANOVDB_COMPUTE_FORMAT_R8G8_UNORM, PNANOVDB_COMPUTE_TEXTURE_ASPECT_PLANE_1);

        pnanovdb_compute_resource_t resources[4u] = {};
        resources[0u].buffer_transient = registerBufferAsTransient(context, ptr->encoderCPU->constant_buffer);
        resources[1u].texture_transient = tex_plane0;
        resources[2u].texture_transient = tex_plane1;
        resources[3u].buffer_transient = registerBufferAsTransient(context, ptr->encoderCPU->device_buffer);

        pnanovdb_uint32_t grid_dim_x = (ptr->width + 15u) / 16u;
        pnanovdb_uint32_t grid_dim_y = (ptr->height + 15u) / 16u;

        ptr->encoderCPU->compute.dispatch_shader(
            context_iface, context,
            ptr->encoderCPU->shader_context, resources,
            grid_dim_x, grid_dim_y, 1u, "copy_texture_to_buffer");

        pnanovdb_uint64_t buf_size = ptr->width * ptr->height +
            2u * (ptr->width / 2u) * (ptr->height / 2u);

        pnanovdb_compute_copy_buffer_params_t copy_params = {};
        copy_params.num_bytes = buf_size;
        copy_params.src = resources[3u].buffer_transient;
        copy_params.dst = registerBufferAsTransient(context, ptr->encoderCPU->readback_buffer);
        copy_params.debug_label = "copy_cpu_encoder_buffer";

        addPassCopyBuffer(context, &copy_params);
    }

    int deviceReset = flushStepA(ptr->deviceQueue, nullptr, nullptr);

    flushStepB(ptr->deviceQueue);

    return deviceReset;
}

void destroy_encoder_cpu(pnanovdb_compute_encoder_t* encoder)
{
    auto ptr = cast(encoder);
    DeviceQueue* deviceQueue = ptr->deviceQueue;
    Context* ctx = deviceQueue->context;

    Device* device = deviceQueue->device;
    auto loader = &device->loader;

    destroyTexture(cast(ptr->deviceQueue->context), ptr->srcTexture);
    loader->vkDestroyImage(device->vulkanDevice, ptr->srcImage, nullptr);
    loader->vkFreeMemory(device->vulkanDevice, ptr->srcMemory, nullptr);

    destroyBuffer(cast(ctx), ptr->encoderCPU->constant_buffer);
    destroyBuffer(cast(ctx), ptr->encoderCPU->device_buffer);
    destroyBuffer(cast(ctx), ptr->encoderCPU->readback_buffer);

    ptr->encoderCPU->compute.destroy_shader_context(
        &ptr->encoderCPU->compute, cast(deviceQueue), ptr->encoderCPU->shader_context);

    pnanovdb_compute_free(&ptr->encoderCPU->compute);
    pnanovdb_compiler_free(&ptr->encoderCPU->compiler);

    auto openh264_encoder = ptr->encoderCPU->openh264_encoder;
    openh264_encoder->Uninitialize();
    WelsDestroySVCEncoder(openh264_encoder);

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
    DeviceQueue* deviceQueue = ptr->deviceQueue;
    Context* ctx = deviceQueue->context;

    waitIdle(cast(deviceQueue));

    unsigned char* mapped_readback = (unsigned char*)mapBuffer(cast(ctx), ptr->encoderCPU->readback_buffer);

    auto openh264_encoder = ptr->encoderCPU->openh264_encoder;

    // Prepare source picture
    SSourcePicture sourcePicture;
    memset(&sourcePicture, 0, sizeof(SSourcePicture));
    sourcePicture.iPicWidth = ptr->width;
    sourcePicture.iPicHeight = ptr->height;
    sourcePicture.iColorFormat = videoFormatI420;
    sourcePicture.iStride[0] = ptr->width;
    sourcePicture.iStride[1] = ptr->width / 2;
    sourcePicture.iStride[2] = ptr->width / 2;
    sourcePicture.pData[0] = mapped_readback;
    sourcePicture.pData[1] = mapped_readback + (ptr->width * ptr->height);
    sourcePicture.pData[2] = mapped_readback + (ptr->width * ptr->height) + ((ptr->width / 2u) * (ptr->height/ 2u));

    // Encode frame
    SFrameBSInfo frameInfo;
    memset(&frameInfo, 0, sizeof(SFrameBSInfo));

    int rv = openh264_encoder->EncodeFrame(&sourcePicture, &frameInfo);
    if (rv != cmResultSuccess)
    {
        printf("Error: Failed to encode frame (code: %d)\n", rv);
    }

    ptr->encoderCPU->bitstream.clear();
    for (int i = 0; i < frameInfo.iLayerNum; i++)
    {
        int copy_size = 0;
        for (int j = 0; j < frameInfo.sLayerInfo[i].iNalCount; j++)
        {
            copy_size += frameInfo.sLayerInfo[i].pNalLengthInByte[j];
        }
        size_t offset = ptr->encoderCPU->bitstream.size();
        ptr->encoderCPU->bitstream.resize(offset + copy_size);
        memcpy(ptr->encoderCPU->bitstream.data() + offset, frameInfo.sLayerInfo[i].pBsBuf, copy_size);
    }

    unmapBuffer(cast(ctx), ptr->encoderCPU->readback_buffer);

    void* outData = ptr->encoderCPU->bitstream.data();
    *p_mapped_byte_count = ptr->encoderCPU->bitstream.size();

    return outData;
}

void unmap_encoder_data_cpu(pnanovdb_compute_encoder_t* encoder)
{
    // nop
}

} // end namespace
