/* Linux host end-to-end check of brook-desktop-renderer on a real Intel GPU.
 * desktop_renderer.c is compiled in with its Brook system calls routed to a
 * small fake kernel: windows, chrome, wallpaper, a taskbar band and a
 * pointer, the GPU handoff (reserve/observe/present) and output buffers
 * (i915 GEM). A scripted scenario changes the desktop between frames; after
 * every present the fake screen must equal an independent CPU composition
 * (composite.comp semantics) of the state that frame captured. Not run by
 * the nix build (no GPU there).
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

enum {FW=640,FH=384,BAND=32,CURSOR=16,MAXW=4};
struct FakeWindow {int live;uint32_t window,w,h,stride,opacity,chromeW,chromeH;uint64_t incarnation,generation;
    int32_t x,y;uint32_t *pixels,*chrome;};
static struct {
    struct FakeWindow win[MAXW];unsigned order[MAXW],count;
    uint32_t backdrop[FW*FH],band[FW*BAND],cursor[64*64];
    uint64_t display,backdropGeneration,overlayGeneration,cursorSerial,nextTicket;
    int32_t cursorX,cursorY;int cursorVisible;
    uint32_t screen[FW*FH],expected[FW*FH];int expectedValid;
    unsigned step,presents,partials,failures;uint64_t presentedPixels,presentedPixelsSmall;
    int intel;long tickets[8][2];
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
        LAYER(w->pixels,0,w->stride,w->w,w->h,0,0,w->w,w->h,w->x,w->y,w->w,w->h,0,0,FW,FH,w->opacity|BROOK_VK_STRAIGHT_ALPHA);
        LAYER(w->chrome,0,w->chromeW,w->chromeW,w->chromeH,0,0,w->chromeW,w->chromeH,w->x-2,w->y-10,w->chromeW,w->chromeH,0,0,FW,FH,
              w->opacity|BROOK_VK_STRAIGHT_ALPHA);}
    LAYER(K.band,0,FW,FW,BAND,0,0,FW,BAND,0,FH-BAND,FW,BAND,0,0,FW,FH,255|BROOK_VK_STRAIGHT_ALPHA);
    if(K.cursorVisible)LAYER(K.cursor,0,64,CURSOR,CURSOR,0,0,CURSOR,CURSOR,K.cursorX,K.cursorY,CURSOR,CURSOR,0,0,FW,FH,255|BROOK_VK_STRAIGHT_ALPHA);
#undef LAYER
    scene[2]=n;
    for(uint32_t y=0;y<FH;++y)for(uint32_t x=0;x<FW;++x)K.expected[y*FW+x]=quad_reference_pixel(scene,src,x,y);
    K.expectedValid=1;
}
/* One scripted change per frame (applied when the renderer queries). */
static void advance(void){
    const unsigned s=K.step++;
    struct FakeWindow *a=window_by_id(1),*b=window_by_id(2);
    switch(s){
    case 0:window_open(0,1,40,40,300,180,1);window_open(1,2,260,120,200,140,0);K.cursorVisible=1;K.cursorX=100;K.cursorY=100;break;
    case 1:break;                                                    /* nothing: keepalive or idle */
    case 2:K.cursorX=420;K.cursorY=200;++K.cursorSerial;break;
    case 3:for(uint32_t x=0;x<a->w;++x)a->pixels[77*a->stride+x]^=0x00ff00ffu;++a->generation;break;
    case 4:b->x=300;b->y=90;break;
    case 5:a->opacity=128;break;
    case 6:K.order[0]=1;K.order[1]=0;break;                          /* restack: window 1 above 2 */
    case 7:for(uint32_t i=0;i<FW*8;++i)K.band[i]^=0x00102030u;++K.overlayGeneration;break;
    case 8:chrome_draw(a,99);break;                                  /* title/focus change */
    case 9:window_close(1);break;
    case 10:a->opacity=255;window_open(2,3,120,200,150,90,0);break;
    case 11:for(uint32_t i=0;i<FW*FH;i+=7)K.backdrop[i]^=0x00010101u;++K.backdropGeneration;break;
    case 12:break;
    case 13:K.cursorVisible=0;break;
    case 14:K.cursorVisible=1;K.cursorX=FW-8;K.cursorY=FH-8;break;   /* partly off screen */
    default:{
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
    reference();
}
static void check_screen(const char *how){
    ++K.presents;
    if(!K.expectedValid)return;
    for(uint32_t i=0;i<FW*FH;++i)if(K.screen[i]!=K.expected[i]){
        fprintf(stderr,"HOST_E2E_MISMATCH step=%u present=%u %s x=%u y=%u screen=%08x expected=%08x\n",K.step-1,K.presents,how,
            i%FW,i/FW,K.screen[i],K.expected[i]);++K.failures;
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
static int ticket_fd(long ticket){for(unsigned i=0;i<8;++i)if(K.tickets[i][0]==ticket)return (int)K.tickets[i][1];return -1;}

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
    if(op==0)return 1;
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
static long handoff_call(long op,long arg,long a){
    switch(op){
    case BROOK_GPU_HANDOFF_ATTACH:case BROOK_GPU_HANDOFF_DETACH:return 0;
    case BROOK_GPU_HANDOFF_PRESENT_CAPS:return BROOK_GPU_PRESENT_CAP_FRAME|BROOK_GPU_PRESENT_CAP_DEPENDENT;
    case BROOK_GPU_HANDOFF_RESERVE:case BROOK_GPU_HANDOFF_RESERVE_DEPENDENT:{
        (void)a;const long ticket=(long)++K.nextTicket;
        for(unsigned i=0;i<8;++i)if(!K.tickets[i][0]){K.tickets[i][0]=ticket;K.tickets[i][1]=arg;return ticket;}
        return errno=EBUSY,-1;}
    case BROOK_GPU_HANDOFF_OBSERVE:return 1;
    case BROOK_GPU_HANDOFF_RELEASE:for(unsigned i=0;i<8;++i)if(K.tickets[i][0]==arg)K.tickets[i][0]=0;return 0;
    case BROOK_GPU_HANDOFF_PRESENT_FRAME:{const struct BrookGpuPresentFrame *f=(const void*)arg;
        int fd=ticket_fd((long)f->ticket);uint64_t bytes=0;uint32_t *m=fd>=0?map_fd(fd,&bytes):NULL;if(!m)return errno=EINVAL,-1;
        dma_sync(fd,1);for(uint32_t y=0;y<FH;++y)memcpy(K.screen+y*FW,m+(uint64_t)y*f->stride,FW*4);dma_sync(fd,1|4);
        munmap(m,bytes);K.presentedPixels+=FW*FH;handoff_call(BROOK_GPU_HANDOFF_RELEASE,(long)f->ticket,0);check_screen("full");return 0;}
    case BROOK_GPU_HANDOFF_PRESENT_REGIONS:{const struct BrookGpuPresentRegions *r=(const void*)arg;
        int fd=ticket_fd((long)r->ticket);uint64_t bytes=0;uint32_t *m=fd>=0?map_fd(fd,&bytes):NULL;if(!m)return errno=EINVAL,-1;
        dma_sync(fd,1);uint64_t pixels=0;
        for(uint32_t i=0;i<r->count;++i){const struct BrookGpuPresentRegion *g=&r->regions[i];
            if(g->offset&63u){fprintf(stderr,"HOST_E2E_UNALIGNED offset=%llu\n",(unsigned long long)g->offset);exit(1);}
            for(uint32_t y=0;y<g->height;++y)memcpy(K.screen+(uint64_t)(g->y+y)*FW+g->x,(const uint8_t*)m+g->offset+(uint64_t)y*g->stride*4,(size_t)g->width*4);
            pixels+=(uint64_t)g->width*g->height;}
        dma_sync(fd,1|4);munmap(m,bytes);K.presentedPixels+=pixels;++K.partials;if(pixels<=4096)++K.presentedPixelsSmall;
        handoff_call(BROOK_GPU_HANDOFF_RELEASE,(long)r->ticket,0);check_screen("partial");return 0;}
    default:return errno=ENOSYS,-1;
    }
}
long fake_syscall(long number,...){
    va_list ap;va_start(ap,number);long a[6];for(int i=0;i<6;++i)a[i]=va_arg(ap,long);va_end(ap);
    switch(number){
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
    char *args[]={"brook-desktop-renderer","--frames",(char*)frames,NULL};
    const int result=renderer_main(3,args);
    printf("HOST_E2E steps=%u presents=%u partial=%u failures=%u presented_px=%llu small_presents=%u renderer=%d\n",K.step,K.presents,
        K.partials,K.failures,(unsigned long long)K.presentedPixels,(unsigned)K.presentedPixelsSmall,result);
    return result || K.failures?1:0;
}
