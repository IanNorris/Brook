/* Linux host end-to-end check of brook-desktop-renderer on a real Intel GPU.
 * desktop_renderer.c is compiled in with its Brook system calls routed to a
 * small fake kernel: windows, chrome, wallpaper, a taskbar band and a
 * pointer, the GPU handoff (reserve/observe/present) and output buffers
 * (i915 GEM). A scripted scenario changes the desktop between frames; after
 * every present the fake screen must equal an independent CPU composition
 * (composite.comp semantics) of the state that frame captured. Not run by
 * the nix build (no GPU there).
 *
 * The fake handoff follows the kernel's admission rules
 * (intel_bdw_prepare.cpp, desktop_frame_scenes.h): a plain reservation made
 * while another ticket is outstanding is full-only, a dependent reservation
 * needs the last accepted ticket as its baseline, an accepted present adds
 * its writes to every younger ticket's dependencies, which a partial present
 * must cover, and an older ticket never presents after a newer one. Queued
 * presents (BROOK_RENDER_ASYNC_PRESENT / BROOK_RENDER_PIPELINE=2) complete
 * HOST_E2E_PRESENT_DELAY_US after queueing, checked at every fake call, so
 * the next frame is prepared while the previous one "blits".
 *
 * HOST_E2E_DIRECT=1 adds an in-process provider that offers version 2
 * (retained) GPU sources for windows 2 and 3 over the renderer's socket and
 * poisons each buffer as soon as its SAFE arrives, so a source released
 * while any in-flight frame samples it shows up as a screen mismatch.
 * HOST_E2E_OVERLAY_FEEDBACK=1 models the kernel overlay's compositor cadence
 * graph: a capture after any present re-renders it with new content and a
 * new generation. HOST_E2E_STALE_BASE=1 models the kernel refusing partial
 * presents while a window of the last full frame's scene is gone (closed),
 * until a full frame replaces that base scene. HOST_E2E_MIN_IDLE and
 * HOST_E2E_MAX_FAILURES bound idle polls from below and failed presents
 * from above.
 * HOST_E2E_EXPECT_PIPELINE=1 also requires overlapped submissions, no
 * full frame forced by an in-flight frame and no failed presents.
 * Usage: brook-desktop-renderer-host-test [frames]   (env as for the renderer) */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
long fake_syscall(long number,...);
int fake_open(const char *path,int flags,...);
#define main renderer_main
#define syscall fake_syscall
#define open fake_open
#include "desktop_renderer.c"
#undef main
#undef syscall
#undef open
#include "quad_reference.h"

enum {FW=640,FH=384,BAND=32,CURSOR=16,MAXW=4,TICKETS=8,DEPS=32,POOL=16};
struct FakeTicket {long ticket;int fd,fullOnly,depsFull;uint32_t depCount;struct BrookGpuPresentRegion deps[DEPS];uint32_t *expected;int expectedValid;
    /* GPU sources the frame selected, and whether the renderer has observed
     * its completion (OBSERVE follows the composition fence). */
    unsigned samples,observed;struct {uint32_t window;uint64_t commit;} sampled[8];
    unsigned sceneCount;struct {uint32_t window;uint64_t incarnation;} scene[MAXW];};
/* One provider buffer: a GEM object exported as a DMA-BUF. */
struct FakeBuffer {uint32_t handle;int fd;uint64_t bytes;uint32_t *map;int busy;uint32_t window;uint64_t commit;int presented;};
struct FakeWindow {int live;uint32_t window,w,h,stride,opacity,chromeW,chromeH;uint64_t incarnation,generation;
    int32_t x,y;uint32_t *pixels,*chrome;};
static struct {
    struct FakeWindow win[MAXW];unsigned order[MAXW],count;
    uint32_t backdrop[FW*FH],band[FW*BAND],cursor[64*64];
    uint64_t display,backdropGeneration,overlayGeneration,cursorSerial,nextTicket;
    int32_t cursorX,cursorY;int cursorVisible;
    uint32_t screen[FW*FH],expected[FW*FH];int expectedValid;
    unsigned step,presents,partials,failures;uint64_t presentedPixels,presentedPixelsSmall;
    int intel;struct FakeTicket tickets[TICKETS];
    uint64_t lastPresented;unsigned outstanding,maxOutstanding,overlapReserves,fullOnlyReserves,dependentReserves,staleReserves;
    unsigned fulls,rejected;
    struct {long ticket;int state,full;struct BrookGpuPresentRegions regions;struct BrookGpuPresentFrame frame;uint64_t readyNs;long result;} queue;
    unsigned queued;uint64_t presentDelayNs;
    /* Provider (HOST_E2E_DIRECT). visible: the newest offer of each window
     * the renderer has received by this frame's query. */
    int direct,peer;char socketPath[108];struct FakeBuffer pool[POOL];uint64_t nextCommit;
    struct {uint64_t sent,visible;int sentBuffer,visibleBuffer;uint32_t *pixels[2];} offer[MAXW+1];
    unsigned offers,safes,presentedReplies,badReplies,poisoned,earlySafes;
    int overlayFeedback,staleBase,lateClose;uint64_t overlayRenderedNs;unsigned overlayBumps,baseCount,staleBaseRejects;
    struct {uint32_t window;uint64_t incarnation;} base[MAXW];
} K;
static uint32_t rng=0x2545f491u;
static uint32_t next(void){rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;return rng;}
static uint32_t pick(uint32_t n){return n?next()%n:0;}

