/* Query, but do not import or modify, externally shared compositor images. */
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int has_extension(const VkExtensionProperties *extensions, uint32_t count, const char *name)
{
    for (uint32_t i = 0; i < count; ++i)
        if (!strcmp(extensions[i].extensionName, name)) return 1;
    return 0;
}

static void query(VkPhysicalDevice device, const char *name, VkFormat format,
                  VkImageUsageFlags usage, VkExternalMemoryHandleTypeFlagBits handle)
{
    const VkPhysicalDeviceExternalImageFormatInfo external = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
        .handleType = handle,
    };
    const VkPhysicalDeviceImageFormatInfo2 input = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
        .pNext = &external, .format = format, .type = VK_IMAGE_TYPE_2D,
        .tiling = VK_IMAGE_TILING_LINEAR, .usage = usage,
    };
    VkExternalImageFormatProperties externalOutput = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES,
    };
    VkImageFormatProperties2 output = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, .pNext = &externalOutput,
    };
    VkResult result = vkGetPhysicalDeviceImageFormatProperties2(device, &input, &output);
    const VkExternalMemoryProperties *memory = &externalOutput.externalMemoryProperties;
    printf("EXTERNAL_IMAGE name=%s format=%d usage=0x%x handle=0x%x result=%d "
           "features=0x%x compatible=0x%x max=%ux%u\n", name, (int)format,
           usage, handle, result, memory->externalMemoryFeatures, memory->compatibleHandleTypes,
           output.imageFormatProperties.maxExtent.width, output.imageFormatProperties.maxExtent.height);
}

int main(void)
{
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "brook-external-image-probe", .apiVersion = VK_API_VERSION_1_1};
    VkInstanceCreateInfo create = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VkInstance instance = VK_NULL_HANDLE;
    VkResult result = vkCreateInstance(&create, NULL, &instance);
    if (result != VK_SUCCESS) { fprintf(stderr, "vkCreateInstance: %d\n", result); return 1; }
    uint32_t count = 0;
    result = vkEnumeratePhysicalDevices(instance, &count, NULL);
    if (result != VK_SUCCESS || !count) { fprintf(stderr, "no Vulkan device: %d\n", result); vkDestroyInstance(instance, NULL); return 1; }
    VkPhysicalDevice *devices = calloc(count, sizeof(*devices));
    if (!devices) { vkDestroyInstance(instance, NULL); return 1; }
    result = vkEnumeratePhysicalDevices(instance, &count, devices);
    if (result != VK_SUCCESS) { free(devices); vkDestroyInstance(instance, NULL); return 1; }
    for (uint32_t i = 0; i < count; ++i) {
        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(devices[i], &properties);
        uint32_t n = 0;
        result = vkEnumerateDeviceExtensionProperties(devices[i], NULL, &n, NULL);
        if (result != VK_SUCCESS) continue;
        VkExtensionProperties *extensions = calloc(n ? n : 1, sizeof(*extensions));
        if (!extensions) continue;
        result = vkEnumerateDeviceExtensionProperties(devices[i], NULL, &n, extensions);
        if (result != VK_SUCCESS) { free(extensions); continue; }
        printf("DEVICE name=%s vendor=0x%x device=0x%x api=%u.%u.%u "
               "external_fd=%d dma_buf=%d drm_modifier=%d\n", properties.deviceName,
               properties.vendorID, properties.deviceID, VK_VERSION_MAJOR(properties.apiVersion),
               VK_VERSION_MINOR(properties.apiVersion), VK_VERSION_PATCH(properties.apiVersion),
               has_extension(extensions,n,VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME),
               has_extension(extensions,n,VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME),
               has_extension(extensions,n,VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME));
        if (has_extension(extensions,n,VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME)) {
            const VkExternalMemoryHandleTypeFlagBits handle = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
            query(devices[i], "sample-bgra", VK_FORMAT_B8G8R8A8_UNORM,
                  VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, handle);
            query(devices[i], "sample-bgra-only", VK_FORMAT_B8G8R8A8_UNORM,
                  VK_IMAGE_USAGE_SAMPLED_BIT, handle);
            query(devices[i], "render-bgra", VK_FORMAT_B8G8R8A8_UNORM,
                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, handle);
        }
        free(extensions);
    }
    free(devices);
    vkDestroyInstance(instance, NULL);
    return 0;
}
