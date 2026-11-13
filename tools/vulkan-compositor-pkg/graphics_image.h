#pragma once
#include <stdint.h>
#include <vulkan/vulkan.h>

struct BrookVkGraphicsImage {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
};

/* Import a distinct, linear BGRA8 DMA-BUF. The caller validates FD identity
 * and producer synchronization, keeps the source stable through retirement,
 * and never destroys this object while GPU access may still be pending.
 * The destination struct must be zero-initialized and must not own an image.
 * Unsupported layout, size or memory type is a clean rejection. Like the
 * existing buffer importer, size discovery resets the shared FD offset to 0. */
int brook_vk_graphics_image_import(VkPhysicalDevice physical,VkDevice device,int fd,
        uint32_t width,uint32_t height,uint32_t offsetWords,uint32_t strideWords,
        uint64_t allocationBytes,VkImageUsageFlags usage,struct BrookVkGraphicsImage *out);
void brook_vk_graphics_image_destroy(VkDevice device,struct BrookVkGraphicsImage *image);
