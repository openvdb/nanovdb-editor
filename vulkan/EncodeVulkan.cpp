
// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   EncodeVulkan.cpp

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

namespace pnanovdb_vulkan
{

pnanovdb_compute_encoder_t* create_encoder(pnanovdb_compute_queue_t* queue, const pnanovdb_compute_encoder_desc_t* desc)
{
    DeviceQueue* deviceQueue = cast(queue);
    Device* device = deviceQueue->device;
    Context* ctx = deviceQueue->context;

    if (!device->enabledExtensions.VK_KHR_VIDEO_QUEUE || !device->enabledExtensions.VK_KHR_VIDEO_ENCODE_QUEUE)
    {
        device->logPrint(PNANOVDB_COMPUTE_LOG_LEVEL_WARNING, "Vulkan Video Encode is not supported.");
        return nullptr;
    }

    auto ptr = new Encoder();

    ptr->deviceQueue = deviceQueue;

    ptr->desc = *desc;
    ptr->width = ptr->desc.width;
    ptr->height = ptr->desc.height;

    auto loader = &device->loader;

    VkCommandPoolCreateInfo poolCreateInfo = {};
    poolCreateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolCreateInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolCreateInfo.queueFamilyIndex = device->encodeQueueFamilyIdx;

    loader->vkCreateCommandPool(device->vulkanDevice, &poolCreateInfo, nullptr, &ptr->commandPool);

    VkVideoEncodeH264ProfileInfoKHR encodeH264ProfileInfoExt = {};
    encodeH264ProfileInfoExt.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_PROFILE_INFO_KHR;
    encodeH264ProfileInfoExt.stdProfileIdc = STD_VIDEO_H264_PROFILE_IDC_MAIN;

    VkVideoProfileInfoKHR videoProfile = {};
    videoProfile.sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR;
    videoProfile.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR;
    videoProfile.chromaSubsampling = VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR;
    videoProfile.chromaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
    videoProfile.lumaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
    videoProfile.pNext = &encodeH264ProfileInfoExt;

    VkVideoProfileListInfoKHR videoProfileList = {};
    videoProfileList.sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR;
    videoProfileList.profileCount = 1u;
    videoProfileList.pProfiles = &videoProfile;

    VkVideoEncodeH264CapabilitiesKHR h264capabilities = {};
    h264capabilities.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_CAPABILITIES_KHR;

    VkVideoEncodeCapabilitiesKHR encodeCapabilities = {};
    encodeCapabilities.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_CAPABILITIES_KHR;
    encodeCapabilities.pNext = &h264capabilities;

    VkVideoCapabilitiesKHR capabilities = {};
    capabilities.sType = VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR;
    capabilities.pNext = &encodeCapabilities;

    device->deviceManager->loader.vkGetPhysicalDeviceVideoCapabilitiesKHR(
        device->physicalDevice, &videoProfile, &capabilities);

    VkVideoEncodeRateControlModeFlagBitsKHR chosenRateControlMode = VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DEFAULT_KHR;
    if (encodeCapabilities.rateControlModes & VK_VIDEO_ENCODE_RATE_CONTROL_MODE_VBR_BIT_KHR)
    {
        chosenRateControlMode = VK_VIDEO_ENCODE_RATE_CONTROL_MODE_VBR_BIT_KHR;
    }
    else if (encodeCapabilities.rateControlModes & VK_VIDEO_ENCODE_RATE_CONTROL_MODE_CBR_BIT_KHR)
    {
        chosenRateControlMode = VK_VIDEO_ENCODE_RATE_CONTROL_MODE_CBR_BIT_KHR;
    }
    else if (encodeCapabilities.rateControlModes & VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DISABLED_BIT_KHR)
    {
        chosenRateControlMode = VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DISABLED_BIT_KHR;
    }
    ptr->chosenRateControlMode = chosenRateControlMode;

    VkPhysicalDeviceVideoEncodeQualityLevelInfoKHR qualityLevelInfo = {};
    qualityLevelInfo.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_ENCODE_QUALITY_LEVEL_INFO_KHR;
    qualityLevelInfo.pVideoProfile = &videoProfile;
    qualityLevelInfo.qualityLevel = 0;

    VkVideoEncodeH264QualityLevelPropertiesKHR h264QualityLevelProperties = {};
    h264QualityLevelProperties.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_QUALITY_LEVEL_PROPERTIES_KHR;
    VkVideoEncodeQualityLevelPropertiesKHR qualityLevelProperties = {};
    qualityLevelProperties.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_QUALITY_LEVEL_PROPERTIES_KHR;
    qualityLevelProperties.pNext = &h264QualityLevelProperties;

    device->deviceManager->loader.vkGetPhysicalDeviceVideoEncodeQualityLevelPropertiesKHR(
        device->physicalDevice, &qualityLevelInfo, &qualityLevelProperties);

    VkPhysicalDeviceVideoFormatInfoKHR videoFormatInfo = {};
    videoFormatInfo.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_FORMAT_INFO_KHR;
    videoFormatInfo.pNext = &videoProfileList;
    videoFormatInfo.imageUsage = VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    uint32_t videoFormatPropertyCount = 0u;
    device->deviceManager->loader.vkGetPhysicalDeviceVideoFormatPropertiesKHR(
        device->physicalDevice, &videoFormatInfo, &videoFormatPropertyCount, nullptr);

    std::vector<VkVideoFormatPropertiesKHR> srcVideoFormatProperties(videoFormatPropertyCount);
    for (uint32_t i = 0; i < videoFormatPropertyCount; i++)
    {
        VkVideoFormatPropertiesKHR prop = {};
        prop.sType = VK_STRUCTURE_TYPE_VIDEO_FORMAT_PROPERTIES_KHR;
        srcVideoFormatProperties[i] = prop;
    }
    device->deviceManager->loader.vkGetPhysicalDeviceVideoFormatPropertiesKHR(
        device->physicalDevice, &videoFormatInfo, &videoFormatPropertyCount, srcVideoFormatProperties.data());

    VkFormat chosenSrcImageFormat = VK_FORMAT_UNDEFINED;
    for (uint32_t i = 0; i < videoFormatPropertyCount; i++)
    {
        VkFormat format = srcVideoFormatProperties[i].format;
        if (format == VK_FORMAT_G8_B8R8_2PLANE_420_UNORM || format == VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM)
        {
            chosenSrcImageFormat = format;
            break;
        }
    }
    if (chosenSrcImageFormat == VK_FORMAT_UNDEFINED)
    {
        if (ctx->logPrint)
        {
            ctx->logPrint(PNANOVDB_COMPUTE_LOG_LEVEL_ERROR, "No supported video encode source image format");
        }
        return nullptr;
    }

    videoFormatInfo.imageUsage = VK_IMAGE_USAGE_VIDEO_ENCODE_DPB_BIT_KHR;
    device->deviceManager->loader.vkGetPhysicalDeviceVideoFormatPropertiesKHR(
        device->physicalDevice, &videoFormatInfo, &videoFormatPropertyCount, nullptr);
    std::vector<VkVideoFormatPropertiesKHR> dpbVideoFormatProperties(videoFormatPropertyCount);
    for (uint32_t i = 0; i < videoFormatPropertyCount; i++)
    {
        VkVideoFormatPropertiesKHR prop = {};
        prop.sType = VK_STRUCTURE_TYPE_VIDEO_FORMAT_PROPERTIES_KHR;
        dpbVideoFormatProperties[i] = prop;
    }
    device->deviceManager->loader.vkGetPhysicalDeviceVideoFormatPropertiesKHR(
        device->physicalDevice, &videoFormatInfo, &videoFormatPropertyCount, dpbVideoFormatProperties.data());
    if (dpbVideoFormatProperties.size() == 0u)
    {
        if (ctx->logPrint)
        {
            ctx->logPrint(PNANOVDB_COMPUTE_LOG_LEVEL_ERROR, "No supported video encode DPB image format");
        }
        return nullptr;
    }
    VkFormat chosenDpbImageFormat = dpbVideoFormatProperties[0].format;

    static const VkExtensionProperties h264StdExtensionVersion = { VK_STD_VULKAN_VIDEO_CODEC_H264_ENCODE_EXTENSION_NAME,
                                                                   VK_STD_VULKAN_VIDEO_CODEC_H264_ENCODE_SPEC_VERSION };
    VkVideoSessionCreateInfoKHR createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_CREATE_INFO_KHR;
    createInfo.pVideoProfile = &videoProfile;
    createInfo.queueFamilyIndex = device->encodeQueueFamilyIdx;
    createInfo.pictureFormat = chosenSrcImageFormat;
    createInfo.maxCodedExtent = { ptr->width, ptr->height };
    createInfo.maxDpbSlots = 16u;
    createInfo.maxActiveReferencePictures = 16u;
    createInfo.referencePictureFormat = chosenDpbImageFormat;
    createInfo.pStdHeaderVersion = &h264StdExtensionVersion;

    loader->vkCreateVideoSessionKHR(device->vulkanDevice, &createInfo, nullptr, &ptr->videoSession);

    uint32_t videoSessionMemoryRequirementsCount = 0u;
    loader->vkGetVideoSessionMemoryRequirementsKHR(
        device->vulkanDevice, ptr->videoSession, &videoSessionMemoryRequirementsCount, nullptr);
    std::vector<VkVideoSessionMemoryRequirementsKHR> encodeSessionMemoryRequirements(videoSessionMemoryRequirementsCount);
    for (uint32_t i = 0; i < videoSessionMemoryRequirementsCount; i++)
    {
        VkVideoSessionMemoryRequirementsKHR reqs = {};
        reqs.sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_MEMORY_REQUIREMENTS_KHR;
        encodeSessionMemoryRequirements[i] = reqs;
    }
    loader->vkGetVideoSessionMemoryRequirementsKHR(device->vulkanDevice, ptr->videoSession,
                                                   &videoSessionMemoryRequirementsCount,
                                                   encodeSessionMemoryRequirements.data());
    if (videoSessionMemoryRequirementsCount == 0u)
    {
        if (ctx->logPrint)
        {
            ctx->logPrint(PNANOVDB_COMPUTE_LOG_LEVEL_ERROR, "Failed to get video session memory requirements");
        }
        return nullptr;
    }

    ptr->memories.resize(videoSessionMemoryRequirementsCount);
    std::vector<VkBindVideoSessionMemoryInfoKHR> encodeSessionBindMemory(videoSessionMemoryRequirementsCount);
    for (uint32_t memIdx = 0u; memIdx < videoSessionMemoryRequirementsCount; memIdx++)
    {
        uint32_t memTypeBits = encodeSessionMemoryRequirements[memIdx].memoryRequirements.memoryTypeBits;
        uint32_t memTypeIdx = context_getMemoryType(ctx, memTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (memTypeIdx == ~0u)
        {
            if (ctx->logPrint)
            {
                ctx->logPrint(PNANOVDB_COMPUTE_LOG_LEVEL_DEBUG, "Encode memory doesn't support device local");
            }
            memTypeIdx = context_getMemoryType(ctx, memTypeBits, 0);
        }
        if (ctx->logPrint)
        {
            ctx->logPrint(PNANOVDB_COMPUTE_LOG_LEVEL_DEBUG, "Encode memory[%d] memTypeBits(%d) memTypeIdx(%d)", memIdx,
                          memTypeBits, memTypeIdx);
        }

        VkMemoryAllocateInfo allocInfo = {};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = encodeSessionMemoryRequirements[memIdx].memoryRequirements.size;
        allocInfo.memoryTypeIndex = memTypeIdx;

        loader->vkAllocateMemory(device->vulkanDevice, &allocInfo, nullptr, &ptr->memories[memIdx]);

        encodeSessionBindMemory[memIdx].sType = VK_STRUCTURE_TYPE_BIND_VIDEO_SESSION_MEMORY_INFO_KHR;
        encodeSessionBindMemory[memIdx].pNext = nullptr;
        encodeSessionBindMemory[memIdx].memory = ptr->memories[memIdx];
        encodeSessionBindMemory[memIdx].memoryBindIndex = encodeSessionMemoryRequirements[memIdx].memoryBindIndex;
        encodeSessionBindMemory[memIdx].memoryOffset = 0u;
        encodeSessionBindMemory[memIdx].memorySize = allocInfo.allocationSize;
    }
    loader->vkBindVideoSessionMemoryKHR(
        device->vulkanDevice, ptr->videoSession, videoSessionMemoryRequirementsCount, encodeSessionBindMemory.data());

    uint32_t fps = ptr->desc.fps;

    // vui
    StdVideoH264SequenceParameterSetVui vui = {};
    {
        StdVideoH264SpsVuiFlags vuiFlags = {};
        vuiFlags.timing_info_present_flag = 1u;
        vuiFlags.fixed_frame_rate_flag = 1u;

        vui.flags = vuiFlags;
        vui.num_units_in_tick = 1;
        vui.time_scale = fps * 2; // 2 fields
    }
    // sps
    StdVideoH264SequenceParameterSet sps = {};
    {
        StdVideoH264SpsFlags spsFlags = {};
        spsFlags.direct_8x8_inference_flag = 1u;
        spsFlags.frame_mbs_only_flag = 1u;
        spsFlags.vui_parameters_present_flag = 1u;

        const uint32_t alignedWidth = (ptr->width + 15) & ~15;
        const uint32_t alignedHeight = (ptr->height + 15) & ~15;

        sps.profile_idc = STD_VIDEO_H264_PROFILE_IDC_MAIN;
        sps.level_idc = STD_VIDEO_H264_LEVEL_IDC_4_1;
        sps.seq_parameter_set_id = 0u;
        sps.chroma_format_idc = STD_VIDEO_H264_CHROMA_FORMAT_IDC_420;
        sps.bit_depth_luma_minus8 = 0u;
        sps.bit_depth_chroma_minus8 = 0u;
        sps.log2_max_frame_num_minus4 = 0u;
        sps.pic_order_cnt_type = STD_VIDEO_H264_POC_TYPE_0;
        sps.max_num_ref_frames = 1u;
        sps.pic_width_in_mbs_minus1 = (alignedWidth / 16) - 1;
        sps.pic_height_in_map_units_minus1 = (alignedHeight / 16) - 1;
        sps.flags = spsFlags;
        sps.pSequenceParameterSetVui = &vui;
        sps.frame_crop_right_offset = alignedWidth - ptr->width;
        sps.frame_crop_bottom_offset = alignedHeight - ptr->height;

        sps.log2_max_pic_order_cnt_lsb_minus4 = 4u;

        if (sps.frame_crop_right_offset || sps.frame_crop_bottom_offset)
        {
            sps.flags.frame_cropping_flag = true;
            if (sps.chroma_format_idc == STD_VIDEO_H264_CHROMA_FORMAT_IDC_420)
            {
                sps.frame_crop_right_offset >>= 1;
                sps.frame_crop_bottom_offset >>= 1;
            }
        }
    }
    // pps
    StdVideoH264PictureParameterSet pps = {};
    {
        StdVideoH264PpsFlags ppsFlags = {};
        ppsFlags.transform_8x8_mode_flag = 0u;
        ppsFlags.constrained_intra_pred_flag = 0u;
        ppsFlags.deblocking_filter_control_present_flag = 1u;
        ppsFlags.entropy_coding_mode_flag = 1u;

        pps.seq_parameter_set_id = 0u;
        pps.pic_parameter_set_id = 0u;
        pps.num_ref_idx_l0_default_active_minus1 = 0u;
        pps.flags = ppsFlags;
    }

    VkVideoEncodeH264SessionParametersAddInfoKHR encodeH264SessionParametersAddInfo = {};
    encodeH264SessionParametersAddInfo.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_SESSION_PARAMETERS_ADD_INFO_KHR;
    encodeH264SessionParametersAddInfo.pNext = nullptr;
    encodeH264SessionParametersAddInfo.stdSPSCount = 1u;
    encodeH264SessionParametersAddInfo.pStdSPSs = &sps;
    encodeH264SessionParametersAddInfo.stdPPSCount = 1u;
    encodeH264SessionParametersAddInfo.pStdPPSs = &pps;

    VkVideoEncodeH264SessionParametersCreateInfoKHR encodeH264SessionParametersCreateInfo = {};
    encodeH264SessionParametersCreateInfo.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_SESSION_PARAMETERS_CREATE_INFO_KHR;
    encodeH264SessionParametersCreateInfo.pNext = nullptr;
    encodeH264SessionParametersCreateInfo.maxStdSPSCount = 1u;
    encodeH264SessionParametersCreateInfo.maxStdPPSCount = 1u;
    encodeH264SessionParametersCreateInfo.pParametersAddInfo = &encodeH264SessionParametersAddInfo;

    VkVideoSessionParametersCreateInfoKHR sessionParametersCreateInfo = {};
    sessionParametersCreateInfo.sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_PARAMETERS_CREATE_INFO_KHR;
    sessionParametersCreateInfo.pNext = &encodeH264SessionParametersCreateInfo;
    sessionParametersCreateInfo.videoSessionParametersTemplate = nullptr;
    sessionParametersCreateInfo.videoSession = ptr->videoSession;

    loader->vkCreateVideoSessionParametersKHR(
        device->vulkanDevice, &sessionParametersCreateInfo, nullptr, &ptr->videoSessionParameters);

    VkVideoEncodeH264SessionParametersGetInfoKHR h264getInfo = {};
    h264getInfo.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_SESSION_PARAMETERS_GET_INFO_KHR;
    h264getInfo.stdSPSId = 0;
    h264getInfo.stdPPSId = 0;
    h264getInfo.writeStdPPS = VK_TRUE;
    h264getInfo.writeStdSPS = VK_TRUE;
    VkVideoEncodeSessionParametersGetInfoKHR getInfo = {};
    getInfo.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_SESSION_PARAMETERS_GET_INFO_KHR;
    getInfo.pNext = &h264getInfo;
    getInfo.videoSessionParameters = ptr->videoSessionParameters;

    VkVideoEncodeH264SessionParametersFeedbackInfoKHR h264feedback = {};
    h264feedback.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_SESSION_PARAMETERS_FEEDBACK_INFO_KHR;
    VkVideoEncodeSessionParametersFeedbackInfoKHR feedback = {};
    feedback.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_SESSION_PARAMETERS_FEEDBACK_INFO_KHR;
    feedback.pNext = &h264feedback;
    size_t datalen = 1024;
    loader->vkGetEncodedVideoSessionParametersKHR(device->vulkanDevice, &getInfo, nullptr, &datalen, nullptr);
    ptr->bitStreamHeader.resize(datalen);
    loader->vkGetEncodedVideoSessionParametersKHR(
        device->vulkanDevice, &getInfo, &feedback, &datalen, ptr->bitStreamHeader.data());
    ptr->bitStreamHeader.resize(datalen);

    // create readback buffer
    ptr->bitStreamBuffer = VK_NULL_HANDLE;
    ptr->bitStreamMemory = VK_NULL_HANDLE;
    {
        VkBufferCreateInfo bufferInfo = {};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = 4u * 1024u * 1024u;
        bufferInfo.usage = VK_BUFFER_USAGE_VIDEO_ENCODE_DST_BIT_KHR;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        bufferInfo.pNext = &videoProfileList;

        loader->vkCreateBuffer(device->vulkanDevice, &bufferInfo, nullptr, &ptr->bitStreamBuffer);

        VkMemoryRequirements bufMemReq = {};
        loader->vkGetBufferMemoryRequirements(device->vulkanDevice, ptr->bitStreamBuffer, &bufMemReq);

        uint32_t bufMemType = 0u;
        uint32_t bufMemType_sysmem = 0u;
        bufMemType = context_getMemoryType(ctx, bufMemReq.memoryTypeBits,
                                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                               VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
        if (bufMemType == ~0u)
        {
            bufMemType =
                context_getMemoryType(ctx, bufMemReq.memoryTypeBits,
                                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        }

        VkMemoryAllocateInfo bufMemAllocInfo = {};
        bufMemAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        bufMemAllocInfo.allocationSize = bufMemReq.size;
        bufMemAllocInfo.memoryTypeIndex = bufMemType;

        loader->vkAllocateMemory(device->vulkanDevice, &bufMemAllocInfo, nullptr, &ptr->bitStreamMemory);

        loader->vkBindBufferMemory(device->vulkanDevice, ptr->bitStreamBuffer, ptr->bitStreamMemory, 0u);

        loader->vkMapMemory(
            device->vulkanDevice, ptr->bitStreamMemory, 0u, VK_WHOLE_SIZE, 0u, (void**)&ptr->bitStreamData);
    }

    // create reference images
    for (uint32_t i = 0; i < 2u; i++)
    {
        VkImageCreateInfo createInfo = {};
        createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        createInfo.pNext = &videoProfileList;
        createInfo.imageType = VK_IMAGE_TYPE_2D;
        createInfo.format = chosenDpbImageFormat;
        createInfo.extent = { ptr->width, ptr->height, 1 };
        createInfo.mipLevels = 1;
        createInfo.arrayLayers = 1;
        createInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        createInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        createInfo.usage = VK_IMAGE_USAGE_VIDEO_ENCODE_DPB_BIT_KHR; // DPB only
        createInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        createInfo.queueFamilyIndexCount = 1u;
        createInfo.pQueueFamilyIndices = &device->encodeQueueFamilyIdx;
        createInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        createInfo.flags = 0;

        loader->vkCreateImage(device->vulkanDevice, &createInfo, nullptr, &ptr->dpbImages[i]);

        VkMemoryRequirements texMemReq = {};
        loader->vkGetImageMemoryRequirements(device->vulkanDevice, ptr->dpbImages[i], &texMemReq);
        uint32_t texMemType_device =
            context_getMemoryType(ctx, texMemReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VkMemoryAllocateInfo texMemAllocInfo = {};
        texMemAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        texMemAllocInfo.allocationSize = texMemReq.size;
        texMemAllocInfo.memoryTypeIndex = texMemType_device;

        loader->vkAllocateMemory(device->vulkanDevice, &texMemAllocInfo, nullptr, &ptr->dpbMemories[i]);
        loader->vkBindImageMemory(device->vulkanDevice, ptr->dpbImages[i], ptr->dpbMemories[i], 0u);

        VkImageViewCreateInfo viewInfo = {};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = ptr->dpbImages[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = chosenDpbImageFormat;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0u;
        viewInfo.subresourceRange.levelCount = 1u;
        viewInfo.subresourceRange.baseArrayLayer = 0u;
        viewInfo.subresourceRange.layerCount = 1u;

        loader->vkCreateImageView(device->vulkanDevice, &viewInfo, nullptr, &ptr->dpbImageViews[i]);
    }

    {
        uint32_t queueFamilies[] = { device->graphicsQueueFamilyIdx, device->encodeQueueFamilyIdx };
        VkImageCreateInfo createInfo = {};
        createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        createInfo.pNext = &videoProfileList;
        createInfo.imageType = VK_IMAGE_TYPE_2D;
        createInfo.format = chosenSrcImageFormat;
        createInfo.extent = { ptr->width, ptr->height, 1u };
        createInfo.mipLevels = 1u;
        createInfo.arrayLayers = 1u;
        createInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        createInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        createInfo.usage = VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR | VK_IMAGE_USAGE_STORAGE_BIT;
        if (queueFamilies[0] == queueFamilies[1])
        {
            createInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            createInfo.queueFamilyIndexCount = 0u;
            createInfo.pQueueFamilyIndices = nullptr;
        }
        else
        {
            createInfo.sharingMode = VK_SHARING_MODE_CONCURRENT;
            createInfo.queueFamilyIndexCount = 2u;
            createInfo.pQueueFamilyIndices = queueFamilies;
        }
        createInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        createInfo.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;

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
        texDesc.usage = PNANOVDB_COMPUTE_TEXTURE_USAGE_RW_TEXTURE | PNANOVDB_COMPUTE_TEXTURE_USAGE_COPY_DST;
        if (chosenSrcImageFormat == VK_FORMAT_G8_B8R8_2PLANE_420_UNORM)
        {
            texDesc.format = PNANOVDB_COMPUTE_FORMAT_G8_B8R8_2PLANE_420_UNORM;
        }
        else if (chosenSrcImageFormat == VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM)
        {
            texDesc.format = PNANOVDB_COMPUTE_FORMAT_G8_B8_R8_3PLANE_420_UNORM;
        }
        texDesc.width = ptr->width;
        texDesc.height = ptr->height;

        ptr->srcTexture = createTextureExternal(cast(ctx), &texDesc, ptr->srcImage, VK_IMAGE_LAYOUT_GENERAL);
    }

    {
        VkQueryPoolVideoEncodeFeedbackCreateInfoKHR feedbackCreateInfo = {};
        feedbackCreateInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_VIDEO_ENCODE_FEEDBACK_CREATE_INFO_KHR;
        feedbackCreateInfo.encodeFeedbackFlags = VK_VIDEO_ENCODE_FEEDBACK_BITSTREAM_BUFFER_OFFSET_BIT_KHR |
                                                 VK_VIDEO_ENCODE_FEEDBACK_BITSTREAM_BYTES_WRITTEN_BIT_KHR;
        feedbackCreateInfo.pNext = &videoProfile;
        VkQueryPoolCreateInfo createInfo = {};
        createInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        createInfo.queryType = VK_QUERY_TYPE_VIDEO_ENCODE_FEEDBACK_KHR;
        createInfo.queryCount = 1u;
        createInfo.pNext = &feedbackCreateInfo;
        loader->vkCreateQueryPool(device->vulkanDevice, &createInfo, nullptr, &ptr->queryPool);
    }

    {
        VkFenceCreateInfo createInfo = {};
        createInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        createInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        loader->vkCreateFence(device->vulkanDevice, &createInfo, nullptr, &ptr->encodeFinishedFence);
    }

    {
        VkCommandBufferAllocateInfo allocInfo = {};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandPool = ptr->commandPool;
        allocInfo.commandBufferCount = 1u;
        loader->vkAllocateCommandBuffers(device->vulkanDevice, &allocInfo, &ptr->commandBuffer);
        VkCommandBufferBeginInfo beginInfo = {};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        loader->vkBeginCommandBuffer(ptr->commandBuffer, &beginInfo);

        // rate control
        {
            VkVideoBeginCodingInfoKHR encodeBeginInfo = {};
            encodeBeginInfo.sType = VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR;
            encodeBeginInfo.videoSession = ptr->videoSession;
            encodeBeginInfo.videoSessionParameters = ptr->videoSessionParameters;

            VkVideoEncodeH264RateControlLayerInfoKHR* encodeH264RateControlLayerInfo =
                &ptr->encodeH264RateControlLayerInfo;
            encodeH264RateControlLayerInfo->sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_RATE_CONTROL_LAYER_INFO_KHR;

            // scale bitrate with resolution
            uint64_t pixel_count = ptr->width * ptr->height;

            uint64_t ave_bits_per_pixel = 5; // 5 Mbps to 20 Mbps at 1440x720
            uint64_t max_bits_per_pixel = 20;
            if (pixel_count >= 3840 * 2160) // 25 Mbps to 83 Mbps
            {
                ave_bits_per_pixel = 3;
                max_bits_per_pixel = 10;
            }
            else if (pixel_count >= 2560 * 1440) // 15 Mbps to 52 Mbps
            {
                ave_bits_per_pixel = 4;
                max_bits_per_pixel = 14;
            }
            else if (pixel_count >= 1920 * 1080) // 10 Mbps to 35 Mbps
            {
                ave_bits_per_pixel = 5;
                max_bits_per_pixel = 17;
            }

            uint64_t ave_bitrate = ave_bits_per_pixel * pixel_count;
            uint64_t max_bitrate = max_bits_per_pixel * pixel_count;

            VkVideoEncodeRateControlLayerInfoKHR* encodeRateControlLayerInfo = &ptr->encodeRateControlLayerInfo;
            encodeRateControlLayerInfo->sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_RATE_CONTROL_LAYER_INFO_KHR;
            encodeRateControlLayerInfo->pNext = encodeH264RateControlLayerInfo;
            encodeRateControlLayerInfo->frameRateNumerator = fps;
            encodeRateControlLayerInfo->frameRateDenominator = 1u;
            encodeRateControlLayerInfo->averageBitrate = ave_bitrate;
            encodeRateControlLayerInfo->maxBitrate = max_bitrate;

            VkVideoEncodeH264RateControlInfoKHR* encodeH264RateControlInfo = &ptr->encodeH264RateControlInfo;
            encodeH264RateControlInfo->sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_RATE_CONTROL_INFO_KHR;
            encodeH264RateControlInfo->flags = VK_VIDEO_ENCODE_H264_RATE_CONTROL_REGULAR_GOP_BIT_KHR |
                                               VK_VIDEO_ENCODE_H264_RATE_CONTROL_REFERENCE_PATTERN_FLAT_BIT_KHR;
            encodeH264RateControlInfo->gopFrameCount = 16u;
            encodeH264RateControlInfo->idrPeriod = 16u;
            encodeH264RateControlInfo->consecutiveBFrameCount = 0u;
            encodeH264RateControlInfo->temporalLayerCount = 1u;

            VkVideoEncodeRateControlInfoKHR* encodeRateControlInfo = &ptr->encodeRateControlInfo;
            encodeRateControlInfo->sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_RATE_CONTROL_INFO_KHR;
            encodeRateControlInfo->pNext = encodeH264RateControlInfo;
            encodeRateControlInfo->rateControlMode = chosenRateControlMode;
            encodeRateControlInfo->layerCount = 1u;
            encodeRateControlInfo->pLayers = encodeRateControlLayerInfo;
            encodeRateControlInfo->initialVirtualBufferSizeInMs = 100;
            encodeRateControlInfo->virtualBufferSizeInMs = 200;

            if (encodeRateControlInfo->rateControlMode & VK_VIDEO_ENCODE_RATE_CONTROL_MODE_CBR_BIT_KHR)
            {
                encodeRateControlLayerInfo->averageBitrate = encodeRateControlLayerInfo->maxBitrate;
            }
            if (encodeRateControlInfo->rateControlMode & VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DISABLED_BIT_KHR ||
                encodeRateControlInfo->rateControlMode == VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DEFAULT_KHR)
            {
                encodeH264RateControlInfo->temporalLayerCount = 0u;
                encodeRateControlInfo->layerCount = 0u;
            }

            VkVideoCodingControlInfoKHR codingControlInfo = {};
            codingControlInfo.sType = VK_STRUCTURE_TYPE_VIDEO_CODING_CONTROL_INFO_KHR;
            codingControlInfo.flags =
                VK_VIDEO_CODING_CONTROL_RESET_BIT_KHR | VK_VIDEO_CODING_CONTROL_ENCODE_RATE_CONTROL_BIT_KHR;
            codingControlInfo.pNext = encodeRateControlInfo;

            VkVideoEndCodingInfoKHR encodeEndInfo = {};
            encodeEndInfo.sType = VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR;
            loader->vkCmdBeginVideoCodingKHR(ptr->commandBuffer, &encodeBeginInfo);
            loader->vkCmdControlVideoCodingKHR(ptr->commandBuffer, &codingControlInfo);
            loader->vkCmdEndVideoCodingKHR(ptr->commandBuffer, &encodeEndInfo);
        }

        // transition src image
        {
            VkImageMemoryBarrier2 barrier = {};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            barrier.srcStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            barrier.subresourceRange.baseMipLevel = 0u;
            barrier.subresourceRange.levelCount = 1u;
            barrier.subresourceRange.baseArrayLayer = 0u;
            barrier.subresourceRange.layerCount = 1u;
            barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;

            barrier.image = ptr->srcImage;

            VkDependencyInfoKHR dependencyInfo = {};
            dependencyInfo.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO_KHR;
            dependencyInfo.imageMemoryBarrierCount = 1u;
            dependencyInfo.pImageMemoryBarriers = &barrier;

            loader->vkCmdPipelineBarrier2(ptr->commandBuffer, &dependencyInfo);
        }

        // transition dpb images
        {
            VkImageMemoryBarrier2 barriers[2] = {};
            barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            barriers[0].srcStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
            barriers[0].dstStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            barriers[0].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barriers[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            barriers[0].subresourceRange.baseMipLevel = 0u;
            barriers[0].subresourceRange.levelCount = 1u;
            barriers[0].subresourceRange.baseArrayLayer = 0u;
            barriers[0].subresourceRange.layerCount = 1u;
            barriers[0].newLayout = VK_IMAGE_LAYOUT_VIDEO_ENCODE_DPB_KHR;

            barriers[0].image = ptr->dpbImages[0];

            barriers[1] = barriers[0];
            barriers[1].image = ptr->dpbImages[1];

            VkDependencyInfoKHR dependencyInfo = {};
            dependencyInfo.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO_KHR;
            dependencyInfo.imageMemoryBarrierCount = 2u;
            dependencyInfo.pImageMemoryBarriers = barriers;

            loader->vkCmdPipelineBarrier2(ptr->commandBuffer, &dependencyInfo);
        }

        loader->vkEndCommandBuffer(ptr->commandBuffer);
        VkSubmitInfo submitInfo = {};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1u;
        submitInfo.pCommandBuffers = &ptr->commandBuffer;
        loader->vkResetFences(device->vulkanDevice, 1u, &ptr->encodeFinishedFence);
        loader->vkQueueSubmit(device->encodeQueueVk, 1u, &submitInfo, ptr->encodeFinishedFence);
        loader->vkWaitForFences(device->vulkanDevice, 1u, &ptr->encodeFinishedFence, VK_TRUE, ~0llu);
        loader->vkResetCommandBuffer(ptr->commandBuffer, 0u);
    }

    ptr->frameCount = 0u;

    return cast(ptr);
}

int present_encoder(pnanovdb_compute_encoder_t* encoder, pnanovdb_uint64_t* flushedFrameID)
{
    auto ptr = cast(encoder);

    Device* device = ptr->deviceQueue->device;
    Context* ctx = ptr->deviceQueue->context;
    auto loader = &device->loader;

    *flushedFrameID = ptr->deviceQueue->nextFenceValue;

    ptr->deviceQueue->currentEndFrameSemaphore =
        ptr->deviceQueue->endFrameSemaphore[device->deviceQueue->commandBufferIdx];

    int deviceReset = flushStepA(ptr->deviceQueue, nullptr, nullptr);

    const uint32_t GOP_LENGTH = 16;
    const uint32_t gopFrameCount = ptr->frameCount % GOP_LENGTH;

    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    loader->vkBeginCommandBuffer(ptr->commandBuffer, &beginInfo);
    const uint32_t querySlotId = 0u;
    loader->vkCmdResetQueryPool(ptr->commandBuffer, ptr->queryPool, querySlotId, 1);

    VkVideoPictureResourceInfoKHR dpbPicResource = {};
    dpbPicResource.sType = VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR;
    dpbPicResource.imageViewBinding = ptr->dpbImageViews[gopFrameCount & 1];
    dpbPicResource.codedOffset = { 0, 0 };
    dpbPicResource.codedExtent = { ptr->width, ptr->height };
    dpbPicResource.baseArrayLayer = 0;

    VkVideoPictureResourceInfoKHR refPicResource = {};
    refPicResource.sType = VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR;
    refPicResource.imageViewBinding = ptr->dpbImageViews[(gopFrameCount & 1) ^ 1];
    refPicResource.codedOffset = { 0, 0 };
    refPicResource.codedExtent = { ptr->width, ptr->height };
    refPicResource.baseArrayLayer = 0;

    const uint32_t log2_max_pic_order_cnt_lsb_minus4 = 4u;
    const uint32_t maxPicOrderCntLsb = 1 << (log2_max_pic_order_cnt_lsb_minus4 + 4u);
    StdVideoEncodeH264ReferenceInfo dpbRefInfo = {};
    dpbRefInfo.FrameNum = gopFrameCount;
    dpbRefInfo.PicOrderCnt = (dpbRefInfo.FrameNum * 2) % maxPicOrderCntLsb;
    dpbRefInfo.primary_pic_type =
        dpbRefInfo.FrameNum == 0 ? STD_VIDEO_H264_PICTURE_TYPE_IDR : STD_VIDEO_H264_PICTURE_TYPE_P;
    VkVideoEncodeH264DpbSlotInfoKHR dpbSlotInfo = {};
    dpbSlotInfo.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_DPB_SLOT_INFO_KHR;
    dpbSlotInfo.pNext = nullptr;
    dpbSlotInfo.pStdReferenceInfo = &dpbRefInfo;

    StdVideoEncodeH264ReferenceInfo refRefInfo = {};
    refRefInfo.FrameNum = gopFrameCount - 1;
    refRefInfo.PicOrderCnt = (refRefInfo.FrameNum * 2) % maxPicOrderCntLsb;
    refRefInfo.primary_pic_type =
        refRefInfo.FrameNum == 0 ? STD_VIDEO_H264_PICTURE_TYPE_IDR : STD_VIDEO_H264_PICTURE_TYPE_P;
    VkVideoEncodeH264DpbSlotInfoKHR refSlotInfo = {};
    refSlotInfo.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_DPB_SLOT_INFO_KHR;
    refSlotInfo.pNext = nullptr;
    refSlotInfo.pStdReferenceInfo = &refRefInfo;

    VkVideoReferenceSlotInfoKHR referenceSlots[2];
    referenceSlots[0].sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR;
    referenceSlots[0].pNext = &dpbSlotInfo;
    referenceSlots[0].slotIndex = -1;
    referenceSlots[0].pPictureResource = &dpbPicResource;
    referenceSlots[1].sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR;
    referenceSlots[1].pNext = &refSlotInfo;
    referenceSlots[1].slotIndex = (gopFrameCount & 1) ^ 1;
    referenceSlots[1].pPictureResource = &refPicResource;

    VkVideoBeginCodingInfoKHR encodeBeginInfo = {};
    encodeBeginInfo.sType = VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR;
    encodeBeginInfo.pNext = &ptr->encodeRateControlInfo;
    encodeBeginInfo.videoSession = ptr->videoSession;
    encodeBeginInfo.videoSessionParameters = ptr->videoSessionParameters;
    encodeBeginInfo.referenceSlotCount = gopFrameCount == 0 ? 1 : 2;
    encodeBeginInfo.pReferenceSlots = referenceSlots;

    loader->vkCmdBeginVideoCodingKHR(ptr->commandBuffer, &encodeBeginInfo);

    VkImageMemoryBarrier2 imageMemoryBarrier = {};
    imageMemoryBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    imageMemoryBarrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
    imageMemoryBarrier.srcAccessMask = VK_ACCESS_2_NONE;
    imageMemoryBarrier.dstStageMask = VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR;
    imageMemoryBarrier.dstAccessMask = VK_ACCESS_2_VIDEO_ENCODE_READ_BIT_KHR;
    imageMemoryBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    imageMemoryBarrier.newLayout = VK_IMAGE_LAYOUT_VIDEO_ENCODE_SRC_KHR;
    imageMemoryBarrier.image = ptr->srcImage;
    imageMemoryBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    imageMemoryBarrier.subresourceRange.baseMipLevel = 0u;
    imageMemoryBarrier.subresourceRange.levelCount = 1u;
    imageMemoryBarrier.subresourceRange.baseArrayLayer = 0u;
    imageMemoryBarrier.subresourceRange.layerCount = 1u;
    VkDependencyInfoKHR dependencyInfo = {};
    dependencyInfo.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO_KHR;
    dependencyInfo.imageMemoryBarrierCount = 1u;
    dependencyInfo.pImageMemoryBarriers = &imageMemoryBarrier;
    loader->vkCmdPipelineBarrier2(ptr->commandBuffer, &dependencyInfo);

    VkVideoPictureResourceInfoKHR inputPicResource = {};
    inputPicResource.sType = VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR;
    inputPicResource.imageViewBinding = texture_getImageViewAll(
        ctx, cast(ptr->srcTexture), PNANOVDB_COMPUTE_FORMAT_UNKNOWN, PNANOVDB_COMPUTE_TEXTURE_ASPECT_NONE);
    inputPicResource.codedOffset = { 0, 0 };
    inputPicResource.codedExtent = { ptr->width, ptr->height };
    inputPicResource.baseArrayLayer = 0u;

    StdVideoEncodeH264SliceHeaderFlags sliceHeaderFlags = {};
    StdVideoEncodeH264SliceHeader sliceHeader = {};
    VkVideoEncodeH264NaluSliceInfoKHR sliceInfo = {};
    StdVideoEncodeH264PictureInfoFlags pictureInfoFlags = {};
    StdVideoEncodeH264PictureInfo pictureInfo = {};
    VkVideoEncodeH264PictureInfoKHR encodeH264FrameInfo = {};
    StdVideoEncodeH264ReferenceListsInfo referenceLists = {};
    {
        bool useConstantQp = ptr->chosenRateControlMode & VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DISABLED_BIT_KHR;
        bool isI = gopFrameCount == 0;

        sliceHeaderFlags.direct_spatial_mv_pred_flag = 1u;
        sliceHeaderFlags.num_ref_idx_active_override_flag = 0u;

        sliceHeader.flags = sliceHeaderFlags;
        sliceHeader.slice_type = isI ? STD_VIDEO_H264_SLICE_TYPE_I : STD_VIDEO_H264_SLICE_TYPE_P;
        sliceHeader.cabac_init_idc = (StdVideoH264CabacInitIdc)0;
        sliceHeader.disable_deblocking_filter_idc = (StdVideoH264DisableDeblockingFilterIdc)0;
        sliceHeader.slice_alpha_c0_offset_div2 = 0u;
        sliceHeader.slice_beta_offset_div2 = 0u;

        const uint32_t alignedWidth = (ptr->width + 15) & ~15;
        const uint32_t alignedHeight = (ptr->height + 15) & ~15;
        const uint32_t pic_width_in_mbs = (alignedWidth / 16);
        const uint32_t pic_height_in_map_units = (alignedHeight / 16);
        const uint32_t iPicSizeInMbs = pic_width_in_mbs * pic_height_in_map_units;

        sliceInfo.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_NALU_SLICE_INFO_KHR;
        sliceInfo.pNext = nullptr;
        sliceInfo.pStdSliceHeader = &sliceHeader;
        sliceInfo.constantQp = useConstantQp ? /*pic_init_qp_minus26 +*/ 26 : 0;

        pictureInfoFlags.IdrPicFlag = isI ? 1 : 0;
        pictureInfoFlags.is_reference = 1;
        pictureInfoFlags.adaptive_ref_pic_marking_mode_flag = 0;
        pictureInfoFlags.no_output_of_prior_pics_flag = isI ? 1 : 0;

        pictureInfo.flags = pictureInfoFlags;
        pictureInfo.seq_parameter_set_id = 0;
        pictureInfo.pic_parameter_set_id = 0u; // pic_parameter_set_id;
        pictureInfo.idr_pic_id = 0u;
        pictureInfo.primary_pic_type = isI ? STD_VIDEO_H264_PICTURE_TYPE_IDR : STD_VIDEO_H264_PICTURE_TYPE_P;
        // pictureInfo.temporal_id = 1;
        pictureInfo.frame_num = gopFrameCount;
        pictureInfo.PicOrderCnt = (gopFrameCount * 2u) % maxPicOrderCntLsb;

        referenceLists.num_ref_idx_l0_active_minus1 = 0;
        referenceLists.num_ref_idx_l1_active_minus1 = 0;
        for (uint32_t i = 0u; i < STD_VIDEO_H264_MAX_NUM_LIST_REF; i++)
        {
            referenceLists.RefPicList0[i] = STD_VIDEO_H264_NO_REFERENCE_PICTURE;
            referenceLists.RefPicList1[i] = STD_VIDEO_H264_NO_REFERENCE_PICTURE;
        }
        if (!isI)
        {
            referenceLists.RefPicList0[0] = (gopFrameCount & 1) ^ 1;
        }
        pictureInfo.pRefLists = &referenceLists;

        encodeH264FrameInfo.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_PICTURE_INFO_KHR;
        encodeH264FrameInfo.pNext = nullptr;
        encodeH264FrameInfo.naluSliceEntryCount = 1;
        encodeH264FrameInfo.pNaluSliceEntries = &sliceInfo;
        encodeH264FrameInfo.pStdPictureInfo = &pictureInfo;
    }

    VkVideoEncodeInfoKHR videoEncodeInfo = {};
    videoEncodeInfo.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_INFO_KHR;
    videoEncodeInfo.pNext = &encodeH264FrameInfo;
    videoEncodeInfo.dstBuffer = ptr->bitStreamBuffer;
    videoEncodeInfo.dstBufferOffset = 0u;
    videoEncodeInfo.dstBufferRange = 4u * 1024u * 1024u;
    videoEncodeInfo.srcPictureResource = inputPicResource;
    referenceSlots[0].slotIndex = gopFrameCount * 1u;
    videoEncodeInfo.pSetupReferenceSlot = &referenceSlots[0];

    if (gopFrameCount > 0u)
    {
        videoEncodeInfo.referenceSlotCount = 1u;
        videoEncodeInfo.pReferenceSlots = &referenceSlots[1];
    }

    VkQueryControlFlags queryFlags = 0u;
    loader->vkCmdBeginQuery(ptr->commandBuffer, ptr->queryPool, querySlotId, queryFlags);

    loader->vkCmdEncodeVideoKHR(ptr->commandBuffer, &videoEncodeInfo);

    loader->vkCmdEndQuery(ptr->commandBuffer, ptr->queryPool, querySlotId);

    VkVideoEndCodingInfoKHR encodeEndInfo = {};
    encodeEndInfo.sType = VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR;
    loader->vkCmdEndVideoCodingKHR(ptr->commandBuffer, &encodeEndInfo);

    // barrier to restore old state
    imageMemoryBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    imageMemoryBarrier.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
    imageMemoryBarrier.dstAccessMask = VK_ACCESS_2_NONE;
    imageMemoryBarrier.srcStageMask = VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR;
    imageMemoryBarrier.srcAccessMask = VK_ACCESS_2_VIDEO_ENCODE_READ_BIT_KHR;
    imageMemoryBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    imageMemoryBarrier.oldLayout = VK_IMAGE_LAYOUT_VIDEO_ENCODE_SRC_KHR;
    imageMemoryBarrier.image = ptr->srcImage;
    imageMemoryBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    imageMemoryBarrier.subresourceRange.baseMipLevel = 0u;
    imageMemoryBarrier.subresourceRange.levelCount = 1u;
    imageMemoryBarrier.subresourceRange.baseArrayLayer = 0u;
    imageMemoryBarrier.subresourceRange.layerCount = 1u;
    dependencyInfo.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO_KHR;
    dependencyInfo.imageMemoryBarrierCount = 1u;
    dependencyInfo.pImageMemoryBarriers = &imageMemoryBarrier;
    loader->vkCmdPipelineBarrier2(ptr->commandBuffer, &dependencyInfo);

    loader->vkEndCommandBuffer(ptr->commandBuffer);

    ptr->deviceQueue->currentBeginFrameSemaphore =
        ptr->deviceQueue->beginFrameSemaphore[device->deviceQueue->commandBufferIdx];

    VkPipelineStageFlags dstStagemask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo submitInfo = {};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.waitSemaphoreCount = 1u;
    submitInfo.pWaitSemaphores = &ptr->deviceQueue->currentEndFrameSemaphore;
    submitInfo.pWaitDstStageMask = &dstStagemask;
    submitInfo.commandBufferCount = 1u;
    submitInfo.pCommandBuffers = &ptr->commandBuffer;
    submitInfo.signalSemaphoreCount = 1u;
    submitInfo.pSignalSemaphores = &ptr->deviceQueue->currentBeginFrameSemaphore;
    loader->vkResetFences(device->vulkanDevice, 1u, &ptr->encodeFinishedFence);
    loader->vkQueueSubmit(device->encodeQueueVk, 1u, &submitInfo, ptr->encodeFinishedFence);

    ptr->deviceQueue->currentEndFrameSemaphore = VK_NULL_HANDLE;

    flushStepB(ptr->deviceQueue);

    return deviceReset;
}

void destroy_encoder(pnanovdb_compute_encoder_t* encoder)
{
    auto ptr = cast(encoder);
    Device* device = ptr->deviceQueue->device;
    auto loader = &device->loader;

    loader->vkDestroyFence(device->vulkanDevice, ptr->encodeFinishedFence, nullptr);

    loader->vkDestroyVideoSessionParametersKHR(device->vulkanDevice, ptr->videoSessionParameters, nullptr);
    loader->vkDestroyQueryPool(device->vulkanDevice, ptr->queryPool, nullptr);

    loader->vkDestroyBuffer(device->vulkanDevice, ptr->bitStreamBuffer, nullptr);
    loader->vkFreeMemory(device->vulkanDevice, ptr->bitStreamMemory, nullptr);

    destroyTexture(cast(ptr->deviceQueue->context), ptr->srcTexture);
    loader->vkDestroyImage(device->vulkanDevice, ptr->srcImage, nullptr);
    loader->vkFreeMemory(device->vulkanDevice, ptr->srcMemory, nullptr);

    for (uint32_t i = 0; i < 2u; i++)
    {
        loader->vkDestroyImageView(device->vulkanDevice, ptr->dpbImageViews[i], nullptr);
        loader->vkDestroyImage(device->vulkanDevice, ptr->dpbImages[i], nullptr);
        loader->vkFreeMemory(device->vulkanDevice, ptr->dpbMemories[i], nullptr);
    }

    loader->vkDestroyVideoSessionKHR(device->vulkanDevice, ptr->videoSession, nullptr);
    for (uint32_t i = 0; i < ptr->memories.size(); i++)
    {
        loader->vkFreeMemory(device->vulkanDevice, ptr->memories[i], nullptr);
    }
    ptr->memories.clear();
    ptr->bitStreamHeader.clear();

    loader->vkFreeCommandBuffers(device->vulkanDevice, ptr->commandPool, 1u, &ptr->commandBuffer);
    loader->vkDestroyCommandPool(device->vulkanDevice, ptr->commandPool, nullptr);
}

pnanovdb_compute_texture_t* get_encoder_front_texture(pnanovdb_compute_encoder_t* encoder)
{
    auto ptr = cast(encoder);

    return ptr->srcTexture;
}

void* map_encoder_data(pnanovdb_compute_encoder_t* encoder, pnanovdb_uint64_t* p_mapped_byte_count)
{
    auto ptr = cast(encoder);
    Device* device = ptr->deviceQueue->device;
    auto loader = &device->loader;

    loader->vkWaitForFences(device->vulkanDevice, 1u, &ptr->encodeFinishedFence, VK_TRUE, ~0llu);

    loader->vkResetCommandBuffer(ptr->commandBuffer, 0u);

    struct VideoEncodeStatus
    {
        uint32_t bitstreamStartOffset;
        uint32_t bitstreamSize;
        VkQueryResultStatusKHR status;
    };
    VideoEncodeStatus encodeResult = {};
    const uint32_t querySlotId = 0;
    loader->vkGetQueryPoolResults(device->vulkanDevice, ptr->queryPool, querySlotId, 1u, sizeof(VideoEncodeStatus),
                                  &encodeResult, sizeof(VideoEncodeStatus),
                                  VK_QUERY_RESULT_WITH_STATUS_BIT_KHR | VK_QUERY_RESULT_WAIT_BIT);

    uint8_t* outData = ptr->bitStreamData + encodeResult.bitstreamStartOffset;
    uint32_t outSize = encodeResult.bitstreamSize;

    // every five seconds, include bitstream header for livestream
    if ((ptr->frameCount % 60u) == 0)
    {
        ptr->bitStreamTmp = ptr->bitStreamHeader;
        for (uint32_t i = 0u; i < outSize; i++)
        {
            ptr->bitStreamTmp.push_back(outData[i]);
        }
        outData = (uint8_t*)ptr->bitStreamTmp.data();
        outSize = (uint32_t)ptr->bitStreamTmp.size();
    }

    ptr->frameCount++;

    *p_mapped_byte_count = outSize;
    return outData;
}

void unmap_encoder_data(pnanovdb_compute_encoder_t* encoder)
{
    // nop
}

} // end namespace
