#define _POSIX_C_SOURCE 200809L
#include "renderer.h"
#include "scene.h"
#include <vulkan/vulkan.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <fcntl.h>
#include "compositor_spv.h"
#include "compositor_external_spv.h"
#include "compositor_multi_spv.h"
#include "compositor_output_spv.h"

enum { IMPORT_CACHE_SLOTS=16 };
#define IMPORT_CACHE_MAX_BYTES (128ull*1024*1024)
struct ImportCacheEntry {
    dev_t dev;ino_t ino;uint64_t bytes,stamp;int pin,active;
    VkBuffer buffer;VkDeviceMemory memory;
};

struct BrookVkRenderer {
    VkInstance instance; VkPhysicalDevice physical; VkDevice device; VkQueue queue;
    VkBuffer buffer; VkDeviceMemory memory; void *mapped; VkMemoryPropertyFlags flags;
    VkDescriptorSetLayout layout; VkDescriptorPool descriptors; VkDescriptorSet set;
    VkPipelineLayout pipelineLayout; VkPipeline pipeline; VkShaderModule shader;
    VkCommandPool pool; VkCommandBuffer command; VkFence fence;
    VkQueryPool timestampPool;uint32_t timestampBits;uint64_t gpuTimestampNs;int gpuTimestampValid,timestampPending,timestampDispatchOnly;
    VkPhysicalDeviceProperties properties; size_t maxWords; uint32_t maxTiles; int quarantined,pending,external,syncSupported;
    uint32_t family; VkBuffer source; VkDeviceMemory sourceMemory; VkSemaphore sourceSemaphore;
    VkBuffer multiSource[BROOK_VK_MAX_LAYERS+1]; VkDeviceMemory multiMemory[BROOK_VK_MAX_LAYERS+1];
    VkSemaphore multiSemaphore[BROOK_VK_MAX_LAYERS]; uint32_t multiCount;
    struct ImportCacheEntry cache[IMPORT_CACHE_SLOTS];
    int multiCache[BROOK_VK_MAX_LAYERS+1];uint64_t cacheBytes,cacheClock;
    struct BrookVkTiming timing;
    struct BrookVkFailure failure;
};

struct BrookVkTiming brook_vk_renderer_take_timing(BrookVkRenderer *r) {
    if(!r)return (struct BrookVkTiming){0};
    struct BrookVkTiming result=r->timing;r->timing=(struct BrookVkTiming){0};return result;
}

struct BrookVkFailure brook_vk_renderer_failure(const BrookVkRenderer *r) {
    return r?r->failure:(struct BrookVkFailure){0};
}
int brook_vk_renderer_gpu_timestamp_ns(const BrookVkRenderer *r,uint64_t *ns){
    if(!r || !ns || !r->gpuTimestampValid)return 0;
    *ns=r->gpuTimestampNs;return 1;
}

static uint64_t renderer_now_ns(void) {
    struct timespec t;
    return clock_gettime(CLOCK_MONOTONIC,&t) ? 0 : (uint64_t)t.tv_sec*1000000000ull+(uint64_t)t.tv_nsec;
}

/* Caller holds no uncertain work: an active entry belongs to this call and
 * cannot be evicted. Its duplicate fd pins the identity across producer reuse. */
static void cache_drop(BrookVkRenderer *r,unsigned index){
    struct ImportCacheEntry *e=&r->cache[index];if(e->pin<0)return;
    if(e->buffer)vkDestroyBuffer(r->device,e->buffer,NULL);
    if(e->memory)vkFreeMemory(r->device,e->memory,NULL);
    close(e->pin);r->cacheBytes-=e->bytes;
    *e=(struct ImportCacheEntry){.pin=-1};
}
static int cache_lookup(BrookVkRenderer *r,const struct stat *s,uint64_t bytes){
    for(unsigned i=0;i<IMPORT_CACHE_SLOTS;++i){struct ImportCacheEntry *e=&r->cache[i];
        if(e->pin>=0 && !e->active && e->dev==s->st_dev && e->ino==s->st_ino && e->bytes==bytes){
            e->active=1;e->stamp=++r->cacheClock;return (int)i;
        }}return -1;
}
static int cache_slot(BrookVkRenderer *r,uint64_t bytes){
    if(!bytes || bytes>IMPORT_CACHE_MAX_BYTES)return -1;
    for(;;){int freeSlot=-1,oldest=-1;
        for(unsigned i=0;i<IMPORT_CACHE_SLOTS;++i){struct ImportCacheEntry *e=&r->cache[i];
            if(e->pin<0){if(freeSlot<0)freeSlot=(int)i;}
            else if(!e->active && (oldest<0 || e->stamp<r->cache[oldest].stamp))oldest=(int)i;
        }
        if(freeSlot>=0 && r->cacheBytes<=IMPORT_CACHE_MAX_BYTES-bytes)return freeSlot;
        if(oldest<0)return -1;
        cache_drop(r,(unsigned)oldest);++r->timing.cache_evictions;
    }
}

int brook_vk_renderer_destroy(BrookVkRenderer *r) {
    if (!r) return 1;
    if (r->quarantined || r->pending) return 0;
    if(r->device) {
        if(r->mapped)vkUnmapMemory(r->device,r->memory);
        if(r->fence)vkDestroyFence(r->device,r->fence,NULL);
        if(r->timestampPool)vkDestroyQueryPool(r->device,r->timestampPool,NULL);
        if(r->pool)vkDestroyCommandPool(r->device,r->pool,NULL);
        if(r->pipeline)vkDestroyPipeline(r->device,r->pipeline,NULL);
        if(r->shader)vkDestroyShaderModule(r->device,r->shader,NULL);
        if(r->pipelineLayout)vkDestroyPipelineLayout(r->device,r->pipelineLayout,NULL);
        if(r->descriptors)vkDestroyDescriptorPool(r->device,r->descriptors,NULL);
        if(r->layout)vkDestroyDescriptorSetLayout(r->device,r->layout,NULL);
        if(r->source)vkDestroyBuffer(r->device,r->source,NULL);
        if(r->sourceMemory)vkFreeMemory(r->device,r->sourceMemory,NULL);
        if(r->sourceSemaphore)vkDestroySemaphore(r->device,r->sourceSemaphore,NULL);
        for(uint32_t i=0;i<BROOK_VK_MAX_LAYERS+1;++i){
            if(r->multiCache[i]<0 && r->multiSource[i])vkDestroyBuffer(r->device,r->multiSource[i],NULL);
            if(r->multiCache[i]<0 && r->multiMemory[i])vkFreeMemory(r->device,r->multiMemory[i],NULL);
            if(i<BROOK_VK_MAX_LAYERS && r->multiSemaphore[i])vkDestroySemaphore(r->device,r->multiSemaphore[i],NULL);
        }
        for(unsigned i=0;i<IMPORT_CACHE_SLOTS;++i)cache_drop(r,i);
        if(r->buffer)vkDestroyBuffer(r->device,r->buffer,NULL);
        if(r->memory)vkFreeMemory(r->device,r->memory,NULL);
        vkDestroyDevice(r->device,NULL);
    }
    if(r->instance)vkDestroyInstance(r->instance,NULL);
    free(r);return 1;
}

