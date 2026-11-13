/* Matched offscreen full-frame comparison of full and compact chrome. */
#define _GNU_SOURCE
#include "renderer.h"
#include "scene.h"
#include "desktop_chrome_spans.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#define IOC(n,s) ((3u<<30)|((unsigned)(s)<<16)|('d'<<8)|(n))
#define CHECK(c) do {if(!(c)){fprintf(stderr,"CHROME_BENCH_FAIL line=%d errno=%d\n",__LINE__,errno);return 1;}} while(0)
struct create {uint64_t size;uint32_t handle,pad;};
struct map_offset {uint32_t handle,pad;uint64_t offset,flags,extensions;};
struct prime {uint32_t handle,flags;int32_t fd;};
struct domain {uint32_t handle,read,write;};
struct buffer {struct create gem;struct prime prime;uint32_t *pixels;};
static int buffer_open(int drm,struct buffer *b,uint64_t bytes){
    b->gem=(struct create){bytes,0,0};if(ioctl(drm,IOC(0x5b,sizeof(b->gem)),&b->gem))return 0;
    struct map_offset map={b->gem.handle,0,0,2,0};if(ioctl(drm,IOC(0x64,sizeof(map)),&map))return 0;
    b->pixels=mmap(NULL,b->gem.size,PROT_READ|PROT_WRITE,MAP_SHARED,drm,(off_t)map.offset);
    if(b->pixels==MAP_FAILED)return 0;
    b->prime=(struct prime){b->gem.handle,O_CLOEXEC|O_RDWR,-1};return !ioctl(drm,IOC(0x2d,sizeof(b->prime)),&b->prime);
}
static uint64_t ns(void){struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))return 0;return (uint64_t)t.tv_sec*1000000000ull+t.tv_nsec;}
int main(void){
    enum { W=1600,H=900,ROUNDS=6 };
    int drm=open("/dev/dri/renderD129",O_RDWR|O_CLOEXEC);CHECK(drm>=0);
    struct buffer bg={0},full={0},out={0},parts[4]={0};
    CHECK(buffer_open(drm,&bg,(uint64_t)W*H*4) && buffer_open(drm,&full,(uint64_t)W*H*4) && buffer_open(drm,&out,(uint64_t)W*H*4));
    for(unsigned y=0;y<H;++y)for(unsigned x=0;x<W;++x){
        bg.pixels[y*W+x]=0xff000000u|((x*13u+y*7u)&0xffffffu);
        full.pixels[y*W+x]=(x>=4 && x<W-4 && y>=32 && y<H-4)?0:0xff2368acu;
    }
    struct BrookDesktopLayer outer={.window=1,.incarnation=1,.width=W,.height=H};
    struct BrookDesktopLayer inner={.window=1,.incarnation=1,.clientX=4,.clientY=32,.clientWidth=W-8,.clientHeight=H-36};
    struct DesktopChromeSpan spans[4];CHECK(desktop_chrome_spans(&inner,&outer,spans)==4);
    struct BrookVkExternalSource fullSources[2]={{bg.prime.fd,bg.gem.size/4,-1},{full.prime.fd,full.gem.size/4,-1}};
    struct BrookVkExternalSource compactSources[5]={{bg.prime.fd,bg.gem.size/4,-1}};
    uint32_t fullScene[BROOK_VK_TILE_STRIDE]={W,H,2,0},compactScene[BROOK_VK_TILE_STRIDE]={W,H,5,0};
    struct BrookVkSceneLayer base={0,W,W,H,0,0,W,H,0,0,W,H,0,0,W,H,255};
    struct BrookVkSceneLayer chrome=base;chrome.opacity|=BROOK_VK_STRAIGHT_ALPHA;
    memcpy(fullScene+4,&base,sizeof(base));memcpy(fullScene+21,&chrome,sizeof(chrome));memcpy(compactScene+4,&base,sizeof(base));
    uint64_t compactBytes=0;
    for(unsigned i=0;i<4;++i){const struct DesktopChromeSpan *s=&spans[i];
        CHECK(buffer_open(drm,&parts[i],(uint64_t)s->width*s->height*4));
        CHECK(desktop_chrome_copy_span(full.pixels,W,W,H,s,parts[i].pixels,parts[i].gem.size/4));
        compactSources[i+1]=(struct BrookVkExternalSource){parts[i].prime.fd,parts[i].gem.size/4,-1};compactBytes+=parts[i].gem.size;
        struct BrookVkSceneLayer layer={0,s->width,s->width,s->height,0,0,s->width,s->height,(int32_t)s->x,(int32_t)s->y,
            s->width,s->height,0,0,W,H,255|BROOK_VK_STRAIGHT_ALPHA};memcpy(compactScene+4+(i+1)*17,&layer,sizeof(layer));
    }
    BrookVkRenderer *r=brook_vk_renderer_create_batch_output(1,0);CHECK(r);
    fprintf(stderr,"CHROME_BENCH_DEVICE %s full_bytes=%lu compact_bytes=%lu\n",brook_vk_renderer_device(r),
            (unsigned long)full.gem.size,(unsigned long)compactBytes);
    uint64_t timing[2][ROUNDS];
    for(unsigned round=0;round<ROUNDS;++round)for(unsigned order=0;order<2;++order){
        unsigned compact=(round+order)&1u;uint64_t start=ns();
        /* Real desktop frames repack the strips after each chrome capture.
         * Include those CPU copies in the compact interval. */
        if(compact)for(unsigned i=0;i<4;++i)
            CHECK(desktop_chrome_copy_span(full.pixels,W,W,H,&spans[i],parts[i].pixels,parts[i].gem.size/4));
        BrookVkResult status=brook_vk_renderer_compose_batch_output(r,compact?compactScene:fullScene,BROOK_VK_TILE_STRIDE,1,
            compact?compactSources:fullSources,compact?5:2,out.prime.fd,out.gem.size/4,10000000000ull);
        if(status==BROOK_VK_UNCERTAIN){fprintf(stderr,"CHROME_BENCH_UNCERTAIN resources retained\n");for(;;)pause();}
        CHECK(status==BROOK_VK_COMPLETE);uint64_t end=ns();CHECK(start && end>=start);timing[compact][round]=end-start;
        if(!round){struct domain d={out.gem.handle,1,0};CHECK(!ioctl(drm,IOC(0x5f,sizeof(d)),&d));
            for(unsigned i=0;i<W*H;++i)CHECK(out.pixels[i]==(full.pixels[i]?full.pixels[i]:bg.pixels[i]));}
    }
    for(unsigned i=0;i<ROUNDS;++i)fprintf(stderr,"CHROME_BENCH round=%u full_us=%lu compact_us=%lu\n",i,
        (unsigned long)(timing[0][i]/1000),(unsigned long)(timing[1][i]/1000));
    CHECK(brook_vk_renderer_destroy(r));puts("CHROME_BENCH_PASS");return 0;
}
