#define _GNU_SOURCE
#include "desktop_layers_abi.h"
#include "overlay_copy.h"
#include "gpu_output_abi.h"
#include "wm_window_state.h"
#include "brook_debug_frame.h"
#include "renderer.h"
#include "scene.h"
#include "desktop_tile.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#define IOC(n,s) ((3u<<30)|((unsigned)(s)<<16)|('d'<<8)|(n))
#define CHECK(x) do{if(!(x)){fprintf(stderr,"DESKTOP_VK_FAIL line=%d errno=%d\n",__LINE__,errno);return 1;}}while(0)
struct window_out {uint32_t id,stride;uint64_t pixels;};
struct create {uint64_t size;uint32_t handle,pad;};
struct map_offset {uint32_t handle,pad;uint64_t offset,flags,extensions;};
struct prime {uint32_t handle,flags;int32_t fd;};
struct gem_close {uint32_t handle,pad;};
struct domain {uint32_t handle,read,write;};
static struct BrookDesktopOverlayBands capturedBands;
static int forceTopOpaque;
static uint32_t pattern(unsigned layer,unsigned x,unsigned y){
    uint32_t alpha=layer && !forceTopOpaque?((x+y)&255u):255u;
    return (alpha<<24) | (layer?0x00800000u:0x00000000u) | (y<<8) | x;
}
static uint32_t straight_over(uint32_t src,uint32_t dst){
    uint32_t a=src>>24,inv=255-a,out=0;
    for(unsigned c=0;c<3;++c){uint32_t s=(src>>(8*c))&255,d=(dst>>(8*c))&255;
        out|=((s*a+d*inv+127)/255)<<(8*c);}
    uint32_t alpha=a+(((dst>>24)*inv+127)/255);
    return out | (alpha<<24);
}
static void uncertain(const char *message){fprintf(stderr,"%s resources retained\n",message);for(;;)pause();}
static int capture_overlay(uint32_t *output,uint32_t width,uint32_t height){
    long token=syscall(BROOK_SYS_DESKTOP_OVERLAY,0,0,0,0,0,0);CHECK(token>0);
    struct BrookDebugFrameInfo info={0};
    for(unsigned retry=0;retry<500;++retry){
        CHECK(!syscall(BROOK_SYS_DESKTOP_OVERLAY,1,token,&info,sizeof(info),0,0));
        if(info.state!=1)break;
        usleep(10000);
    }
    CHECK(info.state==2 && info.width==width && info.height==height && info.stride==width && info.bytes==(uint64_t)width*height*4);
    if(getenv("BROOK_COMPOSE_OVERLAY_BANDS"))
        CHECK(!syscall(BROOK_SYS_DESKTOP_OVERLAY,4,token,&capturedBands,sizeof(capturedBands),0,0));
    for(uint64_t offset=0;offset<info.bytes;){
        uint64_t n=info.bytes-offset<BROOK_DEBUG_FRAME_CHUNK?info.bytes-offset:BROOK_DEBUG_FRAME_CHUNK;
        CHECK(syscall(BROOK_SYS_DESKTOP_OVERLAY,2,token,(uint8_t*)output+offset,n,offset,0)==(long)n);offset+=n;
    }
    if(getenv("BROOK_VERIFY_OVERLAY_BANDS")){
        struct BrookDesktopOverlayBands bands={0};
        CHECK(!syscall(BROOK_SYS_DESKTOP_OVERLAY,4,token,&bands,sizeof(bands),0,0));
        CHECK(!bands.reserved && bands.count<=BROOK_DESKTOP_OVERLAY_MAX_BANDS);
        for(uint32_t i=0;i<bands.count;++i){const struct BrookDesktopOverlayBand *b=&bands.bands[i];
            CHECK(b->width && b->height && b->x<width && b->y<height && b->width<=width-b->x && b->height<=height-b->y);
            CHECK(!i || b->y>=bands.bands[i-1].y+bands.bands[i-1].height);
            uint32_t *packed=malloc((size_t)b->width*b->height*4);CHECK(packed);
            if(getenv("BROOK_REQUIRE_PACKED_OVERLAY")){
                const uint64_t bytes=(uint64_t)b->width*b->height*4;
                CHECK(syscall(BROOK_SYS_DESKTOP_OVERLAY,BROOK_DESKTOP_OVERLAY_READ_BAND,token,packed,bytes,i,0)==(long)bytes);
                errno=0;CHECK(syscall(BROOK_SYS_DESKTOP_OVERLAY,BROOK_DESKTOP_OVERLAY_READ_BAND,token,packed,bytes-1,i,0)==-1 && errno==EINVAL);
                errno=0;CHECK(syscall(BROOK_SYS_DESKTOP_OVERLAY,BROOK_DESKTOP_OVERLAY_READ_BAND,token,packed,bytes,bands.count,0)==-1 && errno==EINVAL);
            }
            CHECK(!desktop_overlay_copy_band(token,i,packed,width,height,b));
            for(uint32_t y=0;y<b->height;++y){uint64_t offset=((uint64_t)(b->y+y)*width+b->x)*4;
                CHECK(!memcmp(packed+(uint64_t)y*b->width,(uint8_t*)output+offset,(size_t)b->width*4));
            }
            free(packed);
        }
        for(uint32_t y=0;y<height;++y)for(uint32_t x=0;x<width;++x)if(output[(uint64_t)y*width+x]>>24){
            unsigned hits=0;for(uint32_t i=0;i<bands.count;++i){const struct BrookDesktopOverlayBand *b=&bands.bands[i];
                if(x>=b->x && x<b->x+b->width && y>=b->y && y<b->y+b->height)++hits;}
            CHECK(hits==1);
        }
        fprintf(stderr,"DESKTOP_OVERLAY_BANDS_PASS count=%u packed_required=%d\n",bands.count,getenv("BROOK_REQUIRE_PACKED_OVERLAY")!=NULL);
    }
    CHECK(!syscall(BROOK_SYS_DESKTOP_OVERLAY,3,token,0,0,0,0));
    return 0;
}
int main(int argc,char **argv){
    int legacy=0,presentProbe=0,tracked=0,staleCheck=0,titleCheck=0,backdropEnabled=0,chromeEnabled=0,overlayEnabled=0,leaseCheck=0,leaseHold=0,repeatLease=0,liveLease=0,liveCache=0,regionCheck=0,overlapCheck=0,dependentCheck=0,coverPoint=0;
    unsigned long coverX=0,coverY=0;
    for(int i=1;i<argc;++i){
        if(!strcmp(argv[i],"--with-legacy"))legacy=1;
        else if(!strcmp(argv[i],"--present-probe"))presentProbe=1;
        else if(!strcmp(argv[i],"--tracked"))tracked=1;
        else if(!strcmp(argv[i],"--backdrop"))backdropEnabled=1;
        else if(!strcmp(argv[i],"--chrome"))chromeEnabled=1;
        else if(!strcmp(argv[i],"--overlay"))overlayEnabled=1;
        else if(!strcmp(argv[i],"--stale-check")){tracked=1;staleCheck=1;presentProbe=1;}
        else if(!strcmp(argv[i],"--title-stale-check")){tracked=1;staleCheck=1;titleCheck=1;presentProbe=1;}
        else if(!strcmp(argv[i],"--lease-check")){tracked=1;backdropEnabled=1;presentProbe=1;leaseCheck=1;}
        else if(!strcmp(argv[i],"--lease-hold")){tracked=1;backdropEnabled=1;presentProbe=1;leaseCheck=1;leaseHold=1;}
        else if(!strcmp(argv[i],"--repeat-lease")){tracked=1;backdropEnabled=1;chromeEnabled=1;overlayEnabled=1;presentProbe=1;leaseCheck=1;repeatLease=1;}
        else if(!strcmp(argv[i],"--live-lease")){tracked=1;backdropEnabled=1;chromeEnabled=1;overlayEnabled=1;presentProbe=1;leaseCheck=1;liveLease=1;}
        else if(!strcmp(argv[i],"--live-cache")){tracked=1;backdropEnabled=1;chromeEnabled=1;overlayEnabled=1;presentProbe=1;leaseCheck=1;liveLease=1;liveCache=1;}
        else if(!strcmp(argv[i],"--region-check")){tracked=1;backdropEnabled=1;chromeEnabled=1;overlayEnabled=1;presentProbe=1;leaseCheck=1;regionCheck=1;}
        else if(!strcmp(argv[i],"--overlap-check")){tracked=1;backdropEnabled=1;chromeEnabled=1;overlayEnabled=1;presentProbe=1;leaseCheck=1;regionCheck=1;overlapCheck=1;}
        else if(!strcmp(argv[i],"--dependent-check")){tracked=1;backdropEnabled=1;chromeEnabled=1;overlayEnabled=1;presentProbe=1;leaseCheck=1;regionCheck=1;dependentCheck=1;}
        else if(!strcmp(argv[i],"--cover-point") && i+2<argc){
            char *end=NULL;coverX=strtoul(argv[++i],&end,10);CHECK(end && !*end);
            coverY=strtoul(argv[++i],&end,10);CHECK(end && !*end);coverPoint=1;
        }
        else CHECK(0);
    }
    CHECK(!(backdropEnabled && presentProbe && !tracked));
    forceTopOpaque=coverPoint;
    CHECK(!(chromeEnabled && presentProbe && !leaseCheck));
    CHECK(!overlayEnabled || leaseCheck);
    enum { WINW=320,WINH=200,HEADER=1024 };
    uint32_t WIDTH=640,HEIGHT=400;
    if(leaseCheck){
        struct BrookDesktopLayers screen={.version=BROOK_DESKTOP_LAYERS_VERSION};
        CHECK(!syscall(BROOK_SYS_DESKTOP_LAYERS,BROOK_DESKTOP_BACKDROP_QUERY,&screen,sizeof(screen),0,0,0));
        WIDTH=screen.layers[0].width;HEIGHT=screen.layers[0].height;
    }
    const size_t WORDS=HEADER+(size_t)WIDTH*HEIGHT;
    struct window_out windows[2]={{0}};uint64_t incarnations[2]={0};
    for(unsigned n=0;n<2;++n){
        CHECK(syscall(506,(long)WINW,(long)WINH,(long)"Vulkan scene fixture",(long)&windows[n],chromeEnabled?4L:5L,0L)==0);
        CHECK(windows[n].id && windows[n].pixels && windows[n].stride>=WINW);
        BrookWindowState state={0};
        CHECK(!syscall(BROOK_SYS_WM_WINDOW_STATE,(long)windows[n].id,0L,(long)&state,(long)sizeof(state),0L,0L));
        incarnations[n]=state.incarnation;
        CHECK(!syscall(BROOK_SYS_WM_PLACE_WINDOW,(long)windows[n].id,(long)state.incarnation,n?80L:20L,n?80L:20L,0L,0L));
        uint32_t *pixels=(void *)(uintptr_t)windows[n].pixels;
        for(unsigned y=0;y<WINH;++y)for(unsigned x=0;x<WINW;++x)pixels[y*windows[n].stride+x]=pattern(n,x,y);
        CHECK(!syscall(508,(long)windows[n].id,0L,0L,0L,0L,0L));
    }
    struct BrookDesktopLayers desktop={.version=BROOK_DESKTOP_LAYERS_VERSION};
    CHECK(!syscall(BROOK_SYS_DESKTOP_LAYERS,BROOK_DESKTOP_LAYERS_QUERY,&desktop,sizeof(desktop),0,0,0));
    CHECK(desktop.count>=2 && desktop.count<=BROOK_DESKTOP_LAYER_LIMIT && (legacy || desktop.count==2));
    int drm=open("/dev/dri/renderD129",O_RDWR|O_CLOEXEC);CHECK(drm>=0);
    struct create gem[BROOK_DESKTOP_LAYER_LIMIT]={{0}};struct prime prime[BROOK_DESKTOP_LAYER_LIMIT]={{0}};void *maps[BROOK_DESKTOP_LAYER_LIMIT]={0};
    struct BrookVkExternalSource sources[BROOK_VK_MAX_LAYERS],clientSources[BROOK_DESKTOP_LAYER_LIMIT];
    struct BrookDesktopLayers chrome[BROOK_DESKTOP_LAYER_LIMIT]={{0}};
    struct create chromeGem[BROOK_DESKTOP_LAYER_LIMIT]={{0}};
    struct prime chromePrime[BROOK_DESKTOP_LAYER_LIMIT]={{0}};uint32_t *chromeMap[BROOK_DESKTOP_LAYER_LIMIT]={0};
    uint32_t clientIndex[BROOK_DESKTOP_LAYER_LIMIT]={0},chromeIndex[BROOK_DESKTOP_LAYER_LIMIT]={0},layerIds[BROOK_DESKTOP_LAYER_LIMIT]={0};
    struct BrookDesktopLayers backdrop={.version=BROOK_DESKTOP_LAYERS_VERSION};
    struct create backdropGem={0};struct prime backdropPrime={0};uint32_t *backdropMap=NULL;
    struct create overlayGem={0};struct prime overlayPrime={0};uint32_t *overlayMap=NULL;
    struct create bandGem[BROOK_DESKTOP_OVERLAY_MAX_BANDS]={{0}};
    struct prime bandPrime[BROOK_DESKTOP_OVERLAY_MAX_BANDS]={{0}};
    uint32_t *bandMap[BROOK_DESKTOP_OVERLAY_MAX_BANDS]={0};
    if(backdropEnabled){
        CHECK(!syscall(BROOK_SYS_DESKTOP_LAYERS,BROOK_DESKTOP_BACKDROP_QUERY,&backdrop,sizeof(backdrop),0,0,0));
        CHECK(backdrop.count==1 && backdrop.layers[0].width>=WIDTH && backdrop.layers[0].height>=HEIGHT);
        backdropGem.size=backdrop.layers[0].bytes;CHECK(!ioctl(drm,IOC(0x5b,sizeof(backdropGem)),&backdropGem));
        struct map_offset map={backdropGem.handle,0,0,2,0};CHECK(!ioctl(drm,IOC(0x64,sizeof(map)),&map));
        backdropMap=mmap(NULL,backdropGem.size,PROT_READ|PROT_WRITE,MAP_SHARED,drm,(off_t)map.offset);CHECK(backdropMap!=MAP_FAILED);
        backdropPrime=(struct prime){backdropGem.handle,O_CLOEXEC|O_RDWR,-1};CHECK(!ioctl(drm,IOC(0x2d,sizeof(backdropPrime)),&backdropPrime));
        sources[0]=(struct BrookVkExternalSource){backdropPrime.fd,backdropGem.size/4,-1};
        backdrop.layers[0].pixels=(uintptr_t)backdropMap;backdrop.layers[0].capacity=backdropGem.size;
    }
    for(unsigned i=0;i<desktop.count;++i){
        CHECK(desktop.layers[i].bytes && desktop.layers[i].bytes<=64ull*1024*1024);
        gem[i].size=desktop.layers[i].bytes;CHECK(!ioctl(drm,IOC(0x5b,sizeof(gem[i])),&gem[i]));
        struct map_offset map={gem[i].handle,0,0,2,0};CHECK(!ioctl(drm,IOC(0x64,sizeof(map)),&map));
        maps[i]=mmap(NULL,gem[i].size,PROT_READ|PROT_WRITE,MAP_SHARED,drm,(off_t)map.offset);CHECK(maps[i]!=MAP_FAILED);
        prime[i]=(struct prime){gem[i].handle,O_CLOEXEC|O_RDWR,-1};CHECK(!ioctl(drm,IOC(0x2d,sizeof(prime[i])),&prime[i]));
        clientSources[i]=(struct BrookVkExternalSource){prime[i].fd,gem[i].size/4,-1};
        desktop.layers[i].pixels=(uintptr_t)maps[i];desktop.layers[i].capacity=gem[i].size;
    }
    CHECK(!syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_ATTACH,0,0,0,0,0));
    CHECK(!syscall(BROOK_SYS_DESKTOP_LAYERS,tracked?BROOK_DESKTOP_LAYERS_CAPTURE_TRACKED:BROOK_DESKTOP_LAYERS_CAPTURE,&desktop,sizeof(desktop),0,0,0));
    CHECK(!tracked || desktop.reserved);
    if(backdropEnabled)CHECK(!syscall(BROOK_SYS_DESKTOP_LAYERS,tracked?BROOK_DESKTOP_BACKDROP_TRACKED_CAPTURE:BROOK_DESKTOP_BACKDROP_CAPTURE,&backdrop,sizeof(backdrop),0,0,0));
    uint32_t *scene=calloc(WORDS,sizeof(*scene));CHECK(scene);
    scene[0]=WIDTH;scene[1]=HEIGHT;scene[3]=0;unsigned sceneCount=backdropEnabled;
    if(backdropEnabled){
        struct BrookVkSceneLayer background={0,backdrop.layers[0].stride,backdrop.layers[0].width,backdrop.layers[0].height,
            0,0,WIDTH,HEIGHT,0,0,WIDTH,HEIGHT,0,0,WIDTH,HEIGHT,255};
        memcpy(scene+4,&background,sizeof(background));
    }
    unsigned found=0,legacyCount=0,nativeIndex=0;
    for(unsigned i=0;i<desktop.count;++i){
        const struct BrookDesktopLayer *l=&desktop.layers[i];
        layerIds[i]=l->window;chromeIndex[i]=UINT32_MAX;
        CHECK(l->upscale==1 && !(l->flags&~BROOK_DESKTOP_LAYER_LEGACY_OPAQUE));
        uint32_t *pixels=maps[i];
        if(l->window==windows[0].id || l->window==windows[1].id){
            unsigned n=l->window==windows[1].id;found|=1u<<n;
            if(!n)nativeIndex=i;
            CHECK(!l->flags && l->width==WINW && l->height==WINH && l->stride>=WINW);
            CHECK(l->clientX==(n?80:20) && l->clientY==(n?80:20));
            for(unsigned y=0;y<WINH;++y)for(unsigned x=0;x<WINW;++x)CHECK(pixels[y*l->stride+x]==pattern(n,x,y));
        }else{
            CHECK(legacy && l->flags==BROOK_DESKTOP_LAYER_LEGACY_OPAQUE);++legacyCount;
            for(uint64_t p=0;p<(uint64_t)l->stride*l->height;++p)pixels[p]|=0xff000000u;
        }
        uint32_t w=l->width<l->clientWidth?l->width:l->clientWidth;
        uint32_t h=l->height<l->clientHeight?l->height:l->clientHeight;CHECK(w && h);
        struct BrookVkSceneLayer layer={0,l->stride,l->width,l->height,0,0,w,h,
            l->clientX,l->clientY,w,h,0,0,WIDTH,HEIGHT,255|BROOK_VK_STRAIGHT_ALPHA};
        CHECK(sceneCount<BROOK_VK_MAX_LAYERS);clientIndex[i]=sceneCount;sources[sceneCount]=clientSources[i];
        memcpy(scene+4+sceneCount++*17,&layer,sizeof(layer));
        if(chromeEnabled){
            chrome[i].version=BROOK_DESKTOP_LAYERS_VERSION;chrome[i].layers[0].window=l->window;
            CHECK(!syscall(BROOK_SYS_DESKTOP_LAYERS,BROOK_DESKTOP_CHROME_QUERY,&chrome[i],sizeof(chrome[i]),0,0,0));
            struct BrookDesktopLayer *c=&chrome[i].layers[0];
            if(c->bytes){
                CHECK(sceneCount<BROOK_VK_MAX_LAYERS);chromeGem[i].size=c->bytes;
                CHECK(!ioctl(drm,IOC(0x5b,sizeof(chromeGem[i])),&chromeGem[i]));
                struct map_offset cm={chromeGem[i].handle,0,0,2,0};CHECK(!ioctl(drm,IOC(0x64,sizeof(cm)),&cm));
                chromeMap[i]=mmap(NULL,c->bytes,PROT_READ|PROT_WRITE,MAP_SHARED,drm,(off_t)cm.offset);CHECK(chromeMap[i]!=MAP_FAILED);
                chromePrime[i]=(struct prime){chromeGem[i].handle,O_CLOEXEC|O_RDWR,-1};CHECK(!ioctl(drm,IOC(0x2d,sizeof(chromePrime[i])),&chromePrime[i]));
                c->pixels=(uintptr_t)chromeMap[i];c->capacity=c->bytes;
                CHECK(!syscall(BROOK_SYS_DESKTOP_LAYERS,leaseCheck?BROOK_DESKTOP_CHROME_TRACKED_CAPTURE:BROOK_DESKTOP_CHROME_CAPTURE,&chrome[i],sizeof(chrome[i]),0,0,0));
                struct BrookVkSceneLayer decor={0,c->stride,c->width,c->height,0,0,c->width,c->height,
                    c->clientX,c->clientY,c->width,c->height,0,0,WIDTH,HEIGHT,255|BROOK_VK_STRAIGHT_ALPHA};
                chromeIndex[i]=sceneCount;sources[sceneCount]=(struct BrookVkExternalSource){chromePrime[i].fd,chromeGem[i].size/4,-1};
                memcpy(scene+4+sceneCount++*17,&decor,sizeof(decor));
            } else if(leaseCheck)CHECK(!syscall(BROOK_SYS_DESKTOP_LAYERS,BROOK_DESKTOP_CHROME_TRACKED_CAPTURE,&chrome[i],sizeof(chrome[i]),0,0,0));
        }
    }
    if(overlayEnabled){
        overlayGem.size=(uint64_t)WIDTH*HEIGHT*4;CHECK(!ioctl(drm,IOC(0x5b,sizeof(overlayGem)),&overlayGem));
        struct map_offset om={overlayGem.handle,0,0,2,0};CHECK(!ioctl(drm,IOC(0x64,sizeof(om)),&om));
        overlayMap=mmap(NULL,overlayGem.size,PROT_READ|PROT_WRITE,MAP_SHARED,drm,(off_t)om.offset);CHECK(overlayMap!=MAP_FAILED);
        CHECK(!capture_overlay(overlayMap,WIDTH,HEIGHT));
        for(uint32_t x=0;x<WIDTH;++x)CHECK((overlayMap[(uint64_t)(HEIGHT-1)*WIDTH+x]>>24)==255);
        uint64_t aboveOpaque=0,abovePartial=0;
        for(uint64_t i=0;i<(uint64_t)(HEIGHT>100?HEIGHT-100:0)*WIDTH;++i){
            const uint32_t a=overlayMap[i]>>24;if(a==255)++aboveOpaque;else if(a)++abovePartial;
        }
        fprintf(stderr,"DESKTOP_OVERLAY_ABOVE_TASKBAR opaque=%lu partial=%lu\n",(unsigned long)aboveOpaque,(unsigned long)abovePartial);
        if(getenv("BROOK_COMPOSE_OVERLAY_BANDS")){
            CHECK(!liveLease && !repeatLease && capturedBands.count<=BROOK_DESKTOP_OVERLAY_MAX_BANDS && !capturedBands.reserved);
            for(uint32_t i=0;i<capturedBands.count;++i){const struct BrookDesktopOverlayBand *b=&capturedBands.bands[i];
                CHECK(b->width && b->height && b->x<WIDTH && b->y<HEIGHT && b->width<=WIDTH-b->x && b->height<=HEIGHT-b->y);
                CHECK(!i || b->y>=capturedBands.bands[i-1].y+capturedBands.bands[i-1].height);
                CHECK(sceneCount<BROOK_VK_MAX_LAYERS);bandGem[i].size=(uint64_t)b->width*b->height*4;
                CHECK(!ioctl(drm,IOC(0x5b,sizeof(bandGem[i])),&bandGem[i]));
                struct map_offset bm={bandGem[i].handle,0,0,2,0};CHECK(!ioctl(drm,IOC(0x64,sizeof(bm)),&bm));
                bandMap[i]=mmap(NULL,bandGem[i].size,PROT_READ|PROT_WRITE,MAP_SHARED,drm,(off_t)bm.offset);CHECK(bandMap[i]!=MAP_FAILED);
                for(uint32_t y=0;y<b->height;++y)memcpy(bandMap[i]+(uint64_t)y*b->width,
                    overlayMap+(uint64_t)(b->y+y)*WIDTH+b->x,(size_t)b->width*4);
                bandPrime[i]=(struct prime){bandGem[i].handle,O_CLOEXEC|O_RDWR,-1};CHECK(!ioctl(drm,IOC(0x2d,sizeof(bandPrime[i])),&bandPrime[i]));
                struct BrookVkSceneLayer band={0,b->width,b->width,b->height,0,0,b->width,b->height,
                    b->x,b->y,b->width,b->height,0,0,WIDTH,HEIGHT,255|BROOK_VK_STRAIGHT_ALPHA};
                sources[sceneCount]=(struct BrookVkExternalSource){bandPrime[i].fd,bandGem[i].size/4,-1};
                memcpy(scene+4+sceneCount++*17,&band,sizeof(band));
            }
        }else{
            overlayPrime=(struct prime){overlayGem.handle,O_CLOEXEC|O_RDWR,-1};CHECK(!ioctl(drm,IOC(0x2d,sizeof(overlayPrime)),&overlayPrime));
            CHECK(sceneCount<BROOK_VK_MAX_LAYERS);
            struct BrookVkSceneLayer overlay={0,WIDTH,WIDTH,HEIGHT,0,0,WIDTH,HEIGHT,0,0,WIDTH,HEIGHT,0,0,WIDTH,HEIGHT,255|BROOK_VK_STRAIGHT_ALPHA};
            sources[sceneCount]=(struct BrookVkExternalSource){overlayPrime.fd,overlayGem.size/4,-1};
            memcpy(scene+4+sceneCount++*17,&overlay,sizeof(overlay));
        }
    }
    scene[2]=sceneCount;
    CHECK(found==3 && (!legacy || legacyCount));
    BrookVkRenderer *batchRenderer=NULL;int batchOutput=-1;uint32_t batchScenes[2*BROOK_VK_TILE_STRIDE]={0};
    struct BrookGpuPresentRegions batch={0};size_t batchWords=0;uint32_t *regionPixels=NULL;
    struct create regionBackdropGem={0};struct prime regionBackdropPrime={0};uint32_t *regionBackdropMap=NULL;
    struct BrookVkExternalSource regionSources[BROOK_VK_MAX_LAYERS];
    if(regionCheck){
        CHECK(WIDTH>=100 && HEIGHT>=100);size_t sourceWords[BROOK_VK_MAX_LAYERS];
        for(unsigned i=0;i<sceneCount;++i){sourceWords[i]=sources[i].words;regionSources[i]=sources[i];}
        const uint32_t xs[]={10,WIDTH-50},ys[]={10,HEIGHT-50};batch.count=2;
        regionBackdropGem.size=2*1600*4;CHECK(!ioctl(drm,IOC(0x5b,sizeof(regionBackdropGem)),&regionBackdropGem));
        struct map_offset map={regionBackdropGem.handle,0,0,2,0};CHECK(!ioctl(drm,IOC(0x64,sizeof(map)),&map));
        regionBackdropMap=mmap(NULL,regionBackdropGem.size,PROT_READ|PROT_WRITE,MAP_SHARED,drm,(off_t)map.offset);CHECK(regionBackdropMap!=MAP_FAILED);
        regionBackdropPrime=(struct prime){regionBackdropGem.handle,O_CLOEXEC|O_RDWR,-1};CHECK(!ioctl(drm,IOC(0x2d,sizeof(regionBackdropPrime)),&regionBackdropPrime));
        struct DesktopDamageSet damage={0};struct DesktopPackedTile tiles[2];
        for(unsigned i=0;i<2;++i)desktop_damage_add(&damage,(struct DesktopDamageRect){xs[i],ys[i],xs[i]+40,ys[i]+40},WIDTH,HEIGHT);
        CHECK(damage.count==2 && desktop_tile_pack(scene,WORDS,sourceWords,sceneCount,&damage,backdropMap,regionBackdropMap,regionBackdropGem.size/4,
                                                   batchScenes,2*BROOK_VK_TILE_STRIDE,tiles,2));
        for(unsigned i=0;i<2;++i){const struct DesktopPackedTile *t=&tiles[i];
            batch.regions[i]=(struct BrookGpuPresentRegion){t->offsetBytes,t->x,t->y,t->width,t->height,t->width,0};}
        regionSources[0]=(struct BrookVkExternalSource){regionBackdropPrime.fd,regionBackdropGem.size/4,-1};
        batchOutput=(int)syscall(BROOK_SYS_GPU_OUTPUT_ALLOC,2*1600*4,0,0,0,0,0);CHECK(batchOutput>=0);
        off_t size=lseek(batchOutput,0,SEEK_END);CHECK(size>0 && !(size%4));batchWords=(size_t)size/4;
        CHECK(lseek(batchOutput,0,SEEK_SET)==0);batchRenderer=brook_vk_renderer_create_batch_output(2,0);CHECK(batchRenderer);
    }
    struct create dst={WIDTH*HEIGHT*4,0,0};CHECK(!ioctl(drm,IOC(0x5b,sizeof(dst)),&dst));
    struct map_offset dm={dst.handle,0,0,2,0};CHECK(!ioctl(drm,IOC(0x64,sizeof(dm)),&dm));
    uint32_t *result=mmap(NULL,dst.size,PROT_READ|PROT_WRITE,MAP_SHARED,drm,(off_t)dm.offset);CHECK(result!=MAP_FAILED);
    struct prime target={dst.handle,O_CLOEXEC|O_RDWR,-1};CHECK(!ioctl(drm,IOC(0x2d,sizeof(target)),&target));
    int protectedFd=(int)syscall(BROOK_SYS_GPU_OUTPUT_ALLOC,(long)dst.size,0L,0L,0L,0L,0L);CHECK(protectedFd>=0);
    BrookVkRenderer *renderer=brook_vk_renderer_create_multi_output(WORDS,0);CHECK(renderer);
    if(!tracked)fprintf(stderr,"DESKTOP_VK_DEVICE %s\n",brook_vk_renderer_device(renderer));
    long ticket=syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_RESERVE,protectedFd,0,0,0,0);CHECK(ticket>0);
    BrookVkResult status=brook_vk_renderer_compose_multi_output(renderer,scene,WORDS,sources,sceneCount,protectedFd,dst.size/4,10000000000ULL);
    if(status==BROOK_VK_UNCERTAIN)uncertain("DESKTOP_VK_UNCERTAIN");
    CHECK(status==BROOK_VK_COMPLETE);
    CHECK(syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_OBSERVE,ticket,0,0,0,0)==1);
    struct BrookGpuPresentProbe probe={(uint64_t)ticket,leaseCheck?0:40,leaseCheck?0:40,WIDTH,HEIGHT,WIDTH,0,{0}};
    if(staleCheck){
        if(titleCheck)CHECK(!syscall(509,(long)windows[0].id,(long)"Changed title",0L,0L,0L,0L));
        else CHECK(!syscall(BROOK_SYS_WM_PLACE_WINDOW,(long)windows[0].id,(long)incarnations[0],21L,20L,0L,0L));
        errno=0;CHECK(syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_PRESENT_PROBE,&probe,0,0,0,0)==-1 && errno==ESTALE);
        errno=0;CHECK(syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_RELEASE,ticket,0,0,0,0)==-1 && errno==ENOENT);
        if(titleCheck)CHECK(!syscall(509,(long)windows[0].id,(long)"Vulkan scene fixture",0L,0L,0L,0L));
        else CHECK(!syscall(BROOK_SYS_WM_PLACE_WINDOW,(long)windows[0].id,(long)incarnations[0],20L,20L,0L,0L));
        desktop.reserved=0;
        CHECK(!syscall(BROOK_SYS_DESKTOP_LAYERS,BROOK_DESKTOP_LAYERS_CAPTURE_TRACKED,&desktop,sizeof(desktop),0,0,0) && desktop.reserved);
        if(backdropEnabled)CHECK(!syscall(BROOK_SYS_DESKTOP_LAYERS,BROOK_DESKTOP_BACKDROP_TRACKED_CAPTURE,&backdrop,sizeof(backdrop),0,0,0));
        for(unsigned i=0;i<desktop.count;++i)if(desktop.layers[i].flags&BROOK_DESKTOP_LAYER_LEGACY_OPAQUE)
            for(uint64_t p=0;p<(uint64_t)desktop.layers[i].stride*desktop.layers[i].height;++p)((uint32_t*)maps[i])[p]|=0xff000000u;
        ticket=syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_RESERVE,protectedFd,0,0,0,0);CHECK(ticket>0);
        status=brook_vk_renderer_compose_multi_output(renderer,scene,WORDS,sources,sceneCount,protectedFd,dst.size/4,10000000000ULL);
        if(status==BROOK_VK_UNCERTAIN)uncertain("DESKTOP_VK_UNCERTAIN");
        CHECK(status==BROOK_VK_COMPLETE);
        CHECK(syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_OBSERVE,ticket,0,0,0,0)==1);
        probe.ticket=(uint64_t)ticket;
    }
    if(leaseHold){
        CHECK(!syscall(509,(long)windows[0].id,(long)"Newer frame title",0L,0L,0L,0L));
        CHECK(!syscall(508,(long)windows[0].id,0L,0L,0L,0L,0L));
    }
    if(presentProbe)CHECK(!syscall(BROOK_SYS_GPU_HANDOFF_CTL,leaseCheck?BROOK_GPU_HANDOFF_PRESENT_LEASE:BROOK_GPU_HANDOFF_PRESENT_PROBE,&probe,0,0,0,0));
    else CHECK(!syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_RELEASE,ticket,0,0,0,0));
    if(regionCheck){
        long batchTicket=syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_RESERVE,batchOutput,0,0,0,0);CHECK(batchTicket>0);
        batch.ticket=(uint64_t)batchTicket;batch.reserved=1;errno=0;
        CHECK(syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_PRESENT_REGIONS,&batch,0,0,0,0)==-1 && errno==EINVAL);
        batch.reserved=0;
        status=brook_vk_renderer_compose_batch_output(batchRenderer,batchScenes,2*BROOK_VK_TILE_STRIDE,2,regionSources,sceneCount,batchOutput,batchWords,10000000000ULL);
        if(status==BROOK_VK_UNCERTAIN)uncertain("DESKTOP_REGION_UNCERTAIN");
        CHECK(status==BROOK_VK_COMPLETE && syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_OBSERVE,batchTicket,0,0,0,0)==1);
        CHECK(!syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_PRESENT_REGIONS,&batch,0,0,0,0));
        if(dependentCheck){
            const long caps=syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_PRESENT_CAPS,0,0,0,0,0);
            CHECK(caps>0 && (caps&BROOK_GPU_PRESENT_CAP_DEPENDENT));
            int output2=(int)syscall(BROOK_SYS_GPU_OUTPUT_ALLOC,(long)(batchWords*4),0L,0L,0L,0L,0L);CHECK(output2>=0);
            BrookVkRenderer *renderer2=brook_vk_renderer_create_batch_output(2,0);CHECK(renderer2);
            uint64_t baseline=(uint64_t)batchTicket;
            for(unsigned mode=0;mode<4;++mode){
                errno=0;CHECK(syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_RESERVE_DEPENDENT,output2,baseline-1,0,0,0)==-1 && errno==ESTALE);
                long older=syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_RESERVE,mode==3?protectedFd:batchOutput,0,0,0,0);CHECK(older>0);
                desktop.reserved=0;
                CHECK(!syscall(BROOK_SYS_DESKTOP_LAYERS,BROOK_DESKTOP_LAYERS_CAPTURE_TRACKED,&desktop,sizeof(desktop),0,0,0) && desktop.reserved);
                CHECK(!syscall(BROOK_SYS_DESKTOP_LAYERS,BROOK_DESKTOP_BACKDROP_TRACKED_CAPTURE,&backdrop,sizeof(backdrop),0,0,0));
                for(unsigned i=0;i<desktop.count;++i){
                    CHECK(!syscall(BROOK_SYS_DESKTOP_LAYERS,BROOK_DESKTOP_CHROME_QUERY,&chrome[i],sizeof(chrome[i]),0,0,0));
                    CHECK(!syscall(BROOK_SYS_DESKTOP_LAYERS,BROOK_DESKTOP_CHROME_TRACKED_CAPTURE,&chrome[i],sizeof(chrome[i]),0,0,0));
                }
                long newer=syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_RESERVE_DEPENDENT,output2,baseline,0,0,0);CHECK(newer>older);
                status=mode==3?brook_vk_renderer_compose_multi_output(renderer,scene,WORDS,sources,sceneCount,protectedFd,dst.size/4,10000000000ULL):
                    brook_vk_renderer_compose_batch_output(batchRenderer,batchScenes,2*BROOK_VK_TILE_STRIDE,2,regionSources,sceneCount,batchOutput,batchWords,10000000000ULL);
                if(status==BROOK_VK_UNCERTAIN)uncertain("DESKTOP_DEPENDENT_OLDER_UNCERTAIN");
                CHECK(status==BROOK_VK_COMPLETE && syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_OBSERVE,older,0,0,0,0)==1);
                status=brook_vk_renderer_compose_batch_output(renderer2,batchScenes,2*BROOK_VK_TILE_STRIDE,2,regionSources,sceneCount,output2,batchWords,10000000000ULL);
                if(status==BROOK_VK_UNCERTAIN)uncertain("DESKTOP_DEPENDENT_NEWER_UNCERTAIN");
                CHECK(status==BROOK_VK_COMPLETE && syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_OBSERVE,newer,0,0,0,0)==1);
                struct BrookGpuPresentRegions oldBatch=batch,newBatch=batch;oldBatch.ticket=(uint64_t)older;newBatch.ticket=(uint64_t)newer;
                if(mode==3){struct BrookGpuPresentProbe full={(uint64_t)older,0,0,WIDTH,HEIGHT,WIDTH,0,{0}};
                    CHECK(!syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_PRESENT_LEASE,&full,0,0,0,0));}
                else if(mode!=2)CHECK(!syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_PRESENT_REGIONS,&oldBatch,0,0,0,0));
                if(mode==0 || mode==3){if(mode==0)newBatch.count=1;errno=0;
                    CHECK(syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_PRESENT_REGIONS,&newBatch,0,0,0,0)==-1 && errno==ESTALE);
                    baseline=(uint64_t)older;
                }else {CHECK(!syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_PRESENT_REGIONS,&newBatch,0,0,0,0));
                    baseline=(uint64_t)newer;
                    if(mode==2){errno=0;CHECK(syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_PRESENT_REGIONS,&oldBatch,0,0,0,0)==-1 && errno==ESTALE);}
                }
            }
            long expired=syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_RESERVE_DEPENDENT,output2,baseline,0,0,0);CHECK(expired>0);
            status=brook_vk_renderer_compose_batch_output(renderer2,batchScenes,2*BROOK_VK_TILE_STRIDE,2,regionSources,sceneCount,output2,batchWords,10000000000ULL);
            if(status==BROOK_VK_UNCERTAIN)uncertain("DESKTOP_DEPENDENT_EXPIRED_UNCERTAIN");
            CHECK(status==BROOK_VK_COMPLETE && syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_OBSERVE,expired,0,0,0,0)==1);
            usleep(650000);struct BrookGpuPresentRegions expiredBatch=batch;expiredBatch.ticket=(uint64_t)expired;errno=0;
            CHECK(syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_PRESENT_REGIONS,&expiredBatch,0,0,0,0)==-1 && errno==ESTALE);
            // Restore a full lease for the rest of the fixture.
            long full=syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_RESERVE,protectedFd,0,0,0,0);CHECK(full>0);
            status=brook_vk_renderer_compose_multi_output(renderer,scene,WORDS,sources,sceneCount,protectedFd,dst.size/4,10000000000ULL);
            if(status==BROOK_VK_UNCERTAIN)uncertain("DESKTOP_DEPENDENT_RESTORE_UNCERTAIN");
            CHECK(status==BROOK_VK_COMPLETE && syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_OBSERVE,full,0,0,0,0)==1);
            struct BrookGpuPresentProbe reset={(uint64_t)full,0,0,WIDTH,HEIGHT,WIDTH,0,{0}};
            CHECK(!syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_PRESENT_LEASE,&reset,0,0,0,0));
            CHECK(brook_vk_renderer_destroy(renderer2));close(output2);
            fprintf(stderr,"DESKTOP_DEPENDENT_PRESENT_PASS stale_baseline=1 incomplete=1 covered=1 newer_first=1 full_interposed=1 lease_expiry=1\n");
        }
        if(overlapCheck){
            long older=syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_RESERVE,batchOutput,0,0,0,0);CHECK(older>0);
            desktop.reserved=0;
            CHECK(!syscall(BROOK_SYS_DESKTOP_LAYERS,BROOK_DESKTOP_LAYERS_CAPTURE_TRACKED,&desktop,sizeof(desktop),0,0,0) && desktop.reserved);
            CHECK(!syscall(BROOK_SYS_DESKTOP_LAYERS,BROOK_DESKTOP_BACKDROP_TRACKED_CAPTURE,&backdrop,sizeof(backdrop),0,0,0));
            for(unsigned i=0;i<desktop.count;++i){
                CHECK(!syscall(BROOK_SYS_DESKTOP_LAYERS,BROOK_DESKTOP_CHROME_QUERY,&chrome[i],sizeof(chrome[i]),0,0,0));
                CHECK(!syscall(BROOK_SYS_DESKTOP_LAYERS,BROOK_DESKTOP_CHROME_TRACKED_CAPTURE,&chrome[i],sizeof(chrome[i]),0,0,0));
            }
            long newer=syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_RESERVE,protectedFd,0,0,0,0);CHECK(newer>older);
            status=brook_vk_renderer_compose_multi_output(renderer,scene,WORDS,sources,sceneCount,protectedFd,dst.size/4,10000000000ULL);
            if(status==BROOK_VK_UNCERTAIN)uncertain("DESKTOP_OVERLAP_UNCERTAIN");
            CHECK(status==BROOK_VK_COMPLETE && syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_OBSERVE,newer,0,0,0,0)==1);
            batch.ticket=(uint64_t)newer;errno=0;
            CHECK(syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_PRESENT_REGIONS,&batch,0,0,0,0)==-1 && errno==ESTALE);
            CHECK(!syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_RELEASE,older,0,0,0,0));
            fprintf(stderr,"DESKTOP_OVERLAP_STAGING_PASS second_partial_rejected=1\n");

            /* Two full outputs may be submitted before either completes. The
             * second scene is staged before submission so these shared input
             * buffers remain immutable until both GPU fences have signaled. */
            int secondOutput=(int)syscall(BROOK_SYS_GPU_OUTPUT_ALLOC,(long)dst.size,0L,0L,0L,0L,0L);CHECK(secondOutput>=0);
            BrookVkRenderer *pair[2]={brook_vk_renderer_create_multi_output(WORDS,0),brook_vk_renderer_create_multi_output(WORDS,0)};
            CHECK(pair[0] && pair[1]);
            long pairTickets[2];
            pairTickets[0]=syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_RESERVE,protectedFd,0,0,0,0);CHECK(pairTickets[0]>0);
            desktop.reserved=0;
            CHECK(!syscall(BROOK_SYS_DESKTOP_LAYERS,BROOK_DESKTOP_LAYERS_CAPTURE_TRACKED,&desktop,sizeof(desktop),0,0,0) && desktop.reserved);
            CHECK(!syscall(BROOK_SYS_DESKTOP_LAYERS,BROOK_DESKTOP_BACKDROP_TRACKED_CAPTURE,&backdrop,sizeof(backdrop),0,0,0));
            pairTickets[1]=syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_RESERVE,secondOutput,0,0,0,0);CHECK(pairTickets[1]>pairTickets[0]);
            int outputs[2]={protectedFd,secondOutput};
            for(unsigned i=0;i<2;++i){
                status=brook_vk_renderer_submit_batch_output(pair[i],scene,WORDS,1,sources,sceneCount,outputs[i],dst.size/4);
                if(status!=BROOK_VK_PENDING)uncertain("DESKTOP_OVERLAP_SUBMIT_UNCERTAIN");
            }
            unsigned pending=0;
            for(unsigned i=0;i<2;++i){
                status=brook_vk_renderer_poll(pair[i]);
                for(unsigned n=0;n<10000 && status==BROOK_VK_PENDING;++n){++pending;usleep(1000);status=brook_vk_renderer_poll(pair[i]);}
                if(status!=BROOK_VK_COMPLETE)uncertain("DESKTOP_OVERLAP_POLL_UNCERTAIN");
                CHECK(syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_OBSERVE,pairTickets[i],0,0,0,0)==1);
            }
            struct BrookGpuPresentProbe ordered={(uint64_t)pairTickets[1],0,0,WIDTH,HEIGHT,WIDTH,0,{0}};
            CHECK(!syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_PRESENT_LEASE,&ordered,0,0,0,0));
            ordered.ticket=(uint64_t)pairTickets[0];errno=0;
            CHECK(syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_PRESENT_LEASE,&ordered,0,0,0,0)==-1 && errno==ESTALE);
            CHECK(brook_vk_renderer_destroy(pair[0]) && brook_vk_renderer_destroy(pair[1]));close(secondOutput);
            fprintf(stderr,"DESKTOP_OVERLAP_ORDER_PASS submitted=2 pending_polls=%u newer_first=1 older_stale=1\n",pending);

            /* The older packed output is reserved while idle, so it may be
             * presented as regions. A newer full output is reserved while
             * that ticket is outstanding. Sources are immutable during both
             * submissions; exercise both presentation orders. */
            for(unsigned reverse=0;reverse<2;++reverse){
                long partial=syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_RESERVE,batchOutput,0,0,0,0);CHECK(partial>0);
                desktop.reserved=0;
                CHECK(!syscall(BROOK_SYS_DESKTOP_LAYERS,BROOK_DESKTOP_LAYERS_CAPTURE_TRACKED,&desktop,sizeof(desktop),0,0,0) && desktop.reserved);
                CHECK(!syscall(BROOK_SYS_DESKTOP_LAYERS,BROOK_DESKTOP_BACKDROP_TRACKED_CAPTURE,&backdrop,sizeof(backdrop),0,0,0));
                for(unsigned i=0;i<desktop.count;++i){
                    CHECK(!syscall(BROOK_SYS_DESKTOP_LAYERS,BROOK_DESKTOP_CHROME_QUERY,&chrome[i],sizeof(chrome[i]),0,0,0));
                    CHECK(!syscall(BROOK_SYS_DESKTOP_LAYERS,BROOK_DESKTOP_CHROME_TRACKED_CAPTURE,&chrome[i],sizeof(chrome[i]),0,0,0));
                }
                long full=syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_RESERVE,protectedFd,0,0,0,0);CHECK(full>partial);
                status=brook_vk_renderer_submit_batch_output(batchRenderer,batchScenes,2*BROOK_VK_TILE_STRIDE,2,regionSources,sceneCount,batchOutput,batchWords);
                if(status!=BROOK_VK_PENDING)uncertain("DESKTOP_PARTIAL_OVERLAP_SUBMIT_UNCERTAIN");
                status=brook_vk_renderer_submit_batch_output(renderer,scene,WORDS,1,sources,sceneCount,protectedFd,dst.size/4);
                if(status!=BROOK_VK_PENDING)uncertain("DESKTOP_FULL_OVERLAP_SUBMIT_UNCERTAIN");
                BrookVkRenderer *overlap[2]={batchRenderer,renderer};long overlapTickets[2]={partial,full};
                for(unsigned i=0;i<2;++i){
                    status=brook_vk_renderer_poll(overlap[i]);
                    for(unsigned n=0;n<10000 && status==BROOK_VK_PENDING;++n){usleep(1000);status=brook_vk_renderer_poll(overlap[i]);}
                    if(status!=BROOK_VK_COMPLETE)uncertain("DESKTOP_PARTIAL_OVERLAP_POLL_UNCERTAIN");
                    CHECK(syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_OBSERVE,overlapTickets[i],0,0,0,0)==1);
                }
                batch.ticket=(uint64_t)partial;
                struct BrookGpuPresentProbe newerProbe={(uint64_t)full,0,0,WIDTH,HEIGHT,WIDTH,0,{0}};
                if(!reverse)CHECK(!syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_PRESENT_REGIONS,&batch,0,0,0,0));
                CHECK(!syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_PRESENT_LEASE,&newerProbe,0,0,0,0));
                if(reverse){errno=0;CHECK(syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_PRESENT_REGIONS,&batch,0,0,0,0)==-1 && errno==ESTALE);}
            }
            fprintf(stderr,"DESKTOP_PARTIAL_FULL_OVERLAP_PASS submitted_pairs=2 partial_first=1 newer_first_stale=1\n");
        }
        usleep(650000);
        batchTicket=syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_RESERVE,batchOutput,0,0,0,0);CHECK(batchTicket>0);
        batch.ticket=(uint64_t)batchTicket;
        status=brook_vk_renderer_compose_batch_output(batchRenderer,batchScenes,2*BROOK_VK_TILE_STRIDE,2,regionSources,sceneCount,batchOutput,batchWords,10000000000ULL);
        if(status==BROOK_VK_UNCERTAIN)uncertain("DESKTOP_REGION_EXPIRED_UNCERTAIN");
        CHECK(status==BROOK_VK_COMPLETE && syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_OBSERVE,batchTicket,0,0,0,0)==1);
        errno=0;CHECK(syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_PRESENT_REGIONS,&batch,0,0,0,0)==-1 && errno==ESTALE);
        errno=0;CHECK(syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_RELEASE,batchTicket,0,0,0,0)==-1 && errno==ENOENT);
        uint32_t copyScene[21]={80,40,1,0};
        struct BrookVkSceneLayer packedCopy={0,80,80,40,0,0,80,40,0,0,80,40,0,0,80,40,255};
        memcpy(copyScene+4,&packedCopy,sizeof(packedCopy));
        struct BrookVkExternalSource packedInput={batchOutput,batchWords,-1};
        status=brook_vk_renderer_compose_multi_output(renderer,copyScene,21,&packedInput,1,target.fd,dst.size/4,10000000000ULL);
        if(status==BROOK_VK_UNCERTAIN)uncertain("DESKTOP_REGION_READBACK_UNCERTAIN");
        CHECK(status==BROOK_VK_COMPLETE);struct domain cpu={dst.handle,1,0};CHECK(!ioctl(drm,IOC(0x5f,sizeof(cpu)),&cpu));
        regionPixels=malloc(3200*4);CHECK(regionPixels);memcpy(regionPixels,result,3200*4);
        CHECK(brook_vk_renderer_destroy(batchRenderer));close(batchOutput);
        close(regionBackdropPrime.fd);munmap(regionBackdropMap,regionBackdropGem.size);
        struct gem_close closeBackdrop={regionBackdropGem.handle,0};CHECK(!ioctl(drm,IOC(9,sizeof(closeBackdrop)),&closeBackdrop));
        fprintf(stderr,"DESKTOP_REGION_PRESENT_PASS regions=2 expired_rejected=1\n");
    }
    if(repeatLease){
        struct timespec start={0},end={0};CHECK(!clock_gettime(CLOCK_MONOTONIC,&start));
        for(unsigned frame=1;frame<4;++frame){
            struct domain cpu={gem[nativeIndex].handle,1,1};CHECK(!ioctl(drm,IOC(0x5f,sizeof(cpu)),&cpu));
            ((uint32_t*)maps[nativeIndex])[0]=0xff000000u|(frame*0x00202020u);
            ticket=syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_RESERVE,protectedFd,0,0,0,0);CHECK(ticket>0);
            status=brook_vk_renderer_compose_multi_output(renderer,scene,WORDS,sources,sceneCount,protectedFd,dst.size/4,10000000000ULL);
            if(status==BROOK_VK_UNCERTAIN)uncertain("DESKTOP_VK_REPEAT_UNCERTAIN");
            CHECK(status==BROOK_VK_COMPLETE);
            CHECK(syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_OBSERVE,ticket,0,0,0,0)==1);
            probe.ticket=(uint64_t)ticket;
            CHECK(!syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_PRESENT_LEASE,&probe,0,0,0,0));
        }
        CHECK(!clock_gettime(CLOCK_MONOTONIC,&end));
        long elapsedMs=(end.tv_sec-start.tv_sec)*1000+(end.tv_nsec-start.tv_nsec)/1000000;
        fprintf(stderr,"DESKTOP_LEASE_REPEAT_PASS frames=4 repeat_ms=%ld\n",elapsedMs);
    }
    if(liveLease){
        struct timespec start={0},end={0};CHECK(!clock_gettime(CLOCK_MONOTONIC,&start));
        for(unsigned frame=1;frame<4;++frame){
            ((uint32_t*)(uintptr_t)windows[0].pixels)[0]=0xff000000u|(frame*0x00202020u);
            CHECK(!syscall(508,(long)windows[0].id,0L,0L,0L,0L,0L));
            CHECK(!syscall(BROOK_SYS_WM_PLACE_WINDOW,(long)windows[0].id,(long)incarnations[0],20L+frame*5,20L+frame*3,0L,0L));
            for(unsigned i=0;i<desktop.count;++i){
                struct domain cpu={gem[i].handle,1,1};CHECK(!ioctl(drm,IOC(0x5f,sizeof(cpu)),&cpu));
            }
            if(!liveCache){struct domain cpu={backdropGem.handle,1,1};CHECK(!ioctl(drm,IOC(0x5f,sizeof(cpu)),&cpu));}
            desktop.reserved=0;
            CHECK(!syscall(BROOK_SYS_DESKTOP_LAYERS,BROOK_DESKTOP_LAYERS_CAPTURE_TRACKED,&desktop,sizeof(desktop),0,0,0) && desktop.reserved);
            CHECK(desktop.layers[nativeIndex].window==windows[0].id && desktop.layers[nativeIndex].clientX==(int32_t)(20+frame*5) &&
                  desktop.layers[nativeIndex].clientY==(int32_t)(20+frame*3) &&
                  ((uint32_t*)maps[nativeIndex])[0]==(0xff000000u|(frame*0x00202020u)));
            if(!liveCache)CHECK(!syscall(BROOK_SYS_DESKTOP_LAYERS,BROOK_DESKTOP_BACKDROP_TRACKED_CAPTURE,&backdrop,sizeof(backdrop),0,0,0));
            for(unsigned i=0;i<desktop.count;++i){
                struct BrookDesktopLayer *l=&desktop.layers[i];CHECK(l->window==layerIds[i]);
                if(l->flags&BROOK_DESKTOP_LAYER_LEGACY_OPAQUE)
                    for(uint64_t p=0;p<(uint64_t)l->stride*l->height;++p)((uint32_t*)maps[i])[p]|=0xff000000u;
                uint32_t *renderLayer=scene+4+clientIndex[i]*17;
                renderLayer[8]=(uint32_t)l->clientX;renderLayer[9]=(uint32_t)l->clientY;
                if(liveCache && l->window!=windows[0].id)continue;
                if(chromeMap[i]){struct domain cpu={chromeGem[i].handle,1,1};CHECK(!ioctl(drm,IOC(0x5f,sizeof(cpu)),&cpu));}
                CHECK(!syscall(BROOK_SYS_DESKTOP_LAYERS,BROOK_DESKTOP_CHROME_QUERY,&chrome[i],sizeof(chrome[i]),0,0,0));
                struct BrookDesktopLayer *c=&chrome[i].layers[0];
                CHECK((c->bytes!=0)==(chromeMap[i]!=NULL));
                if(c->bytes)CHECK(c->bytes<=chromeGem[i].size && c->pixels==(uintptr_t)chromeMap[i]);
                CHECK(!syscall(BROOK_SYS_DESKTOP_LAYERS,BROOK_DESKTOP_CHROME_TRACKED_CAPTURE,&chrome[i],sizeof(chrome[i]),0,0,0));
                if(chromeMap[i]){
                    uint32_t *renderChrome=scene+4+chromeIndex[i]*17;
                    renderChrome[8]=(uint32_t)c->clientX;renderChrome[9]=(uint32_t)c->clientY;
                }
            }
            {struct domain cpu={overlayGem.handle,1,1};CHECK(!ioctl(drm,IOC(0x5f,sizeof(cpu)),&cpu));}
            CHECK(!capture_overlay(overlayMap,WIDTH,HEIGHT));
            ticket=syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_RESERVE,protectedFd,0,0,0,0);CHECK(ticket>0);
            status=brook_vk_renderer_compose_multi_output(renderer,scene,WORDS,sources,sceneCount,protectedFd,dst.size/4,10000000000ULL);
            if(status==BROOK_VK_UNCERTAIN)uncertain("DESKTOP_VK_LIVE_UNCERTAIN");
            CHECK(status==BROOK_VK_COMPLETE);
            CHECK(syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_OBSERVE,ticket,0,0,0,0)==1);
            probe.ticket=(uint64_t)ticket;
            CHECK(!syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_PRESENT_LEASE,&probe,0,0,0,0));
        }
        CHECK(!clock_gettime(CLOCK_MONOTONIC,&end));
        long elapsedMs=(end.tv_sec-start.tv_sec)*1000+(end.tv_nsec-start.tv_nsec)/1000000;
        fprintf(stderr,"DESKTOP_LIVE_LEASE_PASS frames=4 cached=%d update_ms=%ld\n",liveCache,elapsedMs);
    }
    if(leaseCheck)usleep(650000); // exceed the current 500 ms lease before detach
    if(leaseHold){fprintf(stderr,"LEASE_EXPIRED_ATTACHED\n");sleep(45);}
    CHECK(!syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_DETACH,0,0,0,0,0));
    memset(scene,0,WORDS*sizeof(*scene));scene[0]=WIDTH;scene[1]=HEIGHT;scene[2]=1;scene[3]=0;
    struct BrookVkSceneLayer copy={0,WIDTH,WIDTH,HEIGHT,0,0,WIDTH,HEIGHT,0,0,WIDTH,HEIGHT,0,0,WIDTH,HEIGHT,255};
    memcpy(scene+4,&copy,sizeof(copy));struct BrookVkExternalSource input={protectedFd,dst.size/4,-1};
    status=brook_vk_renderer_compose_multi_output(renderer,scene,WORDS,&input,1,target.fd,dst.size/4,10000000000ULL);
    if(status==BROOK_VK_UNCERTAIN)uncertain("DESKTOP_VK_COPY_UNCERTAIN");
    CHECK(status==BROOK_VK_COMPLETE);
    struct domain d={dst.handle,1,0};CHECK(!ioctl(drm,IOC(0x5f,sizeof(d)),&d));
    if(coverPoint){
        CHECK(coverX>=80 && coverX<80+WINW && coverY>=80 && coverY<80+WINH && coverX<WIDTH && coverY<HEIGHT);
        CHECK(result[coverY*(uint64_t)WIDTH+coverX]==pattern(1,(unsigned)coverX-80,(unsigned)coverY-80));
        fprintf(stderr,"DESKTOP_OCCLUSION_POINT_PASS\n");
    }
    if(presentProbe){
        const unsigned positions[4]={0,WIDTH-1,(HEIGHT-1)*WIDTH,HEIGHT*WIDTH-1};
        for(unsigned i=0;i<4;++i)CHECK((probe.samples[i]&0xffffffu)==(result[positions[i]]&0xffffffu));
        fprintf(stderr,"DESKTOP_DISPLAY_PROBE_PASS corners=%08x,%08x,%08x,%08x\n",probe.samples[0],probe.samples[1],probe.samples[2],probe.samples[3]);
    }
    if(regionCheck){
        CHECK(regionPixels);
        for(unsigned i=0;i<batch.count;++i){const struct BrookGpuPresentRegion *p=&batch.regions[i];
            for(unsigned y=0;y<p->height;++y)for(unsigned x=0;x<p->width;++x){
                const uint32_t actual=regionPixels[p->offset/4+y*p->stride+x];
                const uint32_t expected=result[(uint64_t)(p->y+y)*WIDTH+p->x+x];
                if(actual!=expected){fprintf(stderr,"DESKTOP_REGION_PIXEL_MISMATCH region=%u x=%u y=%u\n",i,x,y);return 1;}
            }
        }
        free(regionPixels);fprintf(stderr,"DESKTOP_REGION_PIXEL_PASS regions=2\n");
    }
    for(unsigned y=0;y<HEIGHT;++y)for(unsigned x=0;x<WIDTH;++x){
        uint32_t expected=backdropEnabled?backdropMap[(uint64_t)y*backdrop.layers[0].stride+x]:0;
        for(unsigned i=0;i<desktop.count;++i){const struct BrookDesktopLayer *l=&desktop.layers[i];
            int64_t sx=(int64_t)x-l->clientX,sy=(int64_t)y-l->clientY;
            if(sx>=0 && sy>=0 && sx<l->width && sy<l->height && sx<l->clientWidth && sy<l->clientHeight)
                expected=straight_over(((uint32_t*)maps[i])[sy*l->stride+sx],expected);
            if(chromeMap[i]){const struct BrookDesktopLayer *c=&chrome[i].layers[0];
                sx=(int64_t)x-c->clientX;sy=(int64_t)y-c->clientY;
                if(sx>=0 && sy>=0 && sx<c->width && sy<c->height)
                    expected=straight_over(chromeMap[i][sy*c->stride+sx],expected);
            }
        }
        if(overlayMap)expected=straight_over(overlayMap[(uint64_t)y*WIDTH+x],expected);
        if(result[y*WIDTH+x]!=expected){fprintf(stderr,"DESKTOP_VK_MISMATCH x=%u y=%u got=%08x expected=%08x\n",x,y,result[y*WIDTH+x],expected);return 1;}
    }
    CHECK(brook_vk_renderer_destroy(renderer));close(protectedFd);close(target.fd);munmap(result,dst.size);
    struct gem_close closeDst={dst.handle,0};CHECK(!ioctl(drm,IOC(9,sizeof(closeDst)),&closeDst));
    for(unsigned i=0;i<desktop.count;++i){close(prime[i].fd);munmap(maps[i],gem[i].size);struct gem_close c={gem[i].handle,0};CHECK(!ioctl(drm,IOC(9,sizeof(c)),&c));}
    for(unsigned i=0;i<desktop.count;++i)if(chromeMap[i]){close(chromePrime[i].fd);munmap(chromeMap[i],chromeGem[i].size);struct gem_close c={chromeGem[i].handle,0};CHECK(!ioctl(drm,IOC(9,sizeof(c)),&c));}
    if(backdropEnabled){close(backdropPrime.fd);munmap(backdropMap,backdropGem.size);struct gem_close c={backdropGem.handle,0};CHECK(!ioctl(drm,IOC(9,sizeof(c)),&c));}
    for(uint32_t i=0;i<capturedBands.count && getenv("BROOK_COMPOSE_OVERLAY_BANDS");++i){
        close(bandPrime[i].fd);munmap(bandMap[i],bandGem[i].size);struct gem_close c={bandGem[i].handle,0};CHECK(!ioctl(drm,IOC(9,sizeof(c)),&c));
    }
    if(overlayMap){if(!getenv("BROOK_COMPOSE_OVERLAY_BANDS"))close(overlayPrime.fd);munmap(overlayMap,overlayGem.size);struct gem_close c={overlayGem.handle,0};CHECK(!ioctl(drm,IOC(9,sizeof(c)),&c));}
    for(unsigned i=0;i<2;++i)CHECK(!syscall(507,(long)windows[i].id,0L,0L,0L,0L,0L));
    free(scene);close(drm);puts("DESKTOP_VULKAN_LAYERS_PASS");return 0;
}
