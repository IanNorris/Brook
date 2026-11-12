#define _GNU_SOURCE
#define vkDestroyBuffer test_vkDestroyBuffer
#define vkFreeMemory test_vkFreeMemory
#define vkGetFenceStatus test_vkGetFenceStatus
#define vkGetQueryPoolResults test_vkGetQueryPoolResults
#include "renderer.c"
#include <assert.h>
static unsigned destroyed,freed;
static VkResult fakeFence=VK_NOT_READY;
static unsigned queryReads;static uint64_t queryAvailable=1;
VKAPI_ATTR void VKAPI_CALL test_vkDestroyBuffer(VkDevice d,VkBuffer b,const VkAllocationCallbacks *a){(void)d;(void)b;(void)a;++destroyed;}
VKAPI_ATTR void VKAPI_CALL test_vkFreeMemory(VkDevice d,VkDeviceMemory m,const VkAllocationCallbacks *a){(void)d;(void)m;(void)a;++freed;}
VKAPI_ATTR VkResult VKAPI_CALL test_vkGetFenceStatus(VkDevice d,VkFence f){(void)d;(void)f;return fakeFence;}
VKAPI_ATTR VkResult VKAPI_CALL test_vkGetQueryPoolResults(VkDevice d,VkQueryPool p,uint32_t first,uint32_t count,size_t size,void *data,VkDeviceSize stride,VkQueryResultFlags flags){
    (void)d;(void)p;assert(first==0 && count==2 && size==32 && stride==16 && (flags&VK_QUERY_RESULT_WITH_AVAILABILITY_BIT));
    uint64_t *v=data;v[0]=250;v[1]=queryAvailable;v[2]=5;v[3]=queryAvailable;++queryReads;return VK_SUCCESS;
}
int main(void){
    BrookVkRenderer r={0};for(unsigned i=0;i<IMPORT_CACHE_SLOTS;++i)r.cache[i].pin=-1;
    char path[]="/tmp/brook-cache-test-XXXXXX";int fd=mkstemp(path);assert(fd>=0);unlink(path);
    struct stat st;assert(!fstat(fd,&st));int pin=fcntl(fd,F_DUPFD_CLOEXEC,0);assert(pin>=0);
    int slot=cache_slot(&r,4096);assert(slot==0);
    r.cache[slot]=(struct ImportCacheEntry){st.st_dev,st.st_ino,4096,1,pin,0,(VkBuffer)1,(VkDeviceMemory)1};
    r.cacheBytes=4096;assert(cache_lookup(&r,&st,4096)==slot);
    assert(cache_lookup(&r,&st,4096)<0); /* selected by this submission */
    assert(cache_lookup(&r,&st,8192)<0); /* size participates in identity */
    for(unsigned i=1;i<IMPORT_CACHE_SLOTS;++i){
        int other=dup(fd);assert(other>=0);
        r.cache[i]=(struct ImportCacheEntry){st.st_dev,st.st_ino+i,4096,i+2,other,1,(VkBuffer)1,(VkDeviceMemory)1};
        r.cacheBytes+=4096;
    }
    assert(cache_slot(&r,4096)<0); /* every cached object is in use */
    r.cache[4].active=0;assert(cache_slot(&r,4096)==4 && destroyed==1 && freed==1);
    assert(cache_slot(&r,IMPORT_CACHE_MAX_BYTES+1)<0);
    for(unsigned i=0;i<IMPORT_CACHE_SLOTS;++i)cache_drop(&r,i);
    assert(!r.cacheBytes && destroyed==IMPORT_CACHE_SLOTS && freed==IMPORT_CACHE_SLOTS);
    /* Two independently pending frame contexts must not retire each other's
     * source or output while their own fence remains unsignaled. */
    BrookVkRenderer a={0},b={0};
    for(unsigned i=0;i<BROOK_VK_MAX_LAYERS+1;++i)a.multiCache[i]=b.multiCache[i]=-1;
    a.pending=b.pending=1;a.multiCount=b.multiCount=1;
    a.multiSource[0]=b.multiSource[0]=(VkBuffer)1;a.multiMemory[0]=b.multiMemory[0]=(VkDeviceMemory)1;
    unsigned before=destroyed;assert(brook_vk_renderer_poll(&a)==BROOK_VK_PENDING);
    assert(!brook_vk_renderer_destroy(&a) && destroyed==before && b.pending);
    fakeFence=VK_SUCCESS;assert(brook_vk_renderer_poll(&a)==BROOK_VK_COMPLETE);
    assert(destroyed==before+1 && b.pending && b.multiSource[0]);
    fakeFence=VK_ERROR_DEVICE_LOST;assert(brook_vk_renderer_poll(&b)==BROOK_VK_UNCERTAIN);
    assert(b.quarantined && b.multiSource[0] && !brook_vk_renderer_destroy(&b) && destroyed==before+1);
    BrookVkRenderer timed={0};for(unsigned i=0;i<BROOK_VK_MAX_LAYERS+1;++i)timed.multiCache[i]=-1;
    timed.pending=timed.timestampPending=1;timed.timestampPool=(VkQueryPool)1;timed.timestampBits=8;timed.properties.limits.timestampPeriod=80;
    fakeFence=VK_NOT_READY;assert(brook_vk_renderer_poll(&timed)==BROOK_VK_PENDING && !queryReads);
    fakeFence=VK_SUCCESS;assert(brook_vk_renderer_poll(&timed)==BROOK_VK_COMPLETE);
    uint64_t ns=0;assert(queryReads==1 && brook_vk_renderer_gpu_timestamp_ns(&timed,&ns) && ns==880);
    timed.pending=timed.timestampPending=1;timed.gpuTimestampValid=0;queryAvailable=0;
    assert(brook_vk_renderer_poll(&timed)==BROOK_VK_COMPLETE && !brook_vk_renderer_gpu_timestamp_ns(&timed,&ns));
    close(fd);puts("IMPORT_CACHE_LIFETIME_PASS");return 0;
}