/* Title bar 10 rows, 2-pixel borders; transparent interior (all zero). */
static void chrome_draw(struct FakeWindow *w,uint32_t salt){
    for(uint32_t y=0;y<w->chromeH;++y)for(uint32_t x=0;x<w->chromeW;++x){
        const int interior=x>=2 && x<2+w->w && y>=10 && y<10+w->h;
        w->chrome[y*w->chromeW+x]=interior?0:0xff000000u|((x*7+y*13+salt)&0xffffffu);}
}
static void window_open(unsigned slot,uint32_t id,int32_t x,int32_t y,uint32_t width,uint32_t height,int translucent){
    struct FakeWindow *w=&K.win[slot];
    *w=(struct FakeWindow){1,id,width,height,width+(width&1),255,width+4,height+12,(uint64_t)100+id+K.step,1,x,y,NULL,NULL};
    w->pixels=calloc((size_t)w->stride*height,4);w->chrome=calloc((size_t)w->chromeW*w->chromeH,4);
    for(uint32_t i=0;i<w->stride*height;++i)w->pixels[i]=(translucent && (i&3)?(next()&0xff000000u):0xff000000u)|(next()&0xffffffu);
    chrome_draw(w,id);K.order[K.count++]=slot;
}
static void window_close(unsigned slot){
    unsigned n=0;for(unsigned i=0;i<K.count;++i)if(K.order[i]!=slot)K.order[n++]=K.order[i];
    const uint32_t id=K.win[slot].window;
    if(id<=MAXW){K.offer[id].sent=K.offer[id].visible=0;}
    K.count=n;free(K.win[slot].pixels);free(K.win[slot].chrome);K.win[slot].live=0;
}
static struct FakeWindow *window_by_id(uint32_t id){
    for(unsigned i=0;i<K.count;++i)if(K.win[K.order[i]].window==id)return &K.win[K.order[i]];
    return NULL;
}
static void reference(void){
    uint32_t scene[BROOK_VK_SCENE_HEADER+16*BROOK_VK_LAYER_WORDS]={FW,FH,0,0};const uint32_t *src[16];unsigned n=0;
#define LAYER(p,...) do{const struct BrookVkSceneLayer l={__VA_ARGS__};memcpy(scene+BROOK_VK_SCENE_HEADER+n*BROOK_VK_LAYER_WORDS,&l,sizeof(l));src[n++]=(p);}while(0)
    LAYER(K.backdrop,0,FW,FW,FH,0,0,FW,FH,0,0,FW,FH,0,0,FW,FH,255);
    for(unsigned i=0;i<K.count;++i){const struct FakeWindow *w=&K.win[K.order[i]];
        if(w->window<=MAXW && K.offer[w->window].visible)
            LAYER(K.offer[w->window].pixels[1],0,w->w,w->w,w->h,0,0,w->w,w->h,w->x,w->y,w->w,w->h,0,0,FW,FH,
                  w->opacity|BROOK_VK_STRAIGHT_ALPHA|BROOK_VK_OPAQUE_SOURCE);
        else LAYER(w->pixels,0,w->stride,w->w,w->h,0,0,w->w,w->h,w->x,w->y,w->w,w->h,0,0,FW,FH,w->opacity|BROOK_VK_STRAIGHT_ALPHA);
        LAYER(w->chrome,0,w->chromeW,w->chromeW,w->chromeH,0,0,w->chromeW,w->chromeH,w->x-2,w->y-10,w->chromeW,w->chromeH,0,0,FW,FH,
              w->opacity|BROOK_VK_STRAIGHT_ALPHA);}
    LAYER(K.band,0,FW,FW,BAND,0,0,FW,BAND,0,FH-BAND,FW,BAND,0,0,FW,FH,255|BROOK_VK_STRAIGHT_ALPHA);
    if(K.cursorVisible)LAYER(K.cursor,0,64,CURSOR,CURSOR,0,0,CURSOR,CURSOR,K.cursorX,K.cursorY,CURSOR,CURSOR,0,0,FW,FH,255|BROOK_VK_STRAIGHT_ALPHA);
#undef LAYER
    scene[2]=n;
    for(uint32_t y=0;y<FH;++y)for(uint32_t x=0;x<FW;++x)K.expected[y*FW+x]=quad_reference_pixel(scene,src,x,y);
    K.expectedValid=1;
}
/* ---- In-process provider of retained GPU sources (HOST_E2E_DIRECT). ---- */
static uint64_t now_ns(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000ull+(uint64_t)t.tv_nsec;}
static void provider_connect(void){
    if(!K.direct || K.peer>=0)return;
    int fd=socket(AF_UNIX,SOCK_SEQPACKET|SOCK_NONBLOCK|SOCK_CLOEXEC,0);if(fd<0)return;
    struct sockaddr_un addr={.sun_family=AF_UNIX};memcpy(addr.sun_path,K.socketPath,strlen(K.socketPath)+1);
    if(connect(fd,(struct sockaddr *)&addr,sizeof(addr))){close(fd);return;}
    K.peer=fd;
}
static struct FakeBuffer *pool_get(uint64_t bytes){
    for(unsigned i=0;i<POOL;++i){struct FakeBuffer *b=&K.pool[i];if(b->handle && !b->busy && b->bytes==bytes)return b;}
    for(unsigned i=0;i<POOL;++i){struct FakeBuffer *b=&K.pool[i];if(b->handle)continue;
        struct create c={bytes,0,0};if(ioctl(K.intel,IOC(0x5b,sizeof(c)),&c))return NULL;
        struct map_offset m={c.handle,0,0,2,0};if(ioctl(K.intel,IOC(0x64,sizeof(m)),&m))return NULL;
        void *map=mmap(NULL,c.size,PROT_READ|PROT_WRITE,MAP_SHARED,K.intel,(off_t)m.offset);if(map==MAP_FAILED)return NULL;
        struct prime pr={c.handle,O_CLOEXEC|O_RDWR,-1};if(ioctl(K.intel,IOC(0x2d,sizeof(pr)),&pr))return NULL;
        *b=(struct FakeBuffer){c.handle,pr.fd,c.size,map,0,0,0,0};return b;}
    return NULL;
}
struct dma_buf_export_sync_file_wire {uint32_t flags;int32_t fd;};
static void provider_drain(void){
    if(K.peer<0)return;
    for(;;){struct BrookGpuWindowPacket p;int fds[2];const int got=brook_gpu_window_receive(K.peer,&p,fds);
        if(got<=0)return;
        struct FakeBuffer *b=NULL;
        for(unsigned i=0;i<POOL;++i)if(K.pool[i].busy && K.pool[i].window==p.identity.window && K.pool[i].commit==p.identity.commit)b=&K.pool[i];
        if(!b || p.version!=BROOK_GPU_WINDOW_WIRE_VERSION_RETAIN){++K.badReplies;continue;}
        if(p.type==BROOK_GPU_WINDOW_PRESENTED){if(b->presented)++K.badReplies;b->presented=1;++K.presentedReplies;continue;}
        /* SAFE: the renderer promises no remaining GPU use, so no frame that
         * sampled the source may still be composing. Then poison it. */
        for(unsigned i=0;i<TICKETS;++i){const struct FakeTicket *t=&K.tickets[i];if(!t->ticket || t->observed)continue;
            for(unsigned j=0;j<t->samples;++j)if(t->sampled[j].window==p.identity.window && t->sampled[j].commit==p.identity.commit){
                fprintf(stderr,"HOST_E2E_EARLY_SAFE window=%u commit=%llu ticket=%ld\n",p.identity.window,(unsigned long long)p.identity.commit,t->ticket);
                ++K.earlySafes;}}
        for(uint64_t i=0;i<b->bytes/4;++i)b->map[i]=0xffff00ffu;
        b->busy=0;++K.safes;++K.poisoned;
    }
}
static void provider_offer(struct FakeWindow *w){
    if(K.peer<0 || !w || w->window>MAXW)return;
    const uint64_t bytes=((uint64_t)w->w*w->h*4+4095)&~4095ull;
    struct FakeBuffer *b=pool_get(bytes);if(!b)return;
    const uint64_t commit=++K.nextCommit;
    uint32_t *copy=K.offer[w->window].pixels[0];
    for(uint32_t y=0;y<w->h;++y)for(uint32_t x=0;x<w->w;++x){
        const uint32_t v=(uint32_t)(commit*0x9e3779b9u)^(x*0x85ebca6bu)^(y*0xc2b2ae35u);
        b->map[y*w->w+x]=v;copy[y*w->w+x]=v;}
    struct dma_buf_export_sync_file_wire sync={1,-1};
    if(ioctl(b->fd,_IOWR('b',2,struct dma_buf_export_sync_file_wire),&sync) || sync.fd<0){fprintf(stderr,"HOST_E2E_NO_SYNC_FILE\n");exit(2);}
    struct stat st;fstat(b->fd,&st);
    struct BrookGpuWindowPacket p={.magic=BROOK_GPU_WINDOW_WIRE_MAGIC,.version=BROOK_GPU_WINDOW_WIRE_VERSION_RETAIN,.type=BROOK_GPU_WINDOW_OFFER,
        .identity={w->window,0,w->incarnation,commit,(uint64_t)st.st_dev,(uint64_t)st.st_ino},.words=bytes/4,
        .offset=0,.stride=w->w,.width=w->w,.height=w->h,.fourcc=0x34325258};
    const int sent=brook_gpu_window_send_offer(K.peer,&p,b->fd,sync.fd);close(sync.fd);
    if(sent!=1)return;
    *b=(struct FakeBuffer){b->handle,b->fd,b->bytes,b->map,1,w->window,commit,0};
    K.offer[w->window].sent=commit;++K.offers;
}
static void provider_window(uint32_t id,uint32_t w,uint32_t h){
    if(!K.direct)return;
    for(unsigned i=0;i<2;++i){free(K.offer[id].pixels[i]);K.offer[id].pixels[i]=calloc((size_t)w*h,4);}
    K.offer[id].sent=K.offer[id].visible=0;
}
/* One scripted change per frame (applied when the renderer queries). */
static void advance(void){
    const unsigned s=K.step++;
    provider_connect();provider_drain();
    /* Offers sent before this frame's channel poll are now selectable. */
    for(unsigned id=1;id<=MAXW;++id)if(K.offer[id].sent!=K.offer[id].visible && K.offer[id].pixels[0]){
        struct FakeWindow *w=window_by_id(id);
        if(w)memcpy(K.offer[id].pixels[1],K.offer[id].pixels[0],(size_t)w->w*w->h*4);
        K.offer[id].visible=K.offer[id].sent;}
    struct FakeWindow *a=window_by_id(1),*b=window_by_id(2);
    switch(s){
    case 0:window_open(0,1,40,40,300,180,1);window_open(1,2,260,120,200,140,0);provider_window(2,200,140);
        K.cursorVisible=1;K.cursorX=100;K.cursorY=100;break;
    case 1:break;                                                    /* nothing: keepalive or idle */
    case 2:K.cursorX=420;K.cursorY=200;++K.cursorSerial;break;
    case 3:for(uint32_t x=0;x<a->w;++x)a->pixels[77*a->stride+x]^=0x00ff00ffu;++a->generation;break;
    case 4:b->x=300;b->y=90;break;
    case 5:a->opacity=128;break;
    case 6:K.order[0]=1;K.order[1]=0;break;                          /* restack: window 1 above 2 */
    case 7:for(uint32_t i=0;i<FW*8;++i)K.band[i]^=0x00102030u;++K.overlayGeneration;break;
    case 8:chrome_draw(a,99);break;                                  /* title/focus change */
    case 9:window_close(1);break;
    case 10:a->opacity=255;window_open(2,3,120,200,150,90,0);provider_window(3,150,90);break;
    case 11:for(uint32_t i=0;i<FW*FH;i+=7)K.backdrop[i]^=0x00010101u;++K.backdropGeneration;break;
    case 12:break;
    case 13:K.cursorVisible=0;break;
    case 14:K.cursorVisible=1;K.cursorX=FW-8;K.cursorY=FH-8;break;   /* partly off screen */
    default:if(s<40)break;                                           /* a quiet stretch (after a GPU burst): idle polls, then a keepalive */
    {
        struct FakeWindow *w=K.count?&K.win[K.order[pick(K.count)]]:NULL;
        switch(pick(8)){
        case 0:K.cursorX=(int32_t)pick(FW)-4;K.cursorY=(int32_t)pick(FH)-4;break;
        case 1:if(w){uint32_t r=pick(w->h),n=1+pick(20);
                   for(uint32_t y=r;y<r+n && y<w->h;++y){for(uint32_t x=0;x<w->w;++x)w->pixels[y*w->stride+x]=next();}
                   ++w->generation;}break;
        case 2:if(w){w->x+=(int32_t)pick(41)-20;w->y+=(int32_t)pick(41)-20;}break;
        case 3:if(w)w->opacity=pick(3)?255:pick(256);break;
        case 4:if(w)chrome_draw(w,next());break;
        case 5:if(K.count>1){unsigned t=K.order[0];K.order[0]=K.order[K.count-1];K.order[K.count-1]=t;}break;
        case 6:for(uint32_t i=pick(FW*BAND);i<FW*BAND;i+=5)K.band[i]=next()|0xff000000u;++K.overlayGeneration;break;
        default:break;
        }}
    }
    /* HOST_E2E_LATE_CLOSE: close the GPU window 3 mid-run and open a new
     * GPU window 4 that commits every frame (a game restarted). */
    if(K.lateClose && s==60){for(unsigned i=0;i<K.count;++i)if(K.win[K.order[i]].window==3){window_close(K.order[i]);break;}}
    if(K.lateClose && s==61 && !K.win[1].live){window_open(1,4,200,150,180,120,0);provider_window(4,180,120);}
    /* GPU commits: window 2 until it closes, then a burst and random
     * commits of window 3 (retained sources survive moves and restacks). */
    if(K.direct){
        if(s>=1 && s<=8 && (s&1 || s==4))provider_offer(window_by_id(2));
        if((s>=12 && s<15) || (s>=16 && s<=24) || (s>=40 && pick(3)==0))provider_offer(window_by_id(3));
        if(K.lateClose && s>61)provider_offer(window_by_id(4));
    }
    reference();
}
/* The screen must equal the reference of the state the presented ticket's
 * frame captured (with two frames in flight, not the latest state). */
