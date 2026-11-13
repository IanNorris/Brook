/* Submit two independent GPU output contexts before polling either. */
#define _GNU_SOURCE
#include "renderer.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#define IOC(n,s) ((3u<<30)|((unsigned)(s)<<16)|('d'<<8)|(n))
#define CHECK(x) do{if(!(x)){fprintf(stderr,"ASYNC_PAIR_FAIL line=%d errno=%d\n",__LINE__,errno);return 1;}}while(0)
struct create {uint64_t size;uint32_t handle,pad;};
struct map_offset {uint32_t handle,pad;uint64_t offset,flags,extensions;};
struct prime {uint32_t handle,flags;int32_t fd;};
struct domain {uint32_t handle,read,write;};
struct gem_close {uint32_t handle,pad;};
int main(void){
    int drm=open("/dev/dri/renderD129",O_RDWR|O_CLOEXEC);CHECK(drm>=0);
    struct create bo[4]={{65536,0,0},{65536,0,0},{65536,0,0},{65536,0,0}};
    struct prime fd[4];uint32_t *pixels[4];
    for(unsigned i=0;i<4;++i){CHECK(!ioctl(drm,IOC(0x5b,sizeof(bo[i])),&bo[i]));
        struct map_offset m={bo[i].handle,0,0,2,0};CHECK(!ioctl(drm,IOC(0x64,sizeof(m)),&m));
        pixels[i]=mmap(NULL,65536,PROT_READ|PROT_WRITE,MAP_SHARED,drm,(off_t)m.offset);CHECK(pixels[i]!=MAP_FAILED);
        fd[i]=(struct prime){bo[i].handle,O_CLOEXEC|O_RDWR,-1};CHECK(!ioctl(drm,IOC(0x2d,sizeof(fd[i])),&fd[i]));
        for(unsigned p=0;p<4096;++p)pixels[i][p]=i<2?(i?0xff2060d0u:0xffd02020u):0;
    }
    uint32_t scene[21]={64,64,1,0};const uint32_t layer[]={0,64,64,64,0,0,64,64,0,0,64,64,0,0,64,64,255};
    memcpy(scene+4,layer,sizeof(layer));
    BrookVkRenderer *r[2]={brook_vk_renderer_create_multi_output(21,0),brook_vk_renderer_create_multi_output(21,0)};CHECK(r[0] && r[1]);
    struct BrookVkExternalSource src[2]={{fd[0].fd,16384,-1},{fd[1].fd,16384,-1}};
    unsigned pendingSeen=0,bothPending=0;
    for(unsigned round=0;round<5;++round){
      for(unsigned i=0;i<2;++i){
        struct domain d={bo[i].handle,1,1};CHECK(!ioctl(drm,IOC(0x5f,sizeof(d)),&d));
        uint32_t color=(i?0xff2060d0u:0xffd02020u)^round;
        for(unsigned p=0;p<4096;++p)pixels[i][p]=color;
        BrookVkResult result=brook_vk_renderer_submit_batch_output(r[i],scene,21,1,&src[i],1,fd[i+2].fd,16384);
        if(result!=BROOK_VK_PENDING){fprintf(stderr,"ASYNC_PAIR_SUBMIT_FAILED result=%d resources retained\n",result);for(;;)pause();}
      }
      BrookVkResult states[2]={brook_vk_renderer_poll(r[0]),brook_vk_renderer_poll(r[1])};
      if(states[0]==BROOK_VK_PENDING && states[1]==BROOK_VK_PENDING)++bothPending;
      for(unsigned i=0;i<2;++i){BrookVkResult status=states[i];
        for(unsigned p=0;p<10000 && status==BROOK_VK_PENDING;++p){status=brook_vk_renderer_poll(r[i]);
            if(status==BROOK_VK_PENDING){++pendingSeen;usleep(1000);}}
        if(status!=BROOK_VK_COMPLETE){fprintf(stderr,"ASYNC_PAIR_UNCERTAIN resources retained\n");for(;;)pause();}
      }
      for(unsigned i=0;i<2;++i){
        struct domain d={bo[i+2].handle,1,0};CHECK(!ioctl(drm,IOC(0x5f,sizeof(d)),&d));
        uint32_t color=(i?0xff2060d0u:0xffd02020u)^round;
        for(unsigned p=0;p<4096;++p)CHECK(pixels[i+2][p]==color);
      }
    }
    uint64_t hits=0;for(unsigned i=0;i<2;++i){struct BrookVkTiming stats=brook_vk_renderer_take_timing(r[i]);hits+=stats.cache_hits;CHECK(brook_vk_renderer_destroy(r[i]));}
    if(getenv("BROOK_VK_IMPORT_CACHE"))CHECK(hits>=16);
    for(unsigned i=0;i<4;++i){close(fd[i].fd);munmap(pixels[i],65536);struct gem_close c={bo[i].handle,0};CHECK(!ioctl(drm,IOC(9,sizeof(c)),&c));}
    close(drm);printf("ASYNC_PAIR_PASS rounds=5 submitted_per_round=2 both_pending=%u pending_polls=%u cache_hits=%llu\n",bothPending,pendingSeen,(unsigned long long)hits);return 0;
}