int brook_vk_renderer_retire_timeout(BrookVkRenderer **renderer) {
    if(!renderer || !*renderer)return 0;
    BrookVkRenderer *r=*renderer;
    // A failed submit or device loss is not evidence that GPU work retired.
    if(r->quarantined!=2)return 0;
    VkResult status=vkGetFenceStatus(r->device,r->fence);
    r->failure.fence_status=(int)status;r->failure.fence_status_valid=1;
    if(status!=VK_SUCCESS)return 0;
    r->quarantined=0;
    if(!brook_vk_renderer_destroy(r))return 0;
    *renderer=NULL;return 1;
}

static BrookVkRenderer *renderer_create(size_t maxWords,int allowSoftware,int external,uint32_t maxTiles) {
    if(maxWords<BROOK_VK_SCENE_HEADER || maxWords>UINT32_MAX || maxWords>SIZE_MAX/4)return NULL;
    if(!maxTiles || maxTiles>BROOK_VK_MAX_TILES || (maxTiles>1 && (external!=3 || maxWords<(size_t)maxTiles*BROOK_VK_TILE_STRIDE)))return NULL;
    /* Separate-output shaders access only metadata in the scene buffer;
     * retain maxWords as the logical validation bound, but do not allocate a
     * second full-screen output reservation in GPU-visible memory. */
    size_t sceneWords=maxWords;
    const size_t metadataWords=BROOK_VK_SCENE_HEADER+(size_t)BROOK_VK_MAX_LAYERS*BROOK_VK_LAYER_WORDS;
    if(external==3 && sceneWords>metadataWords*maxTiles)sceneWords=metadataWords*maxTiles;
    BrookVkRenderer *r=calloc(1,sizeof(*r)); if(!r)return NULL;
    for(unsigned i=0;i<IMPORT_CACHE_SLOTS;++i)r->cache[i].pin=-1;
    for(unsigned i=0;i<BROOK_VK_MAX_LAYERS+1;++i)r->multiCache[i]=-1;
#define VK_OK(call) do { if((call)!=VK_SUCCESS)goto fail; } while(0)
    VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName="Brook compositor renderer",.apiVersion=VK_API_VERSION_1_1};
    VkInstanceCreateInfo instance={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app};
    VK_OK(vkCreateInstance(&instance,NULL,&r->instance));
    uint32_t count=0;VK_OK(vkEnumeratePhysicalDevices(r->instance,&count,NULL));
    if(!count || count>32)goto fail;
    VkPhysicalDevice devices[32];VK_OK(vkEnumeratePhysicalDevices(r->instance,&count,devices));
    uint32_t family=UINT32_MAX;
    for(uint32_t i=0;i<count && !r->physical;++i) {
        VkPhysicalDeviceProperties props;vkGetPhysicalDeviceProperties(devices[i],&props);
        if(props.deviceType==VK_PHYSICAL_DEVICE_TYPE_CPU && !allowSoftware)continue;
        if((uint64_t)sceneWords*4>props.limits.maxStorageBufferRange)continue;
        if(external>=2){
            VkPhysicalDeviceFeatures features;vkGetPhysicalDeviceFeatures(devices[i],&features);
            const uint32_t descriptors=BROOK_VK_MAX_LAYERS+1u+(external==3?1u:0u);
            if(!features.shaderStorageBufferArrayDynamicIndexing ||
               props.limits.maxPerStageDescriptorStorageBuffers<descriptors ||
               props.limits.maxDescriptorSetStorageBuffers<descriptors ||
               props.limits.maxPerStageResources<descriptors)continue;
        }
        uint32_t n=0;vkGetPhysicalDeviceQueueFamilyProperties(devices[i],&n,NULL);
        if(!n || n>64)continue;
        VkQueueFamilyProperties queues[64];vkGetPhysicalDeviceQueueFamilyProperties(devices[i],&n,queues);
        for(uint32_t j=0;j<n;++j)if(queues[j].queueCount && (queues[j].queueFlags&VK_QUEUE_COMPUTE_BIT)) {
            r->physical=devices[i];r->properties=props;family=j;r->timestampBits=queues[j].timestampValidBits;break;
        }
    }
    if(!r->physical)goto fail;
    float priority=1;
    VkDeviceQueueCreateInfo qi={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex=family,.queueCount=1,.pQueuePriorities=&priority};
    const char *extensions[]={VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
                              VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME};
    uint32_t extensionCount=0;
    if(external){
        uint32_t n=0;VK_OK(vkEnumerateDeviceExtensionProperties(r->physical,NULL,&n,NULL));
        if(!n || n>4096)goto fail;
        VkExtensionProperties *available=calloc(n,sizeof(*available));if(!available)goto fail;
        VkResult result=vkEnumerateDeviceExtensionProperties(r->physical,NULL,&n,available);
        int memoryFd=0,dmaBuf=0;
        if(result==VK_SUCCESS)for(uint32_t i=0;i<n;++i){
            if(!strcmp(available[i].extensionName,extensions[0]))memoryFd=1;
            if(!strcmp(available[i].extensionName,extensions[1]))dmaBuf=1;
            if(!strcmp(available[i].extensionName,extensions[2]))r->syncSupported=1;
        }
        free(available);if(result!=VK_SUCCESS || !memoryFd || !dmaBuf)goto fail;
        extensionCount=r->syncSupported?3u:2u;
    }
    VkPhysicalDeviceFeatures enabled={0};if(external>=2)enabled.shaderStorageBufferArrayDynamicIndexing=VK_TRUE;
    VkDeviceCreateInfo di={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.queueCreateInfoCount=1,.pQueueCreateInfos=&qi,
        .pEnabledFeatures=external>=2?&enabled:NULL,
        .enabledExtensionCount=extensionCount,.ppEnabledExtensionNames=external?extensions:NULL};
    VK_OK(vkCreateDevice(r->physical,&di,NULL,&r->device));vkGetDeviceQueue(r->device,family,0,&r->queue);
    if(getenv("BROOK_RENDER_GPU_TIMESTAMPS")){
        r->timestampDispatchOnly=getenv("BROOK_RENDER_GPU_DISPATCH_ONLY")!=NULL;
        if(!r->timestampBits || r->timestampBits>64 || !(r->properties.limits.timestampPeriod>0))goto fail;
        VkQueryPoolCreateInfo query={.sType=VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,.queryType=VK_QUERY_TYPE_TIMESTAMP,.queryCount=2};
        VK_OK(vkCreateQueryPool(r->device,&query,NULL,&r->timestampPool));
    }
    r->family=family;r->external=external;
    VkBufferCreateInfo bi={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=(VkDeviceSize)sceneWords*4,
        .usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
    VK_OK(vkCreateBuffer(r->device,&bi,NULL,&r->buffer));
    VkMemoryRequirements needs;vkGetBufferMemoryRequirements(r->device,r->buffer,&needs);
    VkPhysicalDeviceMemoryProperties memory;vkGetPhysicalDeviceMemoryProperties(r->physical,&memory);
    uint32_t index=UINT32_MAX;
    for(uint32_t i=0;i<memory.memoryTypeCount;++i)if((needs.memoryTypeBits&(1u<<i)) &&
        (memory.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
        if(index==UINT32_MAX || (memory.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))index=i;
        if(memory.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)break;
    }
    if(index==UINT32_MAX)goto fail;
    r->flags=memory.memoryTypes[index].propertyFlags;
    VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=needs.size,.memoryTypeIndex=index};
    VK_OK(vkAllocateMemory(r->device,&ai,NULL,&r->memory));
    VK_OK(vkBindBufferMemory(r->device,r->buffer,r->memory,0));
    VK_OK(vkMapMemory(r->device,r->memory,0,VK_WHOLE_SIZE,0,&r->mapped));
    VkDescriptorSetLayoutBinding binding[3]={{.binding=0,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount=1,.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.descriptorCount=external>=2?BROOK_VK_MAX_LAYERS:1,.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding=2,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT}};
    VkDescriptorSetLayoutCreateInfo li={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount=external==3?3u:external?2u:1u,.pBindings=binding};
    VK_OK(vkCreateDescriptorSetLayout(r->device,&li,NULL,&r->layout));
    VkPushConstantRange push={.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT,.offset=0,.size=4};
    VkPipelineLayoutCreateInfo pi={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount=1,.pSetLayouts=&r->layout,.pushConstantRangeCount=external==3?1u:0u,.pPushConstantRanges=external==3?&push:NULL};
    VK_OK(vkCreatePipelineLayout(r->device,&pi,NULL,&r->pipelineLayout));
    VkShaderModuleCreateInfo si={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize=external==3?sizeof(compositorOutputCode):external==2?sizeof(compositorMultiCode):external?sizeof(compositorExternalCode):sizeof(compositorCode),
        .pCode=external==3?compositorOutputCode:external==2?compositorMultiCode:external?compositorExternalCode:compositorCode};
    VK_OK(vkCreateShaderModule(r->device,&si,NULL,&r->shader));
    VkComputePipelineCreateInfo ci={.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_COMPUTE_BIT,
                .module=r->shader,.pName="main"},.layout=r->pipelineLayout};
    VK_OK(vkCreateComputePipelines(r->device,VK_NULL_HANDLE,1,&ci,NULL,&r->pipeline));
    VkDescriptorPoolSize ps={VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,external>=2?BROOK_VK_MAX_LAYERS+1u+(external==3?1u:0u):external?2u:1u};
    VkDescriptorPoolCreateInfo dp={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets=1,.poolSizeCount=1,.pPoolSizes=&ps};
    VK_OK(vkCreateDescriptorPool(r->device,&dp,NULL,&r->descriptors));
    VkDescriptorSetAllocateInfo ds={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool=r->descriptors,.descriptorSetCount=1,.pSetLayouts=&r->layout};
    VK_OK(vkAllocateDescriptorSets(r->device,&ds,&r->set));
    VkDescriptorBufferInfo db={r->buffer,0,(VkDeviceSize)sceneWords*4};
    VkWriteDescriptorSet write={.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=r->set,
        .dstBinding=0,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.pBufferInfo=&db};
    vkUpdateDescriptorSets(r->device,1,&write,0,NULL);
    VkCommandPoolCreateInfo cp={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,.queueFamilyIndex=family};
    VK_OK(vkCreateCommandPool(r->device,&cp,NULL,&r->pool));
    VkCommandBufferAllocateInfo ca={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool=r->pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    VK_OK(vkAllocateCommandBuffers(r->device,&ca,&r->command));
    VkFenceCreateInfo fi={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VK_OK(vkCreateFence(r->device,&fi,NULL,&r->fence));
    r->maxWords=maxWords;r->maxTiles=maxTiles;return r;
fail:
    brook_vk_renderer_destroy(r);return NULL;
#undef VK_OK
}

BrookVkRenderer *brook_vk_renderer_create(size_t maxWords,int allowSoftware) {return renderer_create(maxWords,allowSoftware,0,1);}
BrookVkRenderer *brook_vk_renderer_create_external(size_t maxWords,int allowSoftware) {return renderer_create(maxWords,allowSoftware,1,1);}
BrookVkRenderer *brook_vk_renderer_create_multi(size_t maxWords,int allowSoftware) {return renderer_create(maxWords,allowSoftware,2,1);}
BrookVkRenderer *brook_vk_renderer_create_multi_output(size_t maxWords,int allowSoftware) {return renderer_create(maxWords,allowSoftware,3,1);}
BrookVkRenderer *brook_vk_renderer_create_batch_output(uint32_t maxTiles,int allowSoftware) {
    return renderer_create((size_t)maxTiles*BROOK_VK_TILE_STRIDE,allowSoftware,3,maxTiles);
}

const char *brook_vk_renderer_device(const BrookVkRenderer *r) {return r?r->properties.deviceName:NULL;}
int brook_vk_renderer_graphics_context(BrookVkRenderer *r,VkPhysicalDevice *physical,VkDevice *device,VkQueue *queue,uint32_t *family){
    if(!r || !physical || !device || !queue || !family || r->quarantined)return 0;
    uint32_t count=0;vkGetPhysicalDeviceQueueFamilyProperties(r->physical,&count,NULL);
    if(!count || count>64 || r->family>=count)return 0;
    VkQueueFamilyProperties properties[64];vkGetPhysicalDeviceQueueFamilyProperties(r->physical,&count,properties);
    if(!(properties[r->family].queueFlags&VK_QUEUE_GRAPHICS_BIT))return 0;
    *physical=r->physical;*device=r->device;*queue=r->queue;*family=r->family;return 1;
}

static int valid_scene(BrookVkRenderer *r,const uint32_t *scene,size_t words,size_t sourceWords,const size_t *multiWords) {
    return r->external==3?brook_vk_scene_valid_multi_output(scene,words,multiWords,r->multiCount):
           r->external==2?brook_vk_scene_valid_multi(scene,words,multiWords,r->multiCount):
           r->external?brook_vk_scene_valid_external(scene,words,sourceWords):brook_vk_scene_valid(scene,words);
}
static int valid_batch(const uint32_t *scene,size_t words,const size_t *sizes,size_t sources,uint32_t tiles,size_t outputWords){
    if(!scene || !sizes || !tiles || tiles>BROOK_VK_MAX_TILES ||
       (tiles>1 && words<(size_t)tiles*BROOK_VK_TILE_STRIDE))return 0;
    for(uint32_t i=0;i<tiles;++i){
        const uint32_t *s=scene+(size_t)i*BROOK_VK_TILE_STRIDE;
        size_t available=tiles==1?words:BROOK_VK_TILE_STRIDE;
        if(!brook_vk_scene_valid_multi_output(s,available,sizes,sources))return 0;
        const uint64_t first=s[3],end=first+(uint64_t)s[0]*s[1];
        if(end>outputWords)return 0;
        for(uint32_t j=0;j<i;++j){const uint32_t *p=scene+(size_t)j*BROOK_VK_TILE_STRIDE;
            const uint64_t pfirst=p[3],pend=pfirst+(uint64_t)p[0]*p[1];
            if(first<pend && pfirst<end)return 0;
        }
    }
    return 1;
}
static BrookVkResult compose(BrookVkRenderer *r,const uint32_t *scene,size_t words,size_t sourceWords,const size_t *multiWords,
                             uint32_t *output,size_t outputWords,uint64_t timeoutNs,uint32_t tiles,int asynchronous) {
    if(!r || r->quarantined || r->pending || !scene || (!output && r->external!=3) || words>r->maxWords ||
       !tiles || tiles>r->maxTiles || (r->external!=3 && tiles!=1) ||
       !(r->external==3?valid_batch(scene,words,multiWords,r->multiCount,tiles,outputWords):
                         valid_scene(r,scene,words,sourceWords,multiWords)))return BROOK_VK_REJECTED;
    /* The separate-output shader reads only scene metadata from this buffer.
     * Its pixels live in imported buffers, so do not copy the unused
     * full-screen output reservation on every frame.  Validate first, then
     * validate the copied metadata again as for the other paths. */
    if(r->external==3){for(uint32_t i=0;i<tiles;++i){const size_t offset=(size_t)i*BROOK_VK_TILE_STRIDE;
        const size_t copied=BROOK_VK_SCENE_HEADER+(size_t)scene[offset+2]*BROOK_VK_LAYER_WORDS;
        memcpy((uint32_t*)r->mapped+offset,scene+offset,copied*4);}}
    else memcpy(r->mapped,scene,words*4);
    const uint32_t *local=r->mapped;
    if(!(r->external==3?valid_batch(local,words,multiWords,r->multiCount,tiles,outputWords):
                         valid_scene(r,local,words,sourceWords,multiWords)) ||
       (r->external!=3 && (uint64_t)local[0]*local[1]>outputWords))return BROOK_VK_REJECTED;
    uint64_t pixels=(uint64_t)local[0]*local[1];
    uint32_t maxX=r->properties.limits.maxComputeWorkGroupCount[0];
#ifdef BROOK_VK_RENDER_TEST_MAX_GROUPS
    /* Test-only: exercise the two-dimensional dispatch on a small scene. */
    if(maxX>BROOK_VK_RENDER_TEST_MAX_GROUPS)maxX=BROOK_VK_RENDER_TEST_MAX_GROUPS;
#endif
    if(!maxX)return BROOK_VK_REJECTED;
    uint32_t dispatchX[BROOK_VK_MAX_TILES],dispatchY[BROOK_VK_MAX_TILES];
    for(uint32_t i=0;i<tiles;++i){const uint32_t *s=local+(size_t)i*BROOK_VK_TILE_STRIDE;
        const uint64_t groups=((uint64_t)s[0]*s[1]+63)/64;
        dispatchX[i]=groups<maxX?(uint32_t)groups:maxX;
        const uint64_t y=(groups+dispatchX[i]-1)/dispatchX[i];
        if(!y || y>r->properties.limits.maxComputeWorkGroupCount[1])return BROOK_VK_REJECTED;
        dispatchY[i]=(uint32_t)y;
    }
    VkMappedMemoryRange range={.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,.memory=r->memory,.size=VK_WHOLE_SIZE};
    if(!(r->flags&VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) && vkFlushMappedMemoryRanges(r->device,1,&range)!=VK_SUCCESS)
        return BROOK_VK_REJECTED;
    if(vkResetCommandBuffer(r->command,0)!=VK_SUCCESS)return BROOK_VK_REJECTED;
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    if(vkBeginCommandBuffer(r->command,&begin)!=VK_SUCCESS)return BROOK_VK_REJECTED;
    r->gpuTimestampValid=0;r->timestampPending=asynchronous && r->timestampPool;
    if(r->timestampPending){vkCmdResetQueryPool(r->command,r->timestampPool,0,2);
        if(!r->timestampDispatchOnly)vkCmdWriteTimestamp(r->command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,r->timestampPool,0);}
    VkMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT,
        .dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT};
    vkCmdPipelineBarrier(r->command,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&barrier,0,NULL,0,NULL);
    if(r->external==1){
        VkBufferMemoryBarrier acquire={.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,.dstAccessMask=VK_ACCESS_SHADER_READ_BIT,
            .srcQueueFamilyIndex=VK_QUEUE_FAMILY_EXTERNAL,.dstQueueFamilyIndex=r->family,.buffer=r->source,.size=VK_WHOLE_SIZE};
        vkCmdPipelineBarrier(r->command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,NULL,1,&acquire,0,NULL);
    }
    if(r->external>=2)for(uint32_t i=0;i<r->multiCount;++i){
        VkBufferMemoryBarrier acquire={.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,.dstAccessMask=VK_ACCESS_SHADER_READ_BIT,
            .srcQueueFamilyIndex=VK_QUEUE_FAMILY_EXTERNAL,.dstQueueFamilyIndex=r->family,.buffer=r->multiSource[i],.size=VK_WHOLE_SIZE};
        vkCmdPipelineBarrier(r->command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,NULL,1,&acquire,0,NULL);
    }
    if(r->external==3){
        VkBufferMemoryBarrier acquire={.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,.dstAccessMask=VK_ACCESS_SHADER_WRITE_BIT,
            .srcQueueFamilyIndex=VK_QUEUE_FAMILY_EXTERNAL,.dstQueueFamilyIndex=r->family,.buffer=r->multiSource[BROOK_VK_MAX_LAYERS],.size=VK_WHOLE_SIZE};
        vkCmdPipelineBarrier(r->command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,NULL,1,&acquire,0,NULL);
    }
    if(r->timestampPending && r->timestampDispatchOnly)
        vkCmdWriteTimestamp(r->command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,r->timestampPool,0);
    vkCmdBindPipeline(r->command,VK_PIPELINE_BIND_POINT_COMPUTE,r->pipeline);
    vkCmdBindDescriptorSets(r->command,VK_PIPELINE_BIND_POINT_COMPUTE,r->pipelineLayout,0,1,&r->set,0,NULL);
    for(uint32_t i=0;i<tiles;++i){
        if(r->external==3){const uint32_t base=i*BROOK_VK_TILE_STRIDE;
            vkCmdPushConstants(r->command,r->pipelineLayout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(base),&base);}
        vkCmdDispatch(r->command,dispatchX[i],dispatchY[i],1);
    }
    if(r->timestampPending && r->timestampDispatchOnly)
        vkCmdWriteTimestamp(r->command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,r->timestampPool,1);
    if(r->external==1){
        VkBufferMemoryBarrier release={.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_SHADER_READ_BIT,
            .srcQueueFamilyIndex=r->family,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_EXTERNAL,.buffer=r->source,.size=VK_WHOLE_SIZE};
        vkCmdPipelineBarrier(r->command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0,0,NULL,1,&release,0,NULL);
    }
    if(r->external>=2)for(uint32_t i=0;i<r->multiCount;++i){
        VkBufferMemoryBarrier release={.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_SHADER_READ_BIT,
            .srcQueueFamilyIndex=r->family,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_EXTERNAL,.buffer=r->multiSource[i],.size=VK_WHOLE_SIZE};
        vkCmdPipelineBarrier(r->command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0,0,NULL,1,&release,0,NULL);
    }
    if(r->external==3){
        VkBufferMemoryBarrier release={.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT,
            .srcQueueFamilyIndex=r->family,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_EXTERNAL,.buffer=r->multiSource[BROOK_VK_MAX_LAYERS],.size=VK_WHOLE_SIZE};
        vkCmdPipelineBarrier(r->command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0,0,NULL,1,&release,0,NULL);
    }
    barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(r->command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,NULL,0,NULL);
    if(r->timestampPending && !r->timestampDispatchOnly)
        vkCmdWriteTimestamp(r->command,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,r->timestampPool,1);
    if(vkEndCommandBuffer(r->command)!=VK_SUCCESS || vkResetFences(r->device,1,&r->fence)!=VK_SUCCESS)return BROOK_VK_REJECTED;
    VkPipelineStageFlags waitStage=VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    VkSemaphore waits[BROOK_VK_MAX_LAYERS];VkPipelineStageFlags stages[BROOK_VK_MAX_LAYERS];uint32_t waitCount=0;
    if(r->external>=2)for(uint32_t i=0;i<r->multiCount;++i)if(r->multiSemaphore[i]){
        waits[waitCount]=r->multiSemaphore[i];stages[waitCount++]=waitStage;
    }
    VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&r->command,
        .waitSemaphoreCount=r->external>=2?waitCount:r->sourceSemaphore?1u:0u,
        .pWaitSemaphores=r->external>=2?waits:r->sourceSemaphore?&r->sourceSemaphore:NULL,
        .pWaitDstStageMask=r->external>=2?stages:r->sourceSemaphore?&waitStage:NULL};
    uint64_t submitted=renderer_now_ns();
    VkResult submitResult=vkQueueSubmit(r->queue,1,&submit,r->fence);
    if(submitResult!=VK_SUCCESS) {
        r->failure=(struct BrookVkFailure){BROOK_VK_FAILURE_SUBMIT,(int)submitResult,0,0};
        r->quarantined=1;return BROOK_VK_UNCERTAIN;
    }
    if(r->external==3 && tiles>1 && getenv("BROOK_RENDER_TEST_KILL_BATCH")){
        fprintf(stderr,"BROOK_VK_INJECT_KILL_AFTER_SUBMIT tiles=%u\n",tiles);
        (void)kill(getpid(),SIGKILL);for(;;)pause();
    }
    if(asynchronous){r->pending=1;return BROOK_VK_PENDING;}
    static int testTimeoutObserved=0;
    const int testTimeout=r->external==3 && getenv("BROOK_RENDER_TEST_LATE_FENCE") && !testTimeoutObserved;
    VkResult waitResult=vkWaitForFences(r->device,1,&r->fence,VK_TRUE,testTimeout?0:timeoutNs);
    if(testTimeout && waitResult==VK_TIMEOUT){
        testTimeoutObserved=1;fprintf(stderr,"BROOK_VK_TEST_LATE_FENCE_TIMEOUT\n");
    }
    if(waitResult!=VK_SUCCESS) {
        r->failure=(struct BrookVkFailure){BROOK_VK_FAILURE_WAIT,(int)waitResult,0,0};
        r->quarantined=waitResult==VK_TIMEOUT?2:1;return BROOK_VK_UNCERTAIN;
    }
    if(r->external==3 && getenv("BROOK_RENDER_TIMING")) {
        uint64_t completed=renderer_now_ns();
        if(submitted && completed>=submitted)fprintf(stderr,"BROOK_VK_TIMING submit_wait_ms=%lu\n",(unsigned long)((completed-submitted)/1000000ull));
    }
    if(r->external!=3 && !(r->flags&VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) && vkInvalidateMappedMemoryRanges(r->device,1,&range)!=VK_SUCCESS)
        return BROOK_VK_REJECTED;
    if(r->external!=3)memcpy(output,local+local[3],(size_t)pixels*4);
    return BROOK_VK_COMPLETE;
}