static void check_screen(const char *how,const struct FakeTicket *t){
    ++K.presents;
    if(!t->expectedValid)return;
    for(uint32_t i=0;i<FW*FH;++i)if(K.screen[i]!=t->expected[i]){
        fprintf(stderr,"HOST_E2E_MISMATCH step=%u present=%u ticket=%ld %s x=%u y=%u screen=%08x expected=%08x\n",K.step-1,K.presents,t->ticket,how,
            i%FW,i/FW,K.screen[i],t->expected[i]);++K.failures;
        if(K.failures>5)exit(1);
        return;
    }
}
static uint32_t *map_fd(int fd,uint64_t *bytes){
    off_t size=lseek(fd,0,SEEK_END);lseek(fd,0,SEEK_SET);*bytes=(uint64_t)size;
    void *p=mmap(NULL,(size_t)size,PROT_READ,MAP_SHARED,fd,0);return p==MAP_FAILED?NULL:p;
}
struct dma_buf_sync_wire {uint64_t flags;};
static void dma_sync(int fd,uint64_t flags){struct dma_buf_sync_wire s={flags};(void)ioctl(fd,_IOW('b',0,struct dma_buf_sync_wire),&s);}
static struct FakeTicket *ticket_find(long ticket){for(unsigned i=0;i<TICKETS;++i)if(ticket>0 && K.tickets[i].ticket==ticket)return &K.tickets[i];return NULL;}

