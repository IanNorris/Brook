/* Read a real Intel GEM PRIME buffer from Vulkan without CPU readback. */
#define _GNU_SOURCE
#include <vulkan/vulkan.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#define IOC(n,s) ((3u<<30)|((unsigned)(s)<<16)|('d'<<8)|(n))
struct gem_create {uint64_t size;uint32_t handle,pad;};
struct gem_map {uint32_t handle,pad;uint64_t offset,flags,extensions;};
struct prime {uint32_t handle,flags;int32_t fd;};
struct gem_close {uint32_t handle,pad;};
struct context {uint32_t id,pad;};
struct object {uint32_t handle,reloc_count;uint64_t relocs,alignment,offset,flags,reserved1,reserved2;};
struct exec {uint64_t objects;uint32_t count,start,length,dr1,dr4,clip_count;uint64_t clips,flags,context,fences;};
struct wait_gem {uint32_t handle,flags;int64_t timeout;};
struct sync_object {uint32_t handle,flags;};
struct exec_fence {uint32_t handle,flags;};
struct sync_handle {uint32_t handle,flags;int32_t fd;uint32_t pad;};
#define REQUIRE(x) do{if(!(x)){fprintf(stderr,"DMABUF_TEST_FAIL line=%d errno=%d\n",__LINE__,errno);goto done;}}while(0)
#define VK_CHECK(x) do{VkResult e=(x);if(e!=VK_SUCCESS){fprintf(stderr,"DMABUF_VK_FAIL line=%d result=%d\n",__LINE__,e);goto done;}}while(0)
int main(void){
    int drm=-1,fd=-1,syncFd=-1,passed=0,submitted=0,gpuSync=getenv("BROOK_GPU_SYNC_FD")!=NULL;
    int gpuProducer=gpuSync || getenv("BROOK_GPU_PRODUCER")!=NULL;
    uint32_t *source=MAP_FAILED,*batchMap=MAP_FAILED;void *mapped=NULL;
    struct gem_create gem={4096,0,0};
    struct gem_create batch={4096,0,0};struct context ctx={0};
    struct sync_object sync={0,0};
    VkInstance instance=VK_NULL_HANDLE;VkDevice device=VK_NULL_HANDLE;VkBuffer input=VK_NULL_HANDLE,output=VK_NULL_HANDLE;
    VkDeviceMemory imported=VK_NULL_HANDLE,host=VK_NULL_HANDLE;VkCommandPool pool=VK_NULL_HANDLE;VkFence fence=VK_NULL_HANDLE;
    VkSemaphore semaphore=VK_NULL_HANDLE;
    drm=open("/dev/dri/renderD129",O_RDWR|O_CLOEXEC);REQUIRE(drm>=0);
    REQUIRE(!ioctl(drm,IOC(0x5b,sizeof(gem)),&gem));
    struct gem_map map={gem.handle,0,0,2,0};REQUIRE(!ioctl(drm,IOC(0x64,sizeof(map)),&map));
    source=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED,drm,(off_t)map.offset);REQUIRE(source!=MAP_FAILED);
    for(unsigned i=0;i<1024;++i)source[i]=0x7733aa55u^(i*0x10203u);
    if(gpuProducer){
        REQUIRE(!ioctl(drm,IOC(0x6d,sizeof(ctx)),&ctx) && ctx.id);
        REQUIRE(!ioctl(drm,IOC(0x5b,sizeof(batch)),&batch));
        struct gem_map bm={batch.handle,0,0,2,0};REQUIRE(!ioctl(drm,IOC(0x64,sizeof(bm)),&bm));
        batchMap=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED,drm,(off_t)bm.offset);REQUIRE(batchMap!=MAP_FAILED);
        batchMap[0]=0x10000002;batchMap[1]=0x40011000;batchMap[2]=0;
        batchMap[3]=0x53544f52;batchMap[4]=0x05000000;batchMap[5]=0;
        struct object objects[]={{gem.handle,0,0,4096,0x40011000,20,0,0},{batch.handle,0,0,4096,0x10000,16,0,0}};
        struct exec request={(uintptr_t)objects,2,0,24,0,0,0,0,1u<<11,ctx.id,0};
        struct exec_fence outputFence={0,2};
        if(gpuSync){
            REQUIRE(!ioctl(drm,IOC(0xbf,sizeof(sync)),&sync) && sync.handle);
            outputFence.handle=sync.handle;request.clip_count=1;request.clips=(uintptr_t)&outputFence;request.flags|=1u<<19;
        }
        submitted=1;REQUIRE(!ioctl(drm,IOC(0x69,sizeof(request)),&request));
        if(gpuSync){
            struct sync_handle handle={sync.handle,1,-1,0};
            REQUIRE(!ioctl(drm,IOC(0xc1,sizeof(handle)),&handle) && handle.fd>=0);syncFd=handle.fd;
            struct pollfd pollFence={.fd=syncFd,.events=POLLIN};int ready=poll(&pollFence,1,0);
            REQUIRE(ready>=0);fprintf(stderr,"PRODUCER_FENCE_INITIAL ready=%d revents=%d\n",ready,pollFence.revents);
        } else {
            struct wait_gem wait={gem.handle,0,5000000000ll};REQUIRE(!ioctl(drm,IOC(0x6c,sizeof(wait)),&wait));
            submitted=0;
        }
    }
    struct prime export={gem.handle,O_CLOEXEC|O_RDWR,-1};REQUIRE(!ioctl(drm,IOC(0x2d,sizeof(export)),&export) && export.fd>=0);fd=export.fd;
    errno=0;off_t length=lseek(fd,0,SEEK_END);int seek_errno=errno;
    fprintf(stderr,"DMABUF_SIZE length=%lld errno=%d\n",(long long)length,seek_errno);
    if(length>=0)REQUIRE(lseek(fd,0,SEEK_SET)==0);
    VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.apiVersion=VK_API_VERSION_1_1};
    VkInstanceCreateInfo ii={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app};
    VK_CHECK(vkCreateInstance(&ii,NULL,&instance));
    uint32_t count=0;VK_CHECK(vkEnumeratePhysicalDevices(instance,&count,NULL));REQUIRE(count && count<=32);
    VkPhysicalDevice devices[32],physical=VK_NULL_HANDLE;VK_CHECK(vkEnumeratePhysicalDevices(instance,&count,devices));
    for(uint32_t i=0;i<count;++i){VkPhysicalDeviceProperties p;vkGetPhysicalDeviceProperties(devices[i],&p);
        if(p.vendorID==0x8086 && p.deviceID==0x1616){physical=devices[i];break;}}
    REQUIRE(physical);
    uint32_t families=0;vkGetPhysicalDeviceQueueFamilyProperties(physical,&families,NULL);REQUIRE(families && families<=64);
    VkQueueFamilyProperties queues[64];vkGetPhysicalDeviceQueueFamilyProperties(physical,&families,queues);
    uint32_t family=UINT32_MAX;for(uint32_t i=0;i<families;++i)if(queues[i].queueCount && (queues[i].queueFlags&VK_QUEUE_TRANSFER_BIT)){family=i;break;}
    REQUIRE(family!=UINT32_MAX);
    float priority=1;VkDeviceQueueCreateInfo qi={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=family,.queueCount=1,.pQueuePriorities=&priority};
    const char *extensions[]={VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME};
    VkDeviceCreateInfo di={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.queueCreateInfoCount=1,.pQueueCreateInfos=&qi,.enabledExtensionCount=gpuSync?3u:2u,.ppEnabledExtensionNames=extensions};
    VK_CHECK(vkCreateDevice(physical,&di,NULL,&device));VkQueue queue;vkGetDeviceQueue(device,family,0,&queue);
    if(gpuSync){
        VkSemaphoreCreateInfo sem={.sType=VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};VK_CHECK(vkCreateSemaphore(device,&sem,NULL,&semaphore));
        PFN_vkImportSemaphoreFdKHR importSemaphore=(PFN_vkImportSemaphoreFdKHR)vkGetDeviceProcAddr(device,"vkImportSemaphoreFdKHR");REQUIRE(importSemaphore);
        VkImportSemaphoreFdInfoKHR si={.sType=VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,.semaphore=semaphore,
            .flags=VK_SEMAPHORE_IMPORT_TEMPORARY_BIT,.handleType=VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,.fd=syncFd};
        VK_CHECK(importSemaphore(device,&si));syncFd=-1;
    }
    VkExternalMemoryBufferCreateInfo external={.sType=VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,.handleTypes=VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkBufferCreateInfo bi={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.pNext=&external,.size=4096,.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
    VK_CHECK(vkCreateBuffer(device,&bi,NULL,&input));bi.pNext=NULL;bi.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VK_CHECK(vkCreateBuffer(device,&bi,NULL,&output));
    VkMemoryRequirements inReq,outReq;vkGetBufferMemoryRequirements(device,input,&inReq);vkGetBufferMemoryRequirements(device,output,&outReq);
    REQUIRE(inReq.size<=4096);
    PFN_vkGetMemoryFdPropertiesKHR properties=(PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(device,"vkGetMemoryFdPropertiesKHR");REQUIRE(properties);
    VkMemoryFdPropertiesKHR fp={.sType=VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
    VK_CHECK(properties(device,VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,fd,&fp));
    VkPhysicalDeviceMemoryProperties memory;vkGetPhysicalDeviceMemoryProperties(physical,&memory);
    uint32_t inType=UINT32_MAX,outType=UINT32_MAX;
    for(uint32_t i=0;i<memory.memoryTypeCount;++i){
        if(inType==UINT32_MAX && (inReq.memoryTypeBits&fp.memoryTypeBits&(1u<<i)))inType=i;
        if(outType==UINT32_MAX && (outReq.memoryTypeBits&(1u<<i)) &&
           (memory.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))outType=i;
    }
    REQUIRE(inType!=UINT32_MAX && outType!=UINT32_MAX);
    VkImportMemoryFdInfoKHR imp={.sType=VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
        .handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,.fd=fd};
    VkMemoryAllocateInfo alloc={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.pNext=&imp,.allocationSize=4096,.memoryTypeIndex=inType};
    VK_CHECK(vkAllocateMemory(device,&alloc,NULL,&imported));fd=-1;
    VK_CHECK(vkBindBufferMemory(device,input,imported,0));
    alloc.pNext=NULL;alloc.allocationSize=outReq.size;alloc.memoryTypeIndex=outType;
    VK_CHECK(vkAllocateMemory(device,&alloc,NULL,&host));VK_CHECK(vkBindBufferMemory(device,output,host,0));
    VK_CHECK(vkMapMemory(device,host,0,VK_WHOLE_SIZE,0,&mapped));
    VkCommandPoolCreateInfo pi={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.queueFamilyIndex=family};
    VK_CHECK(vkCreateCommandPool(device,&pi,NULL,&pool));
    VkCommandBufferAllocateInfo ca={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    VkCommandBuffer cmd;VK_CHECK(vkAllocateCommandBuffers(device,&ca,&cmd));
    VkFenceCreateInfo fi={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};VK_CHECK(vkCreateFence(device,&fi,NULL,&fence));
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};VK_CHECK(vkBeginCommandBuffer(cmd,&begin));
    VkBufferMemoryBarrier acquire={.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,.srcAccessMask=0,.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_EXTERNAL,.dstQueueFamilyIndex=family,.buffer=input,.offset=0,.size=VK_WHOLE_SIZE};
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,1,&acquire,0,NULL);
    VkBufferCopy copy={.size=4096};vkCmdCopyBuffer(cmd,input,output,1,&copy);
    VkMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,NULL,0,NULL);
    VK_CHECK(vkEndCommandBuffer(cmd));VkPipelineStageFlags waitStage=VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&cmd,
        .waitSemaphoreCount=gpuSync?1u:0u,.pWaitSemaphores=gpuSync?&semaphore:NULL,.pWaitDstStageMask=gpuSync?&waitStage:NULL};
    submitted=1;VK_CHECK(vkQueueSubmit(queue,1,&submit,fence));VK_CHECK(vkWaitForFences(device,1,&fence,VK_TRUE,10000000000ULL));submitted=0;
    if(!(memory.memoryTypes[outType].propertyFlags&VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)){
        VkMappedMemoryRange range={.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,.memory=host,.size=VK_WHOLE_SIZE};VK_CHECK(vkInvalidateMappedMemoryRanges(device,1,&range));
    }
    for(unsigned i=0;i<1024;++i)REQUIRE(((uint32_t*)mapped)[i]==((gpuProducer && !i)?0x53544f52u:(0x7733aa55u^(i*0x10203u))));
    puts("INTEL_PRIME_VULKAN_IMPORT_PASS");passed=1;
done:
    if(submitted){fprintf(stderr,"DMABUF_TEST_UNCERTAIN resources retained\n");for(;;)pause();}
    if(device){
        if(mapped)vkUnmapMemory(device,host);
        if(fence)vkDestroyFence(device,fence,NULL);
        if(semaphore)vkDestroySemaphore(device,semaphore,NULL);
        if(pool)vkDestroyCommandPool(device,pool,NULL);
        if(input)vkDestroyBuffer(device,input,NULL);
        if(output)vkDestroyBuffer(device,output,NULL);
        if(imported)vkFreeMemory(device,imported,NULL);
        if(host)vkFreeMemory(device,host,NULL);
        vkDestroyDevice(device,NULL);
    }
    if(instance)vkDestroyInstance(instance,NULL);
    if(fd>=0)close(fd);
    if(syncFd>=0)close(syncFd);
    if(source!=MAP_FAILED)munmap(source,4096);
    if(batchMap!=MAP_FAILED)munmap(batchMap,4096);
    if(drm>=0){
        if(batch.handle){struct gem_close closeArg={batch.handle,0};ioctl(drm,IOC(9,sizeof(closeArg)),&closeArg);}
        if(sync.handle)ioctl(drm,IOC(0xc0,sizeof(sync)),&sync);
        if(ctx.id)ioctl(drm,IOC(0x6e,sizeof(ctx)),&ctx);
        if(gem.handle){struct gem_close closeArg={gem.handle,0};ioctl(drm,IOC(9,sizeof(closeArg)),&closeArg);}close(drm);
    }
    return passed?0:1;
}
