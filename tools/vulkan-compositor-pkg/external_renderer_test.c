/* Dell hardware fixture: compose directly from an exported Intel GEM buffer. */
#define _GNU_SOURCE
#include "renderer.h"
#include "compositor_probe.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#define IOC(n,s) ((3u<<30)|((unsigned)(s)<<16)|('d'<<8)|(n))
#define CHECK(c) do {if(!(c)){fprintf(stderr,"EXTERNAL_COMPOSE_FAIL line=%d errno=%d\n",__LINE__,errno);return 1;}} while(0)
struct create {uint64_t size;uint32_t handle,pad;};
struct map_offset {uint32_t handle,pad;uint64_t offset,flags,extensions;};
struct prime {uint32_t handle,flags;int32_t fd;};
struct gem_close {uint32_t handle,pad;};
struct context {uint32_t id,pad;};
struct object {uint32_t handle,reloc_count;uint64_t relocs,alignment,offset,flags,reserved1,reserved2;};
struct exec {uint64_t objects;uint32_t count,start,length,dr1,dr4,clip_count;uint64_t clips,flags,context,fences;};
struct sync_object {uint32_t handle,flags;};
struct exec_fence {uint32_t handle,flags;};
struct sync_handle {uint32_t handle,flags;int32_t fd;uint32_t pad;};
int main(void){
    int drm=open("/dev/dri/renderD129",O_RDWR|O_CLOEXEC);CHECK(drm>=0);
    struct create gem={65536,0,0};CHECK(!ioctl(drm,IOC(0x5b,sizeof(gem)),&gem));
    struct map_offset map={gem.handle,0,0,2,0};CHECK(!ioctl(drm,IOC(0x64,sizeof(map)),&map));
    uint32_t *source=mmap(NULL,65536,PROT_READ|PROT_WRITE,MAP_SHARED,drm,(off_t)map.offset);CHECK(source!=MAP_FAILED);
    struct prime out={gem.handle,O_CLOEXEC|O_RDWR,-1};CHECK(!ioctl(drm,IOC(0x2d,sizeof(out)),&out) && out.fd>=0);
    struct context ctx={0};CHECK(!ioctl(drm,IOC(0x6d,sizeof(ctx)),&ctx) && ctx.id);
    struct create batch={4096,0,0};CHECK(!ioctl(drm,IOC(0x5b,sizeof(batch)),&batch));
    struct map_offset bm={batch.handle,0,0,2,0};CHECK(!ioctl(drm,IOC(0x64,sizeof(bm)),&bm));
    uint32_t *cmd=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED,drm,(off_t)bm.offset);CHECK(cmd!=MAP_FAILED);
    BrookVkRenderer *r=brook_vk_renderer_create_external(16384,0);CHECK(r);
    fprintf(stderr,"EXTERNAL_COMPOSITOR_DEVICE %s\n",brook_vk_renderer_device(r));
    uint32_t scene[16384],pixels[4096];
    for(uint32_t round=0;round<3;++round){
        compositor_probe_prepare(scene,16384,round);memcpy(source,scene,65536);
        memset(pixels,0,sizeof(pixels));
        struct sync_object sync={0,0};CHECK(!ioctl(drm,IOC(0xbf,sizeof(sync)),&sync) && sync.handle);
        cmd[0]=0x10000002;cmd[1]=0x40011100;cmd[2]=0;cmd[3]=0xff123456;cmd[4]=0x05000000;cmd[5]=0;
        struct object objects[]={{gem.handle,0,0,4096,0x40011000,20,0,0},{batch.handle,0,0,4096,0x10000,16,0,0}};
        struct exec_fence ef={sync.handle,2};
        struct exec request={(uintptr_t)objects,2,0,24,0,0,1,(uintptr_t)&ef,(1u<<11)|(1u<<19),ctx.id,0};
        if(ioctl(drm,IOC(0x69,sizeof(request)),&request)){fprintf(stderr,"PRODUCER_UNCERTAIN resources retained\n");for(;;)pause();}
        struct sync_handle handle={sync.handle,1,-1,0};
        if(ioctl(drm,IOC(0xc1,sizeof(handle)),&handle) || handle.fd<0){fprintf(stderr,"FENCE_UNCERTAIN resources retained\n");for(;;)pause();}
        struct pollfd pollFence={.fd=handle.fd,.events=POLLIN};int ready=poll(&pollFence,1,0);
        fprintf(stderr,"EXTERNAL_PRODUCER_FENCE ready=%d revents=%d\n",ready,pollFence.revents);
        scene[64]=0xff123456;
        BrookVkResult status=brook_vk_renderer_compose_external_sync(r,scene,16384,out.fd,16384,handle.fd,pixels,4096,10000000000ULL);
        if(status==BROOK_VK_UNCERTAIN){fprintf(stderr,"EXTERNAL_COMPOSE_UNCERTAIN resources retained\n");for(;;)pause();}
        if(status!=BROOK_VK_COMPLETE){fprintf(stderr,"PRODUCER_MAY_BE_ACTIVE resources retained\n");for(;;)pause();}
        close(handle.fd);CHECK(!ioctl(drm,IOC(0xc0,sizeof(sync)),&sync));
        for(uint32_t i=0;i<4096;++i){uint32_t want=compositor_probe_pixel(scene,i%64,i/64);
            if(pixels[i]!=want){fprintf(stderr,"mismatch round=%u pixel=%u got=%08x expected=%08x\n",round,i,pixels[i],want);return 1;}}
    }
    pixels[0]=0x12345678;scene[21]=16380;
    CHECK(brook_vk_renderer_compose_external(r,scene,16384,out.fd,16384,pixels,4096,10000000000ULL)==BROOK_VK_REJECTED && pixels[0]==0x12345678);
    CHECK(brook_vk_renderer_destroy(r));close(out.fd);munmap(source,65536);munmap(cmd,4096);
    struct gem_close batchClose={batch.handle,0};CHECK(!ioctl(drm,IOC(9,sizeof(batchClose)),&batchClose));CHECK(!ioctl(drm,IOC(0x6e,sizeof(ctx)),&ctx));
    struct gem_close closeArg={gem.handle,0};CHECK(!ioctl(drm,IOC(9,sizeof(closeArg)),&closeArg));close(drm);
    puts("VULKAN_EXTERNAL_COMPOSITOR_PASS");return 0;
}