static long layers_call(long op,struct BrookDesktopLayers *L,long a){
    if(op==BROOK_DESKTOP_LAYERS_QUERY){
        advance();L->count=K.count;
        for(unsigned i=0;i<K.count;++i){const struct FakeWindow *w=&K.win[K.order[i]];
            L->layers[i]=(struct BrookDesktopLayer){0,0,w->incarnation,w->generation,(uint64_t)w->stride*w->h*4,w->window,w->w,w->h,w->stride,
                w->w,w->h,w->x,w->y,1,w->opacity==255?0:(BROOK_DESKTOP_LAYER_HAS_OPACITY|(w->opacity<<BROOK_DESKTOP_LAYER_OPACITY_SHIFT))};}
        return 0;
    }
    if(op==BROOK_DESKTOP_LAYERS_CAPTURE_TRACKED || op==BROOK_DESKTOP_LAYERS_CAPTURE_TRACKED_SELECTIVE){
        if(L->count!=K.count)return errno=EAGAIN,-1;
        for(unsigned i=0;i<K.count;++i){const struct FakeWindow *w=&K.win[K.order[i]];
            if(op==BROOK_DESKTOP_LAYERS_CAPTURE_TRACKED_SELECTIVE && ((uint64_t)a&(1u<<i)))continue;
            const uint64_t bytes=(uint64_t)w->stride*w->h*4;
            if(L->layers[i].window!=w->window || !L->layers[i].pixels || L->layers[i].capacity<bytes)return errno=EINVAL,-1;
            memcpy((void*)(uintptr_t)L->layers[i].pixels,w->pixels,bytes);}
        return 0;
    }
    if(op==BROOK_DESKTOP_BACKDROP_QUERY || op==BROOK_DESKTOP_BACKDROP_TRACKED_CAPTURE){
        if(op==BROOK_DESKTOP_BACKDROP_TRACKED_CAPTURE){
            if(!L->layers[0].pixels || L->layers[0].capacity<sizeof(K.backdrop) || L->layers[0].generation!=K.backdropGeneration)return errno=EAGAIN,-1;
            memcpy((void*)(uintptr_t)L->layers[0].pixels,K.backdrop,sizeof(K.backdrop));}
        L->count=1;L->layers[0]=(struct BrookDesktopLayer){L->layers[0].pixels,L->layers[0].capacity,K.display,K.backdropGeneration,
            sizeof(K.backdrop),0,FW,FH,FW,FW,FH,0,0,1,0};
        return 0;
    }
    if(op==BROOK_DESKTOP_CHROME_QUERY || op==BROOK_DESKTOP_CHROME_TRACKED_CAPTURE){
        const struct FakeWindow *w=window_by_id(L->layers[0].window);if(!w)return errno=ENOENT,-1;
        const uint64_t bytes=(uint64_t)w->chromeW*w->chromeH*4;
        if(op==BROOK_DESKTOP_CHROME_TRACKED_CAPTURE){
            if(!L->layers[0].pixels || L->layers[0].capacity<bytes)return errno=EINVAL,-1;
            memcpy((void*)(uintptr_t)L->layers[0].pixels,w->chrome,bytes);}
        L->count=1;L->layers[0]=(struct BrookDesktopLayer){L->layers[0].pixels,L->layers[0].capacity,w->incarnation,w->generation,bytes,
            w->window,w->chromeW,w->chromeH,w->chromeW,w->chromeW,w->chromeH,w->x-2,w->y-10,1,0};
        return 0;
    }
    return errno=ENOSYS,-1;
}
static long overlay_call(long op,long token,void *p,long size,long a){
    (void)token;(void)size;
    if(op==BROOK_DESKTOP_OVERLAY_GENERATION)return (long)K.overlayGeneration;
    if(op==0){
        /* The capture renders the overlay; its compositor cadence graph
         * moves with time (the kernel samples every compositor tick). */
        if(K.overlayFeedback && now_ns()-K.overlayRenderedNs>=4000000ull){
            K.overlayRenderedNs=now_ns();
            for(uint32_t x=FW-40;x<FW;++x)K.band[2*FW+x]=x-(FW-40)==K.overlayBumps%40?0xff68acffu:0xff202020u;
            ++K.overlayGeneration;++K.overlayBumps;reference();
        }
        return 1;}
    if(op==3)return 0;
    if(op==BROOK_DESKTOP_OVERLAY_WAIT){struct BrookDesktopOverlayInfo *info=p;
        *info=(struct BrookDesktopOverlayInfo){{1,2,FW,FH,FW,1,(uint64_t)FW*FH*4,0,0},K.overlayGeneration};return 0;}
    if(op==4){struct BrookDesktopOverlayBands *b=p;memset(b,0,sizeof(*b));b->count=1;b->bands[0]=(struct BrookDesktopOverlayBand){0,FH-BAND,FW,BAND};return 0;}
    if(op==BROOK_DESKTOP_OVERLAY_READ_BAND){if(a!=0 || size!=(long)sizeof(K.band))return errno=EINVAL,-1;memcpy(p,K.band,sizeof(K.band));return size;}
    if(op==BROOK_DESKTOP_OVERLAY_CURSOR){struct BrookDesktopOverlayCursor *c=p;
        c->visible=K.cursorVisible;c->width=CURSOR;c->height=CURSOR;c->stride=64;c->x=K.cursorX;c->y=K.cursorY;c->serial=K.cursorSerial+1;
        if((uint64_t)a!=c->serial)memcpy(c->pixels,K.cursor,sizeof(K.cursor));
        return 0;}
    return errno=EINVAL,-1;
}
static void ticket_free(struct FakeTicket *t){if(t && t->ticket){t->ticket=0;--K.outstanding;}}
/* Every pixel of every dependency must lie in one of the writes. */
static int deps_covered(const struct FakeTicket *t,const struct BrookGpuPresentRegions *r){
    if(t->depsFull)return 0;
    if(!t->depCount)return 1;
    static uint8_t mask[FW*FH];memset(mask,0,sizeof(mask));
    for(uint32_t i=0;i<r->count;++i){const struct BrookGpuPresentRegion *g=&r->regions[i];
        for(uint32_t y=g->y;y<g->y+g->height && y<FH;++y)for(uint32_t x=g->x;x<g->x+g->width && x<FW;++x)mask[y*FW+x]=1;}
    for(uint32_t i=0;i<t->depCount;++i){const struct BrookGpuPresentRegion *g=&t->deps[i];
        for(uint32_t y=g->y;y<g->y+g->height;++y)for(uint32_t x=g->x;x<g->x+g->width;++x)if(!mask[y*FW+x])return 0;}
    return 1;
}
/* Kernel admission and the copy to the fake scanout. regions==NULL: full. */
static long present_now(long ticket,const struct BrookGpuPresentRegions *r,const struct BrookGpuPresentFrame *f){
    struct FakeTicket *t=ticket_find(ticket);if(!t)return -EINVAL;
    if((uint64_t)ticket<=K.lastPresented){++K.rejected;return -ESTALE;}
    if(r && (t->fullOnly || !deps_covered(t,r))){++K.rejected;return -ESTALE;}
    if(r && K.staleBase)for(unsigned i=0;i<K.baseCount;++i){const struct FakeWindow *w=window_by_id(K.base[i].window);
        if(!w || w->incarnation!=K.base[i].incarnation){++K.rejected;++K.staleBaseRejects;return -ESTALE;}}
    if(!r){K.baseCount=t->sceneCount;memcpy(K.base,t->scene,sizeof(K.base));}
    uint64_t bytes=0;uint32_t *m=map_fd(t->fd,&bytes);if(!m)return -EINVAL;
    dma_sync(t->fd,1);uint64_t pixels=0;
    if(r)for(uint32_t i=0;i<r->count;++i){const struct BrookGpuPresentRegion *g=&r->regions[i];
        if(g->offset&63u){fprintf(stderr,"HOST_E2E_UNALIGNED offset=%llu\n",(unsigned long long)g->offset);exit(1);}
        for(uint32_t y=0;y<g->height;++y)memcpy(K.screen+(uint64_t)(g->y+y)*FW+g->x,(const uint8_t*)m+g->offset+(uint64_t)y*g->stride*4,(size_t)g->width*4);
        pixels+=(uint64_t)g->width*g->height;}
    else {for(uint32_t y=0;y<FH;++y)memcpy(K.screen+y*FW,m+(uint64_t)y*f->stride,FW*4);pixels=FW*FH;++K.fulls;}
    dma_sync(t->fd,1|4);munmap(m,bytes);K.presentedPixels+=pixels;
    if(r){++K.partials;if(pixels<=4096)++K.presentedPixelsSmall;}
    K.lastPresented=(uint64_t)ticket;
    /* RecordAccepted: younger tickets must cover these writes. */
    for(unsigned i=0;i<TICKETS;++i){struct FakeTicket *o=&K.tickets[i];if(!o->ticket || o->ticket<=ticket)continue;
        if(!r || o->depCount+r->count>DEPS){o->depsFull=1;continue;}
        for(uint32_t j=0;j<r->count;++j)o->deps[o->depCount++]=r->regions[j];}
    check_screen(r?"partial":"full",t);ticket_free(t);return 0;
}
/* A queued present completes (on the fake "worker") once its delay passed. */
static void queue_progress(void){
    if(K.queue.state!=1 || now_ns()<K.queue.readyNs)return;
    K.queue.result=present_now(K.queue.ticket,K.queue.full?NULL:&K.queue.regions,&K.queue.frame);K.queue.state=2;
}
static long fail(long e){errno=(int)-e;return -1;}
static long handoff_call(long op,long arg,long a){
    switch(op){
    case BROOK_GPU_HANDOFF_ATTACH:case BROOK_GPU_HANDOFF_DETACH:return 0;
    case BROOK_GPU_HANDOFF_PRESENT_CAPS:return BROOK_GPU_PRESENT_CAP_FRAME|BROOK_GPU_PRESENT_CAP_DEPENDENT|BROOK_GPU_PRESENT_CAP_ASYNC;
    case BROOK_GPU_HANDOFF_RESERVE:case BROOK_GPU_HANDOFF_RESERVE_DEPENDENT:{
        const int dependent=op==BROOK_GPU_HANDOFF_RESERVE_DEPENDENT;
        if(dependent && (!a || (uint64_t)a!=K.lastPresented)){++K.staleReserves;return fail(-ESTALE);}
        for(unsigned i=0;i<TICKETS;++i)if(!K.tickets[i].ticket){struct FakeTicket *t=&K.tickets[i];
            if(K.outstanding)++K.overlapReserves;
            uint32_t *expected=t->expected?t->expected:malloc(sizeof(K.expected));
            *t=(struct FakeTicket){.ticket=(long)++K.nextTicket,.fd=(int)arg,.fullOnly=!dependent && K.outstanding>0,.expected=expected,.expectedValid=K.expectedValid};
            memcpy(t->expected,K.expected,sizeof(K.expected));
            if(t->fullOnly)++K.fullOnlyReserves;
            for(unsigned j=0;j<K.count && j<MAXW;++j){t->scene[j].window=K.win[K.order[j]].window;t->scene[j].incarnation=K.win[K.order[j]].incarnation;}
            t->sceneCount=K.count<MAXW?K.count:MAXW;
            /* The frame being prepared is the channel's newest frame. */
            for(unsigned j=0;j<DESKTOP_GPU_OFFERS && t->samples<8;++j){const struct DesktopGpuOffer *o=&g_gpuChannel.offers[j];
                if(o->state==2 && desktop_gpu_offer_in_frame(o,g_gpuChannel.nextFrame)){
                    t->sampled[t->samples].window=o->packet.identity.window;t->sampled[t->samples++].commit=o->packet.identity.commit;}}
            if(dependent)++K.dependentReserves;
            if(++K.outstanding>K.maxOutstanding)K.maxOutstanding=K.outstanding;
            return t->ticket;}
        return fail(-EBUSY);}
    case BROOK_GPU_HANDOFF_OBSERVE:{struct FakeTicket *t=ticket_find(arg);if(!t)return fail(-EINVAL);t->observed=1;return 1;}
    case BROOK_GPU_HANDOFF_RELEASE:
        if(K.queue.state==1 && K.queue.ticket==arg)return fail(-EBUSY);
        ticket_free(ticket_find(arg));return 0;
    case BROOK_GPU_HANDOFF_PRESENT_FRAME:{const long r=present_now((long)((const struct BrookGpuPresentFrame*)arg)->ticket,NULL,(const void*)arg);
        return r?fail(r):0;}
    case BROOK_GPU_HANDOFF_PRESENT_REGIONS:{const struct BrookGpuPresentRegions *g=(const void*)arg;const long r=present_now((long)g->ticket,g,NULL);
        return r?fail(r):0;}
    case BROOK_GPU_HANDOFF_QUEUE_FRAME:case BROOK_GPU_HANDOFF_QUEUE_REGIONS:{
        if(K.queue.state)return fail(-EBUSY);
        const int full=op==BROOK_GPU_HANDOFF_QUEUE_FRAME;
        const long ticket=full?(long)((const struct BrookGpuPresentFrame*)arg)->ticket:(long)((const struct BrookGpuPresentRegions*)arg)->ticket;
        struct FakeTicket *t=ticket_find(ticket);
        if(!t || (!full && t->fullOnly))return fail(-ESTALE);
        K.queue.ticket=ticket;K.queue.full=full;K.queue.state=1;K.queue.readyNs=now_ns()+K.presentDelayNs;++K.queued;
        if(full)K.queue.frame=*(const struct BrookGpuPresentFrame*)arg;else K.queue.regions=*(const struct BrookGpuPresentRegions*)arg;
        return 0;}
    case BROOK_GPU_HANDOFF_PRESENT_RESULT:
        if(!K.queue.state || K.queue.ticket!=arg)return fail(-ENOENT);
        if(K.queue.state!=2)return 0;
        K.queue.state=0;{const long r=K.queue.result;if(r)ticket_free(ticket_find(arg));return 1-r;}
    default:return fail(-ENOSYS);
    }
}
long fake_syscall(long number,...){
    va_list ap;va_start(ap,number);long a[6];for(int i=0;i<6;++i)a[i]=va_arg(ap,long);va_end(ap);
    queue_progress();
    switch(number){
    case BROOK_SYS_GPU_WINDOW_VALIDATE:{const struct FakeWindow *w=window_by_id((uint32_t)a[1]);
        return w && w->incarnation==(uint64_t)a[2]?0:fail(-ESRCH);}
    case BROOK_SYS_WM_QUERY_SOURCE_COMMIT:if(a[3])*(uint64_t*)a[3]=0;return 0;
    case BROOK_SYS_DESKTOP_LAYERS:return layers_call(a[0],(struct BrookDesktopLayers*)a[1],a[3]);
    case BROOK_SYS_DESKTOP_OVERLAY:return overlay_call(a[0],a[1],(void*)a[2],a[3],a[4]);
    case BROOK_SYS_GPU_HANDOFF_CTL:return handoff_call(a[0],a[1],a[2]);
    case BROOK_SYS_GPU_OUTPUT_ALLOC:{
        struct create c={(uint64_t)a[0],0,0};if(ioctl(K.intel,IOC(0x5b,sizeof(c)),&c))return -1;
        struct prime p={c.handle,O_CLOEXEC|O_RDWR,-1};if(ioctl(K.intel,IOC(0x2d,sizeof(p)),&p))return -1;
        return p.fd;}
    default:return errno=ENOSYS,-1;
    }
}
int fake_open(const char *path,int flags,...){
    va_list ap;va_start(ap,flags);int mode=va_arg(ap,int);va_end(ap);
    if(!strcmp(path,"/dev/dri/renderD129"))return dup(K.intel);
    return open(path,flags,mode);
}
int main(int argc,char **argv){
    const char *frames=argc>1?argv[1]:"60";
    for(int n=128;n<136 && !K.intel;++n){char path[32];snprintf(path,sizeof(path),"/dev/dri/renderD%d",n);
        int fd=open(path,O_RDWR|O_CLOEXEC);if(fd<0)continue;
        char name[32]={0};struct {int a,b,c;size_t nameLen;char *name;size_t dl;char *d;size_t el;char *e;} v={0,0,0,sizeof(name)-1,name,0,0,0,0};
        if(!ioctl(fd,IOC(0x00,sizeof(v)),&v) && !strcmp(name,"i915"))K.intel=fd;else close(fd);}
    if(!K.intel){fprintf(stderr,"HOST_E2E_NO_INTEL\n");return 2;}
    K.display=7;K.backdropGeneration=3;K.overlayGeneration=1;
    for(uint32_t i=0;i<FW*FH;++i)K.backdrop[i]=0xff000000u|(i*2654435761u>>8);
    for(uint32_t i=0;i<FW*BAND;++i)K.band[i]=(i%3?0xc0000000u:0xff000000u)|(i*40503u&0xffffffu);
    for(uint32_t i=0;i<64*64;++i)K.cursor[i]=(i&1)?0x80ffffffu:0xff102030u;
    const char *delay=getenv("HOST_E2E_PRESENT_DELAY_US");K.presentDelayNs=delay?strtoull(delay,NULL,10)*1000ull:2000000ull;
    K.overlayFeedback=getenv("HOST_E2E_OVERLAY_FEEDBACK")!=NULL;K.staleBase=getenv("HOST_E2E_STALE_BASE")!=NULL;
    K.lateClose=getenv("HOST_E2E_LATE_CLOSE")!=NULL;
    K.peer=-1;K.direct=getenv("HOST_E2E_DIRECT") && strcmp(getenv("HOST_E2E_DIRECT"),"0");
    if(K.direct){snprintf(K.socketPath,sizeof(K.socketPath),"/tmp/brook-host-e2e-%d.sock",(int)getpid());
        setenv("BROOK_DESKTOP_GPU_SOCKET",K.socketPath,1);}
    char *args[]={"brook-desktop-renderer","--frames",(char*)frames,NULL};
    const int result=renderer_main(3,args);
    provider_drain();
    printf("HOST_E2E steps=%u presents=%u partial=%u full=%u failures=%u presented_px=%llu small_presents=%u renderer=%d\n",K.step,K.presents,
        K.partials,K.fulls,K.failures,(unsigned long long)K.presentedPixels,(unsigned)K.presentedPixelsSmall,result);
    printf("HOST_E2E_HANDOFF max_outstanding=%u overlapped_reserves=%u dependent=%u full_only=%u stale_reserves=%u rejected_presents=%u queued=%u\n",
        K.maxOutstanding,K.overlapReserves,K.dependentReserves,K.fullOnlyReserves,K.staleReserves,K.rejected,K.queued);
    unsigned bad=0;
    if(K.direct){
        /* Window 2 closed at step 9: every one of its sources must be released. */
        unsigned held=0;for(unsigned i=0;i<POOL;++i)if(K.pool[i].busy && K.pool[i].window==2)++held;
        printf("HOST_E2E_DIRECT offers=%u safes=%u presented=%u bad_replies=%u early_safes=%u closed_window_held=%u direct_layers=%llu\n",
            K.offers,K.safes,K.presentedReplies,K.badReplies,K.earlySafes,held,(unsigned long long)g_gpuDirectLayers);
        if(K.badReplies || K.earlySafes || held || !g_gpuDirectLayers || !K.safes || !K.presentedReplies)++bad;
    }
    printf("HOST_E2E_STATS pipeline=%u submitted_in_flight=%llu/%llu carried=%llu rebased=%llu wait_full=%llu full_forced=%llu forced_in_flight=%llu present_failures=%llu\n",
        g_pipeline,(unsigned long long)g_stats.pipeline.submitted[0],(unsigned long long)g_stats.pipeline.submitted[1],
        (unsigned long long)g_stats.pipeline.carried,(unsigned long long)g_stats.pipeline.rebased,(unsigned long long)g_stats.pipeline.waitFull,
        (unsigned long long)g_stats.fullForced.total,(unsigned long long)g_stats.fullForced.inFlight,(unsigned long long)g_stats.presentFailures);
    if(getenv("HOST_E2E_EXPECT_PIPELINE")){
        /* Overlap really happened, nothing was forced full or refused because
         * a frame was in flight, and the kernel never rejected a present. */
        if(g_pipeline!=2 || K.maxOutstanding<2 || !K.overlapReserves || !g_stats.pipeline.submitted[1] || !g_stats.pipeline.carried ||
           g_stats.fullForced.inFlight || K.fullOnlyReserves>K.fulls || (!K.staleBase && (K.rejected || g_stats.presentFailures)))++bad;
        /* Full frames only where no partial baseline exists: the first frame
         * and the backdrop change. */
        if(K.fulls>(K.staleBase?3u:2u))++bad;
    }
    printf("HOST_E2E_IDLE idle_polls=%llu overlay_bumps=%u overlay_captures=%llu stale_base_rejects=%u present_failures=%llu\n",
        (unsigned long long)g_stats.idleSkips,K.overlayBumps,(unsigned long long)g_stats.overlayCaptures,K.staleBaseRejects,(unsigned long long)g_stats.presentFailures);
    if(K.lateClose){uint64_t shown=0;
        for(unsigned i=0;i<DESKTOP_STATS_WINDOWS;++i)if(g_stats.windows[i].lastUse && g_stats.windows[i].window==4 && g_stats.windows[i].lastGpu)
            shown=g_stats.windows[i].displayed;
        printf("HOST_E2E_LATE_WINDOW gpu_displayed=%llu\n",(unsigned long long)shown);
        if(shown<5)++bad;}
    if(getenv("HOST_E2E_MIN_IDLE") && g_stats.idleSkips<strtoull(getenv("HOST_E2E_MIN_IDLE"),NULL,10))++bad;
    if(getenv("HOST_E2E_MAX_FAILURES") && g_stats.presentFailures>strtoull(getenv("HOST_E2E_MAX_FAILURES"),NULL,10))++bad;
    if(bad)printf("HOST_E2E_EXPECTATION_FAILED\n");
    else if(!K.failures && !result)printf("HOST_E2E_PASS\n");
    if(K.direct)unlink(K.socketPath);
    return result || K.failures || bad?1:0;
}
