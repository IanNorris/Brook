// Disposable, root-only test using the real HasVK compute cache policy.
#include "bdw_cache_probe_abi.h"
#include "cache_probe_spv.h"
#include <fcntl.h>

static int cache_probe_allocate(int node, VkDevice device, uint32_t omit,
                                const VkPhysicalDeviceMemoryProperties* memory,
                                VkDeviceSize size, Buffer* out, int* oracleFd)
{
    struct BrookBdwCacheProbe request={.operation=BROOK_BDW_CACHE_PROBE_ALLOC,
        .omit=omit,.fd=-1,.bytes=size};
    if(ioctl(node,BROOK_BDW_CACHE_PROBE_IOCTL,&request))return 0;
    *oracleFd=request.fd;
    VkExternalMemoryBufferCreateInfo external={.sType=VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
        .handleTypes=VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkBufferCreateInfo create={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.pNext=&external,
        .size=size,.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
    if(vkCreateBuffer(device,&create,NULL,&out->buffer)!=VK_SUCCESS)return 0;
    VkMemoryRequirements needs;
    vkGetBufferMemoryRequirements(device,out->buffer,&needs);
    PFN_vkGetMemoryFdPropertiesKHR properties=(PFN_vkGetMemoryFdPropertiesKHR)
        vkGetDeviceProcAddr(device,"vkGetMemoryFdPropertiesKHR");
    VkMemoryFdPropertiesKHR fdProperties={.sType=VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
    if(!properties || needs.size!=size || properties(device,external.handleTypes,*oracleFd,&fdProperties)!=VK_SUCCESS)return 0;
    const VkMemoryPropertyFlags required=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    uint32_t index=0;
    for(;index<memory->memoryTypeCount;++index)
        if(((needs.memoryTypeBits&fdProperties.memoryTypeBits)&(1u<<index)) &&
           (memory->memoryTypes[index].propertyFlags&required)==required)break;
    if(index==memory->memoryTypeCount)return 0;
    int fd=fcntl(*oracleFd,F_DUPFD_CLOEXEC,0);
    if(fd<0)return 0;
    VkImportMemoryFdInfoKHR import={.sType=VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
        .handleType=external.handleTypes,.fd=fd};
    VkMemoryAllocateInfo allocate={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.pNext=&import,
        .allocationSize=needs.size,.memoryTypeIndex=index};
    if(vkAllocateMemory(device,&allocate,NULL,&out->memory)!=VK_SUCCESS){close(fd);return 0;}
    out->flags=memory->memoryTypes[index].propertyFlags;
    return vkBindBufferMemory(device,out->buffer,out->memory,0)==VK_SUCCESS &&
        vkMapMemory(device,out->memory,0,VK_WHOLE_SIZE,0,&out->mapped)==VK_SUCCESS;
}

static int cache_probe_query(int node,int fd,uint32_t omit,uint64_t size,
                             uint64_t expectedToDevice,uint64_t expectedToCpu)
{
    struct BrookBdwCacheProbe request={.operation=BROOK_BDW_CACHE_PROBE_QUERY,.fd=fd};
    if(ioctl(node,BROOK_BDW_CACHE_PROBE_IOCTL,&request))return 0;
    if(request.bytes==size && request.omit==omit && request.skippedToDevice==expectedToDevice &&
       request.skippedToCpu==expectedToCpu)return 1;
    fprintf(stderr,"CACHE_PROBE_COUNT_MISMATCH omit=%u size=%llu to_device=%llu to_cpu=%llu expected=%llu/%llu\n",
            request.omit,(unsigned long long)request.bytes,(unsigned long long)request.skippedToDevice,
            (unsigned long long)request.skippedToCpu,(unsigned long long)expectedToDevice,
            (unsigned long long)expectedToCpu);
    return 0;
}