BrookVkResult brook_vk_renderer_compose(BrookVkRenderer *r,const uint32_t *scene,size_t words,
                                        uint32_t *output,size_t outputWords,uint64_t timeoutNs) {
    if(!r || r->external)return BROOK_VK_REJECTED;
    return compose(r,scene,words,0,NULL,output,outputWords,timeoutNs,1,0);
}

static BrookVkResult compose_external(BrookVkRenderer *r,const uint32_t *scene,size_t words,
        int fd,size_t sourceWords,int syncFd,uint32_t *output,size_t outputWords,uint64_t timeoutNs) {
    if(!r || r->external!=1 || r->quarantined || r->source || (syncFd>=0 && !r->syncSupported) || fd<0 || !sourceWords || sourceWords>UINT32_MAX ||
       sourceWords>SIZE_MAX/4 || (uint64_t)sourceWords*4>r->properties.limits.maxStorageBufferRange ||
       !brook_vk_scene_valid_external(scene,words,sourceWords) || !output ||
       (uint64_t)scene[0]*scene[1]>outputWords)return BROOK_VK_REJECTED;
    off_t size=lseek(fd,0,SEEK_END);
    if(size<0 || (uint64_t)size!=(uint64_t)sourceWords*4 || lseek(fd,0,SEEK_SET)!=0)return BROOK_VK_REJECTED;
    int owned=dup(fd);if(owned<0)return BROOK_VK_REJECTED;
    BrookVkResult result=BROOK_VK_REJECTED;
    if(syncFd>=0){
        int ownedSync=dup(syncFd);if(ownedSync<0)goto done;
        VkSemaphoreCreateInfo si={.sType=VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if(vkCreateSemaphore(r->device,&si,NULL,&r->sourceSemaphore)!=VK_SUCCESS){close(ownedSync);goto done;}
        PFN_vkImportSemaphoreFdKHR import=(PFN_vkImportSemaphoreFdKHR)vkGetDeviceProcAddr(r->device,"vkImportSemaphoreFdKHR");
        if(!import){close(ownedSync);goto done;}
        VkImportSemaphoreFdInfoKHR info={.sType=VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,.semaphore=r->sourceSemaphore,
            .flags=VK_SEMAPHORE_IMPORT_TEMPORARY_BIT,.handleType=VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,.fd=ownedSync};
        if(import(r->device,&info)!=VK_SUCCESS){close(ownedSync);goto done;}
    }
    VkExternalMemoryBufferCreateInfo ext={.sType=VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
        .handleTypes=VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkBufferCreateInfo bi={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.pNext=&ext,.size=(VkDeviceSize)size,
        .usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
    if(vkCreateBuffer(r->device,&bi,NULL,&r->source)!=VK_SUCCESS)goto done;
    VkMemoryRequirements need;vkGetBufferMemoryRequirements(r->device,r->source,&need);
    if(need.size>(VkDeviceSize)size)goto done;
    PFN_vkGetMemoryFdPropertiesKHR getProperties=(PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(r->device,"vkGetMemoryFdPropertiesKHR");
    if(!getProperties)goto done;
    VkMemoryFdPropertiesKHR fp={.sType=VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
    if(getProperties(r->device,VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,owned,&fp)!=VK_SUCCESS)goto done;
    VkPhysicalDeviceMemoryProperties memory;vkGetPhysicalDeviceMemoryProperties(r->physical,&memory);
    uint32_t index=UINT32_MAX;
    for(uint32_t i=0;i<memory.memoryTypeCount;++i)if((need.memoryTypeBits&fp.memoryTypeBits)&(1u<<i)){index=i;break;}
    if(index==UINT32_MAX)goto done;
    VkImportMemoryFdInfoKHR imp={.sType=VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,.handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,.fd=owned};
    VkMemoryAllocateInfo alloc={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.pNext=&imp,.allocationSize=(VkDeviceSize)size,.memoryTypeIndex=index};
    if(vkAllocateMemory(r->device,&alloc,NULL,&r->sourceMemory)!=VK_SUCCESS)goto done;
    owned=-1;
    if(vkBindBufferMemory(r->device,r->source,r->sourceMemory,0)!=VK_SUCCESS)goto done;
    VkDescriptorBufferInfo db={r->source,0,(VkDeviceSize)size};
    VkWriteDescriptorSet write={.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=r->set,.dstBinding=1,
        .descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.pBufferInfo=&db};
    vkUpdateDescriptorSets(r->device,1,&write,0,NULL);
    result=compose(r,scene,words,sourceWords,NULL,output,outputWords,timeoutNs,1,0);
done:
    if(owned>=0)close(owned);
    if(!r->quarantined){
        if(r->source)vkDestroyBuffer(r->device,r->source,NULL);
        if(r->sourceMemory)vkFreeMemory(r->device,r->sourceMemory,NULL);
        if(r->sourceSemaphore)vkDestroySemaphore(r->device,r->sourceSemaphore,NULL);
        r->source=VK_NULL_HANDLE;r->sourceMemory=VK_NULL_HANDLE;
        r->sourceSemaphore=VK_NULL_HANDLE;
    }
    return result;
}

BrookVkResult brook_vk_renderer_compose_external(BrookVkRenderer *r,const uint32_t *scene,size_t words,
        int fd,size_t sourceWords,uint32_t *output,size_t outputWords,uint64_t timeoutNs) {
    return compose_external(r,scene,words,fd,sourceWords,-1,output,outputWords,timeoutNs);
}
BrookVkResult brook_vk_renderer_compose_external_sync(BrookVkRenderer *r,const uint32_t *scene,size_t words,
        int fd,size_t sourceWords,int syncFd,uint32_t *output,size_t outputWords,uint64_t timeoutNs) {
    if(syncFd<0)return BROOK_VK_REJECTED;
    return compose_external(r,scene,words,fd,sourceWords,syncFd,output,outputWords,timeoutNs);
}

static void multi_cleanup(BrookVkRenderer *r){
    for(uint32_t i=0;i<r->multiCount;++i){
        if(r->multiCache[i]<0 && r->multiSource[i])vkDestroyBuffer(r->device,r->multiSource[i],NULL);
        if(r->multiCache[i]<0 && r->multiMemory[i])vkFreeMemory(r->device,r->multiMemory[i],NULL);
        if(r->multiCache[i]>=0)r->cache[r->multiCache[i]].active=0;
        if(r->multiSemaphore[i])vkDestroySemaphore(r->device,r->multiSemaphore[i],NULL);
        r->multiSource[i]=VK_NULL_HANDLE;r->multiMemory[i]=VK_NULL_HANDLE;r->multiSemaphore[i]=VK_NULL_HANDLE;r->multiCache[i]=-1;
    }
    const unsigned out=BROOK_VK_MAX_LAYERS;
    if(r->multiCache[out]<0 && r->multiSource[out])vkDestroyBuffer(r->device,r->multiSource[out],NULL);
    if(r->multiCache[out]<0 && r->multiMemory[out])vkFreeMemory(r->device,r->multiMemory[out],NULL);
    if(r->multiCache[out]>=0)r->cache[r->multiCache[out]].active=0;
    r->multiSource[out]=VK_NULL_HANDLE;r->multiMemory[out]=VK_NULL_HANDLE;r->multiCache[out]=-1;r->multiCount=0;
}

static BrookVkResult compose_multi(BrookVkRenderer *r,const uint32_t *scene,size_t words,
        const struct BrookVkExternalSource *sources,size_t count,uint32_t *output,int outputFd,size_t outputWords,uint64_t timeoutNs,uint32_t tiles,int asynchronous) {
    uint64_t start=renderer_now_ns(),imported=0,composed=0;
    if(!r || (r->external!=2 && r->external!=3) || r->quarantined || r->multiCount || !sources || !count || count>BROOK_VK_MAX_LAYERS ||
       !scene || !tiles || tiles>r->maxTiles || (r->external==2 && (!output || tiles!=1)) || (r->external==3 && outputFd<0) || words>r->maxWords)return BROOK_VK_REJECTED;
    size_t sizes[BROOK_VK_MAX_LAYERS];struct stat identity[BROOK_VK_MAX_LAYERS+1];
    for(size_t i=0;i<count;++i){
        sizes[i]=sources[i].words;
        if(sources[i].fd<0 || !sizes[i] || sizes[i]>UINT32_MAX || sizes[i]>SIZE_MAX/4 ||
           (uint64_t)sizes[i]*4>r->properties.limits.maxStorageBufferRange ||
           (sources[i].syncFd>=0 && !r->syncSupported))return BROOK_VK_REJECTED;
        if(fstat(sources[i].fd,&identity[i]) || !identity[i].st_ino)return BROOK_VK_REJECTED;
        for(size_t j=0;j<i;++j)if(identity[i].st_dev==identity[j].st_dev && identity[i].st_ino==identity[j].st_ino)return BROOK_VK_REJECTED;
    }
    if(r->external==3){
        struct stat *dest=&identity[BROOK_VK_MAX_LAYERS];
        if(!outputWords || outputWords>UINT32_MAX || outputWords>SIZE_MAX/4 ||
           (uint64_t)outputWords*4>r->properties.limits.maxStorageBufferRange || fstat(outputFd,dest) || !dest->st_ino)return BROOK_VK_REJECTED;
        for(size_t i=0;i<count;++i)if(identity[i].st_dev==dest->st_dev && identity[i].st_ino==dest->st_ino)return BROOK_VK_REJECTED;
    }
    if(!(r->external==3?valid_batch(scene,words,sizes,count,tiles,outputWords):
                          brook_vk_scene_valid_multi(scene,words,sizes,count)) ||
       (r->external==2 && (uint64_t)scene[0]*scene[1]>outputWords))return BROOK_VK_REJECTED;
    PFN_vkGetMemoryFdPropertiesKHR getProps=(PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(r->device,"vkGetMemoryFdPropertiesKHR");
    PFN_vkImportSemaphoreFdKHR importSync=(PFN_vkImportSemaphoreFdKHR)vkGetDeviceProcAddr(r->device,"vkImportSemaphoreFdKHR");
    if(!getProps)return BROOK_VK_REJECTED;
    VkPhysicalDeviceMemoryProperties memory;vkGetPhysicalDeviceMemoryProperties(r->physical,&memory);
    VkDescriptorBufferInfo descriptors[BROOK_VK_MAX_LAYERS];BrookVkResult result=BROOK_VK_REJECTED;
    const char *cacheOption=getenv("BROOK_VK_IMPORT_CACHE");
    const int cacheEnabled=cacheOption && !strcmp(cacheOption,"1");
    r->multiCount=(uint32_t)count;
    for(uint32_t item=0;item<r->multiCount+(r->external==3?1u:0u);++item){
        const int destination=item==r->multiCount;
        const uint32_t i=destination?BROOK_VK_MAX_LAYERS:item;
        const int sourceFd=destination?outputFd:sources[i].fd;
        const size_t sourceWords=destination?outputWords:sizes[i];
        off_t size=lseek(sourceFd,0,SEEK_END);
        if(size<0 || (uint64_t)size!=(uint64_t)sourceWords*4 || lseek(sourceFd,0,SEEK_SET)!=0)goto cleanup;
        if(!destination && sources[i].syncFd>=0){
            if(!importSync)goto cleanup;
            int syncFd=dup(sources[i].syncFd);if(syncFd<0)goto cleanup;
            VkSemaphoreCreateInfo si={.sType=VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            if(vkCreateSemaphore(r->device,&si,NULL,&r->multiSemaphore[i])!=VK_SUCCESS){close(syncFd);goto cleanup;}
            VkImportSemaphoreFdInfoKHR sf={.sType=VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,
                .semaphore=r->multiSemaphore[i],.flags=VK_SEMAPHORE_IMPORT_TEMPORARY_BIT,
                .handleType=VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,.fd=syncFd};
            if(importSync(r->device,&sf)!=VK_SUCCESS){close(syncFd);goto cleanup;}
        }
        int cached=cacheEnabled?cache_lookup(r,&identity[i],(uint64_t)size):-1;
        if(cached>=0){
            ++r->timing.cache_hits;
            r->multiCache[i]=cached;r->multiSource[i]=r->cache[cached].buffer;r->multiMemory[i]=r->cache[cached].memory;
            if(!destination)descriptors[i]=(VkDescriptorBufferInfo){r->multiSource[i],0,(VkDeviceSize)size};
            continue;
        }
        VkExternalMemoryBufferCreateInfo ext={.sType=VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
            .handleTypes=VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
        VkBufferCreateInfo bi={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.pNext=&ext,.size=(VkDeviceSize)size,
            .usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
        if(vkCreateBuffer(r->device,&bi,NULL,&r->multiSource[i])!=VK_SUCCESS)goto cleanup;
        VkMemoryRequirements need;vkGetBufferMemoryRequirements(r->device,r->multiSource[i],&need);
        if(need.size>(VkDeviceSize)size)goto cleanup;
        int memoryFd=dup(sourceFd);if(memoryFd<0)goto cleanup;
        VkMemoryFdPropertiesKHR fp={.sType=VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
        if(getProps(r->device,VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,memoryFd,&fp)!=VK_SUCCESS){close(memoryFd);goto cleanup;}
        uint32_t type=UINT32_MAX;
        for(uint32_t t=0;t<memory.memoryTypeCount;++t)if((need.memoryTypeBits&fp.memoryTypeBits)&(1u<<t)){type=t;break;}
        if(type==UINT32_MAX){close(memoryFd);goto cleanup;}
        VkImportMemoryFdInfoKHR imp={.sType=VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
            .handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,.fd=memoryFd};
        VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.pNext=&imp,.allocationSize=(VkDeviceSize)size,.memoryTypeIndex=type};
        if(vkAllocateMemory(r->device,&ai,NULL,&r->multiMemory[i])!=VK_SUCCESS){close(memoryFd);goto cleanup;}
        if(vkBindBufferMemory(r->device,r->multiSource[i],r->multiMemory[i],0)!=VK_SUCCESS)goto cleanup;
        if(cacheEnabled){
            int pin=fcntl(sourceFd,F_DUPFD_CLOEXEC,0);
            int slot=pin>=0?cache_slot(r,(uint64_t)size):-1;
            if(slot>=0){
                r->cache[slot]=(struct ImportCacheEntry){identity[i].st_dev,identity[i].st_ino,(uint64_t)size,
                    ++r->cacheClock,pin,1,r->multiSource[i],r->multiMemory[i]};
                r->cacheBytes+=(uint64_t)size;r->multiCache[i]=slot;
                ++r->timing.cache_inserts;
            }else if(pin>=0)close(pin);
        }
        if(!destination)descriptors[i]=(VkDescriptorBufferInfo){r->multiSource[i],0,(VkDeviceSize)size};
    }
    /* The layout does not use partially-bound descriptors. */
    for(uint32_t i=r->multiCount;i<BROOK_VK_MAX_LAYERS;++i)descriptors[i]=descriptors[0];
    VkWriteDescriptorSet write={.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=r->set,.dstBinding=1,
        .descriptorCount=BROOK_VK_MAX_LAYERS,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.pBufferInfo=descriptors};
    vkUpdateDescriptorSets(r->device,1,&write,0,NULL);
    if(r->external==3){
        VkDescriptorBufferInfo db={r->multiSource[BROOK_VK_MAX_LAYERS],0,(VkDeviceSize)outputWords*4};
        VkWriteDescriptorSet outWrite={.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=r->set,.dstBinding=2,
            .descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.pBufferInfo=&db};
        vkUpdateDescriptorSets(r->device,1,&outWrite,0,NULL);
    }
    imported=renderer_now_ns();
    result=compose(r,scene,words,0,sizes,output,outputWords,timeoutNs,tiles,asynchronous);
    composed=renderer_now_ns();
cleanup:
    if(!r->quarantined && !r->pending)multi_cleanup(r);
    if(r->external==3 && start && imported>=start && composed>=imported &&
       (getenv("BROOK_RENDER_TIMING") || getenv("BROOK_RENDER_TIMING_AGGREGATE"))) {
        uint64_t end=renderer_now_ns();
        if(result==BROOK_VK_COMPLETE && end>=composed && getenv("BROOK_RENDER_TIMING_AGGREGATE")){
            ++r->timing.samples;r->timing.import_ns+=imported-start;
            r->timing.compose_ns+=composed-imported;r->timing.cleanup_ns+=end-composed;
        }
        if(end>=composed && getenv("BROOK_RENDER_TIMING"))fprintf(stderr,"BROOK_VK_TIMING import_ms=%lu compose_ms=%lu cleanup_ms=%lu\n",
            (unsigned long)((imported-start)/1000000ull),(unsigned long)((composed-imported)/1000000ull),
            (unsigned long)((end-composed)/1000000ull));
    }
    return result;
}

BrookVkResult brook_vk_renderer_compose_multi(BrookVkRenderer *r,const uint32_t *scene,size_t words,
        const struct BrookVkExternalSource *sources,size_t count,uint32_t *output,size_t outputWords,uint64_t timeoutNs) {
    if(!r || r->external!=2)return BROOK_VK_REJECTED;
    return compose_multi(r,scene,words,sources,count,output,-1,outputWords,timeoutNs,1,0);
}
BrookVkResult brook_vk_renderer_compose_multi_output(BrookVkRenderer *r,const uint32_t *scene,size_t words,
        const struct BrookVkExternalSource *sources,size_t count,int outputFd,size_t outputWords,uint64_t timeoutNs) {
    if(!r || r->external!=3)return BROOK_VK_REJECTED;
    return compose_multi(r,scene,words,sources,count,NULL,outputFd,outputWords,timeoutNs,1,0);
}
BrookVkResult brook_vk_renderer_compose_batch_output(BrookVkRenderer *r,const uint32_t *scenes,size_t words,uint32_t tiles,
        const struct BrookVkExternalSource *sources,size_t count,int outputFd,size_t outputWords,uint64_t timeoutNs) {
    if(!r || r->external!=3)return BROOK_VK_REJECTED;
    return compose_multi(r,scenes,words,sources,count,NULL,outputFd,outputWords,timeoutNs,tiles,0);
}
BrookVkResult brook_vk_renderer_submit_batch_output(BrookVkRenderer *r,const uint32_t *scenes,size_t words,uint32_t tiles,
        const struct BrookVkExternalSource *sources,size_t count,int outputFd,size_t outputWords){
    if(!r || r->external!=3)return BROOK_VK_REJECTED;
    return compose_multi(r,scenes,words,sources,count,NULL,outputFd,outputWords,0,tiles,1);
}
BrookVkResult brook_vk_renderer_poll(BrookVkRenderer *r){
    if(!r || !r->pending || r->quarantined)return BROOK_VK_REJECTED;
    VkResult status=vkGetFenceStatus(r->device,r->fence);
    if(status==VK_NOT_READY)return BROOK_VK_PENDING;
    r->pending=0;
    if(status!=VK_SUCCESS){r->quarantined=1;r->failure=(struct BrookVkFailure){BROOK_VK_FAILURE_WAIT,(int)status,0,0};return BROOK_VK_UNCERTAIN;}
    if(r->timestampPending){
        struct {uint64_t value,available;} q[2]={{0}};
        VkResult result=vkGetQueryPoolResults(r->device,r->timestampPool,0,2,sizeof(q),q,sizeof(q[0]),
                                              VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        if(result==VK_SUCCESS && q[0].available && q[1].available){
            const uint64_t mask=r->timestampBits==64?UINT64_MAX:(UINT64_C(1)<<r->timestampBits)-1;
            const uint64_t ticks=(q[1].value-q[0].value)&mask;
            const double ns=(double)ticks*r->properties.limits.timestampPeriod;
            if(ns>=0 && ns<(double)UINT64_MAX){r->gpuTimestampNs=(uint64_t)ns;r->gpuTimestampValid=1;}
        }
        r->timestampPending=0;
    }
    multi_cleanup(r);return BROOK_VK_COMPLETE;
}
