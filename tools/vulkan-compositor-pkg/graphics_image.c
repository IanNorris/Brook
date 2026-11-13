#define _POSIX_C_SOURCE 200809L
#include "graphics_image.h"
#include "graphics_layers.h"
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

void brook_vk_graphics_image_destroy(VkDevice device,struct BrookVkGraphicsImage *image)
{
    if(!image)return;
    if(image->view)vkDestroyImageView(device,image->view,NULL);
    if(image->image)vkDestroyImage(device,image->image,NULL);
    if(image->memory)vkFreeMemory(device,image->memory,NULL);
    memset(image,0,sizeof(*image));
}

int brook_vk_graphics_image_import(VkPhysicalDevice physical,VkDevice device,int fd,
        uint32_t width,uint32_t height,uint32_t offsetWords,uint32_t strideWords,
        uint64_t allocationBytes,VkImageUsageFlags usage,struct BrookVkGraphicsImage *out)
{
    if(!out || out->image || out->memory || out->view)return 0;
    if(!physical || !device || fd<0 || !usage || !allocationBytes ||
       allocationBytes>(uint64_t)INT64_MAX)return 0;
    off_t size=lseek(fd,0,SEEK_END);
    if(lseek(fd,0,SEEK_SET)!=0 || size<0 || (uint64_t)size!=allocationBytes)return 0;
    VkPhysicalDeviceExternalImageFormatInfo externalFormat={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
        .handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkPhysicalDeviceImageFormatInfo2 format={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
        .pNext=&externalFormat,.format=VK_FORMAT_B8G8R8A8_UNORM,.type=VK_IMAGE_TYPE_2D,
        .tiling=VK_IMAGE_TILING_LINEAR,.usage=usage};
    VkExternalImageFormatProperties externalProperties={.sType=VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
    VkImageFormatProperties2 properties2={.sType=VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,.pNext=&externalProperties};
    if(vkGetPhysicalDeviceImageFormatProperties2(physical,&format,&properties2)!=VK_SUCCESS ||
       !(externalProperties.externalMemoryProperties.externalMemoryFeatures&VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) ||
       !width || !height || width>properties2.imageFormatProperties.maxExtent.width ||
       height>properties2.imageFormatProperties.maxExtent.height)return 0;
    VkExternalMemoryImageCreateInfo external={.sType=VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .handleTypes=VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkImageCreateInfo create={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.pNext=&external,
        .imageType=VK_IMAGE_TYPE_2D,.format=VK_FORMAT_B8G8R8A8_UNORM,.extent={width,height,1},
        .mipLevels=1,.arrayLayers=1,.samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_LINEAR,
        .usage=usage,.sharingMode=VK_SHARING_MODE_EXCLUSIVE,.initialLayout=VK_IMAGE_LAYOUT_PREINITIALIZED};
    if(vkCreateImage(device,&create,NULL,&out->image)!=VK_SUCCESS)goto reject;
    VkImageSubresource sub={.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT};VkSubresourceLayout layout;
    vkGetImageSubresourceLayout(device,out->image,&sub,&layout);
    VkMemoryDedicatedRequirements dedicated={.sType=VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
    VkMemoryRequirements2 requirements={.sType=VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,.pNext=&dedicated};
    VkImageMemoryRequirementsInfo2 info={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,.image=out->image};
    vkGetImageMemoryRequirements2(device,&info,&requirements);
    if(!brook_vk_graphics_linear_layout_valid(width,height,offsetWords,strideWords,allocationBytes,
        layout.offset,layout.rowPitch,requirements.memoryRequirements.size))goto reject;
    PFN_vkGetMemoryFdPropertiesKHR getProps=(PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(device,"vkGetMemoryFdPropertiesKHR");
    if(!getProps)goto reject;
    int copy=fcntl(fd,F_DUPFD_CLOEXEC,0);if(copy<0)goto reject;
    VkMemoryFdPropertiesKHR properties={.sType=VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
    if(getProps(device,VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,copy,&properties)!=VK_SUCCESS){close(copy);goto reject;}
    VkPhysicalDeviceMemoryProperties memory;vkGetPhysicalDeviceMemoryProperties(physical,&memory);
    const uint32_t allowed=properties.memoryTypeBits&requirements.memoryRequirements.memoryTypeBits;
    uint32_t type=UINT32_MAX;
    for(uint32_t i=0;i<memory.memoryTypeCount;++i)if(allowed&(1u<<i)){type=i;break;}
    if(type==UINT32_MAX){close(copy);goto reject;}
    VkMemoryDedicatedAllocateInfo dedicatedInfo={.sType=VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,.image=out->image};
    VkImportMemoryFdInfoKHR import={.sType=VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
        .pNext=(dedicated.requiresDedicatedAllocation ||
               (externalProperties.externalMemoryProperties.externalMemoryFeatures&VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT))?&dedicatedInfo:NULL,
        .handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,.fd=copy};
    VkMemoryAllocateInfo allocate={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.pNext=&import,
        .allocationSize=allocationBytes,.memoryTypeIndex=type};
    if(vkAllocateMemory(device,&allocate,NULL,&out->memory)!=VK_SUCCESS){close(copy);goto reject;}
    if(vkBindImageMemory(device,out->image,out->memory,0)!=VK_SUCCESS)goto reject;
    VkImageViewCreateInfo view={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=out->image,
        .viewType=VK_IMAGE_VIEW_TYPE_2D,.format=VK_FORMAT_B8G8R8A8_UNORM,
        .subresourceRange={.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT,.levelCount=1,.layerCount=1}};
    if(vkCreateImageView(device,&view,NULL,&out->view)!=VK_SUCCESS)goto reject;
    return 1;
reject:
    brook_vk_graphics_image_destroy(device,out);return 0;
}
