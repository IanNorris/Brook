#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int has(const VkExtensionProperties *p,uint32_t n,const char *name) {
    for(uint32_t i=0;i<n;++i)if(!strcmp(p[i].extensionName,name))return 1;
    return 0;
}
int main(void) {
    VkInstance instance=VK_NULL_HANDLE;
    VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.pApplicationName="Brook compositor capabilities",.apiVersion=VK_API_VERSION_1_1};
    VkInstanceCreateInfo ci={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app};
    if(vkCreateInstance(&ci,NULL,&instance)!=VK_SUCCESS)return 1;
    uint32_t n=0;if(vkEnumeratePhysicalDevices(instance,&n,NULL)!=VK_SUCCESS || !n || n>32)return 1;
    VkPhysicalDevice devices[32];if(vkEnumeratePhysicalDevices(instance,&n,devices)!=VK_SUCCESS)return 1;
    int found=0;
    for(uint32_t i=0;i<n;++i){
        VkPhysicalDeviceProperties props;vkGetPhysicalDeviceProperties(devices[i],&props);
        if(props.deviceType==VK_PHYSICAL_DEVICE_TYPE_CPU)continue;
        uint32_t count=0;if(vkEnumerateDeviceExtensionProperties(devices[i],NULL,&count,NULL)!=VK_SUCCESS || count>4096)continue;
        VkExtensionProperties *ext=calloc(count?count:1,sizeof(*ext));if(!ext)return 1;
        if(vkEnumerateDeviceExtensionProperties(devices[i],NULL,&count,ext)!=VK_SUCCESS){free(ext);continue;}
        int memory=has(ext,count,VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
        int dma=has(ext,count,VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
        int semaphore=has(ext,count,VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME);
        int modifier=has(ext,count,VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME);
        VkPhysicalDeviceExternalBufferInfo info={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO,
            .usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,.handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
        VkExternalBufferProperties buffer={.sType=VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES};
        vkGetPhysicalDeviceExternalBufferProperties(devices[i],&info,&buffer);
        VkPhysicalDeviceExternalSemaphoreInfo sinfo={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO,
            .handleType=VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT};
        VkExternalSemaphoreProperties sp={.sType=VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};
        vkGetPhysicalDeviceExternalSemaphoreProperties(devices[i],&sinfo,&sp);
        VkPhysicalDeviceFeatures features;vkGetPhysicalDeviceFeatures(devices[i],&features);
        printf("device=%s vendor=%04x device_id=%04x external_memory_fd=%d dma_buf=%d semaphore_fd=%d modifier=%d "
               "buffer_features=%u buffer_handles=%u sync_features=%u dynamic_storage_index=%u max_storage_buffers=%u\n",
            props.deviceName,props.vendorID,props.deviceID,memory,dma,semaphore,modifier,
            buffer.externalMemoryProperties.externalMemoryFeatures,buffer.externalMemoryProperties.compatibleHandleTypes,
            sp.externalSemaphoreFeatures,features.shaderStorageBufferArrayDynamicIndexing,
            props.limits.maxPerStageDescriptorStorageBuffers);
        free(ext);found=1;
    }
    vkDestroyInstance(instance,NULL);return found?0:1;
}
