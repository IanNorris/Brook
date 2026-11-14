#define _GNU_SOURCE
#include "desktop_layers_abi.h"
#include "gpu_output_abi.h"
#include "brook_debug_frame.h"
#include "renderer.h"
#include "graphics_pass.h"
#include "quad_pass.h"
#include "quad_plan.h"
#include "desktop_track.h"
#include "desktop_fence_recovery.h"
#include "desktop_blocked_recovery.h"
#include "scene.h"
#include "backdrop_cache.h"
#include "desktop_content_layer.h"
#include "desktop_chrome_spans.h"
#include "desktop_damage.h"
#include "desktop_graphics_damage.h"
#include "desktop_retry.h"
#include "desktop_tile.h"
#include "overlay_copy.h"
#include "desktop_overlay_cache.h"
#include "source_size_policy.h"
#include "desktop_gpu_channel.h"
#include "desktop_async_source_barrier.h"
#include "desktop_stats.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <signal.h>
#include <poll.h>
#include <unistd.h>

#define IOC(n,s) ((3u<<30)|((unsigned)(s)<<16)|('d'<<8)|(n))
enum { SCENE_HEADER=2048 };
struct create {uint64_t size;uint32_t handle,pad;};
struct map_offset {uint32_t handle,pad;uint64_t offset,flags,extensions;};
struct prime {uint32_t handle,flags;int32_t fd;};
struct gem_close {uint32_t handle,pad;};
struct Source {uint32_t handle;int fd;uint64_t size;void *map;unsigned smallUses;};
struct RenderContext {
    BrookVkRenderer *renderer;int output;uint32_t width,height;uint64_t outputBytes;
    BrookVkGraphicsPass *graphics;
    /* Per-layer quad compositor (default for asynchronous frames), its
     * draw plan, and this context's change-tracking state of the frame it
     * prepared (swapped with g_trackDisplayed when that frame presents). */
    BrookVkQuadPass *quad;struct QuadPlan *plan;struct DesktopTrackState *track;
    BrookVkRenderer *batchRenderer;int batchOutput;uint64_t batchOutputBytes;unsigned batchOutputSmallUses;
    uint32_t *batchScenes;struct Source batchBackdrop;int lastPartial,expiryInjected;
    uint64_t lastPresentNs;
    struct Source client[BROOK_DESKTOP_LAYER_LIMIT],chrome[BROOK_DESKTOP_LAYER_LIMIT];
    struct Source chromeSpan[BROOK_DESKTOP_LAYER_LIMIT][4];
    struct Source background,overlay[BROOK_DESKTOP_OVERLAY_MAX_BANDS];
    /* Retained overlay capture: reused while the kernel generation matches. */
    uint64_t overlayGeneration;uint32_t overlayWidth,overlayHeight;
    struct BrookDesktopOverlayBands overlayBands;
    struct Source cursor;uint64_t cursorSerial;
    struct BackdropIdentity backdropIdentity;
    struct DesktopDamageState damage;
};
struct PendingDesktopFrame {
    long ticket;uint64_t frame,started,nextReport,skippedBytes;uint32_t width,height,visible,skippedLayers;unsigned direct;
    int partial,graphics,quad,compositionReady;struct BrookGpuPresentRegions regions;struct DesktopDamageState damage;
    uint32_t statsLayers,statsDraws;uint64_t statsShaded,statsDamage;
    struct DesktopOverlayFrameState overlay;
    uint64_t traceStart,traceSubmitStart,traceReceived,traceCompleted,statsSubmitStart,statsFence;uint32_t traceWindow;uint64_t traceIncarnation,traceCommit;
    uint64_t traceChannel,traceQuery,traceClients,traceBackdrop,traceChrome,traceOverlay,tracePixels;
    uint32_t traceLayers,traceRegions;
    struct AsyncWindowSource sources[BROOK_DESKTOP_LAYER_LIMIT];
};
struct AsyncTimelineRow {uint64_t frame,ticket,window,incarnation,commit,received,start,submitStart,submitted,completed,presented,finish,gpuNs,gpuValid;
    uint64_t channel,query,clients,backdrop,chrome,overlay,pixels;uint32_t layers,regions,graphics,partial;};
static struct AsyncTimelineRow g_asyncTimeline[512];static unsigned g_asyncTimelineCount,g_asyncTimelineDropped;
static int g_asyncTimelineAllArmed;
static struct DesktopGpuChannel g_gpuChannel;
static struct AsyncSourceBarrier g_asyncSourceBarriers[BROOK_DESKTOP_LAYER_LIMIT];
static volatile sig_atomic_t g_asyncStop;
static void async_stop_signal(int signalNumber){(void)signalNumber;g_asyncStop=1;}
static uint64_t g_gpuDirectLayers;
static uint64_t g_graphicsSubmitted,g_graphicsFallback;
static uint64_t g_gpuSkippedBytes,g_gpuSkippedLayers;
static uint64_t g_gpuDirectTraced[BROOK_DESKTOP_LAYER_LIMIT];
static struct {uint64_t sum[6],maxTotal;unsigned frames;} g_stageTiming;
static int g_presentFrame,g_asyncPresent,g_dependentPartial;
/* Overlay state of the last presented frame (damage baseline), and whether
 * the kernel tracks overlay generations and exports the pointer (op 6/8);
 * otherwise the pointer stays in every per-frame overlay capture. */
static struct DesktopOverlayFrameState g_overlayDisplayed;static uint64_t g_overlayDisplayedTicket;
static int g_overlayTracked=-1;
static struct BrookDesktopOverlayCursor g_cursorInfo;
static long g_presentingTicket;
/* Change tracking: the state of the frame on screen (valid=0: unknown),
 * the last successful present, and the idle poll / lease keepalive policy.
 * The kernel's presentation lease lasts 500 ms; keep it with a minimal
 * partial present when nothing changes. */
static struct DesktopTrackState *g_trackDisplayed;
static uint64_t g_lastPresentNs;
static uint64_t g_idlePollNs=8000000ull;
enum { DESKTOP_KEEPALIVE_NS=200000000ull };
static int g_quadMode=1,g_quadOcclusion=1,g_chromeHoles=1,g_trackDamage=1;
/* Bounded runs (--frames N) present a minimal frame instead of idling. */
static int g_neverIdle;
static uint64_t g_quadSubmitted,g_quadFallback;
/* Always-on counters (desktop_stats.h), published by stats_tick(). */
static struct DesktopStats g_stats;
static volatile sig_atomic_t g_statsRequested;
static void stats_request_signal(int signalNumber){(void)signalNumber;g_statsRequested=1;}

static int present_full(uint64_t ticket,uint32_t width,uint32_t height){
    if(g_presentFrame){
        struct BrookGpuPresentFrame frame={ticket,width,height,width,0};
        return (int)syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_PRESENT_FRAME,&frame,0,0,0,0);
    }
    struct BrookGpuPresentProbe probe={ticket,0,0,width,height,width,0,{0}};
    return (int)syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_PRESENT_LEASE,&probe,0,0,0,0);
}

static void source_close(int drm,struct Source *s);
static void context_init(struct RenderContext *ctx){
    memset(ctx,0,sizeof(*ctx));ctx->output=-1;ctx->batchOutput=-1;ctx->background.fd=-1;ctx->batchBackdrop.fd=-1;ctx->cursor.fd=-1;
    for(unsigned i=0;i<BROOK_DESKTOP_OVERLAY_MAX_BANDS;++i)ctx->overlay[i].fd=-1;
    for(unsigned i=0;i<BROOK_DESKTOP_LAYER_LIMIT;++i){ctx->client[i].fd=-1;ctx->chrome[i].fd=-1;
        for(unsigned j=0;j<4;++j)ctx->chromeSpan[i][j].fd=-1;}
}

static void context_close(int drm,struct RenderContext *ctx){
    if(ctx->quad && !brook_vk_quad_pass_destroy(ctx->quad)){
        fprintf(stderr,"DESKTOP_QUAD_DESTROY_UNCERTAIN resources retained\n");for(;;)pause();
    }
    free(ctx->plan);free(ctx->track);
    if(ctx->graphics && !brook_vk_graphics_pass_destroy(ctx->graphics)){
        fprintf(stderr,"DESKTOP_GRAPHICS_DESTROY_UNCERTAIN resources retained\n");for(;;)pause();
    }
    if(ctx->renderer && !brook_vk_renderer_destroy(ctx->renderer)){
        fprintf(stderr,"DESKTOP_RENDERER_DESTROY_UNCERTAIN resources retained\n");for(;;)pause();
    }
    if(ctx->batchRenderer && !brook_vk_renderer_destroy(ctx->batchRenderer)){
        fprintf(stderr,"DESKTOP_BATCH_DESTROY_UNCERTAIN resources retained\n");for(;;)pause();
    }
    free(ctx->batchScenes);source_close(drm,&ctx->batchBackdrop);
    if(ctx->batchOutput>=0)close(ctx->batchOutput);
    if(ctx->output>=0)close(ctx->output);
    for(unsigned i=0;i<BROOK_DESKTOP_OVERLAY_MAX_BANDS;++i)source_close(drm,&ctx->overlay[i]);
    source_close(drm,&ctx->cursor);
    source_close(drm,&ctx->background);
    for(unsigned i=0;i<BROOK_DESKTOP_LAYER_LIMIT;++i){source_close(drm,&ctx->chrome[i]);source_close(drm,&ctx->client[i]);
        for(unsigned j=0;j<4;++j)source_close(drm,&ctx->chromeSpan[i][j]);}
    context_init(ctx);
}
static int context_prepare(int drm,struct RenderContext *ctx,uint32_t width,uint32_t height,size_t words){
    if(ctx->renderer && ctx->width==width && ctx->height==height)return 0;
    context_close(drm,ctx);
    ctx->output=(int)syscall(BROOK_SYS_GPU_OUTPUT_ALLOC,(long)((uint64_t)width*height*4),0L,0L,0L,0L,0L);
    if(ctx->output<0)return -1;
    {const off_t size=lseek(ctx->output,0,SEEK_END);
     if(size<(off_t)((uint64_t)width*height*4) || lseek(ctx->output,0,SEEK_SET)!=0){context_close(drm,ctx);return -1;}
     ctx->outputBytes=(uint64_t)size;}
    ctx->renderer=brook_vk_renderer_create_multi_output(words,0);
    ctx->plan=calloc(1,sizeof(*ctx->plan));ctx->track=calloc(1,sizeof(*ctx->track));
    if(!ctx->renderer || !ctx->plan || !ctx->track){context_close(drm,ctx);return -1;}
    if(g_quadMode && !getenv("BROOK_RENDER_GRAPHICS_SCENE")){
        VkPhysicalDevice physical;VkDevice device;VkQueue queue;uint32_t family;
        if(brook_vk_renderer_graphics_context(ctx->renderer,&physical,&device,&queue,&family))
            ctx->quad=brook_vk_quad_pass_create(physical,device,queue,family);
        if(!ctx->quad)fprintf(stderr,"DESKTOP_QUAD_UNAVAILABLE compute fallback\n");
    }
    if(getenv("BROOK_RENDER_GRAPHICS_SCENE")){
        VkPhysicalDevice physical;VkDevice device;VkQueue queue;uint32_t family;
        if(brook_vk_renderer_graphics_context(ctx->renderer,&physical,&device,&queue,&family))
            ctx->graphics=brook_vk_graphics_pass_create(physical,device,queue,family);
        if(!ctx->graphics)fprintf(stderr,"DESKTOP_GRAPHICS_UNAVAILABLE compute fallback\n");
    }
    if(getenv("BROOK_RENDER_PARTIAL") || getenv("BROOK_RENDER_ASYNC_PARTIAL")){
        ctx->batchScenes=calloc((size_t)BROOK_VK_MAX_TILES*BROOK_VK_TILE_STRIDE,sizeof(uint32_t));
        ctx->batchRenderer=brook_vk_renderer_create_batch_output(BROOK_VK_MAX_TILES,0);
        if(!ctx->batchScenes || !ctx->batchRenderer){context_close(drm,ctx);return -1;}
    }
    ctx->width=width;ctx->height=height;return 0;
}

static void source_close(int drm,struct Source *s){
    if(s->map && s->map!=MAP_FAILED)munmap(s->map,s->size);
    if(s->fd>=0)close(s->fd);
    if(s->handle){struct gem_close c={s->handle,0};(void)ioctl(drm,IOC(9,sizeof(c)),&c);}
    *s=(struct Source){.fd=-1};
}
static int source_open(int drm,uint64_t size,struct Source *out){
    *out=(struct Source){.fd=-1};
    if(!size || size>64ull*1024*1024)return -1;
    struct create gem={size,0,0};if(ioctl(drm,IOC(0x5b,sizeof(gem)),&gem))return -1;
    out->handle=gem.handle;out->size=gem.size;
    struct map_offset map={gem.handle,0,0,2,0};
    if(ioctl(drm,IOC(0x64,sizeof(map)),&map))goto fail;
    out->map=mmap(NULL,gem.size,PROT_READ|PROT_WRITE,MAP_SHARED,drm,(off_t)map.offset);
    if(out->map==MAP_FAILED)goto fail;
    struct prime prime={gem.handle,O_CLOEXEC|O_RDWR,-1};
    if(ioctl(drm,IOC(0x2d,sizeof(prime)),&prime))goto fail;
    out->fd=prime.fd;return 0;
fail: source_close(drm,out);return -1;
}
static int source_ensure(int drm,uint64_t size,struct Source *source){
    if(!size)return -1;
    if(source->handle && !desktop_source_should_resize(source->size,size,&source->smallUses))return 0;
    source_close(drm,source);return source_open(drm,size,source);
}
static int batch_output_ensure(struct RenderContext *ctx,uint64_t bytes){
    if(!bytes || bytes>BROOK_GPU_OUTPUT_MAX_BYTES)return -1;
    if(ctx->batchOutput>=0 && !desktop_source_should_resize(ctx->batchOutputBytes,bytes,&ctx->batchOutputSmallUses))return 0;
    if(ctx->batchOutput>=0)close(ctx->batchOutput);
    ctx->batchOutput=-1;ctx->batchOutputBytes=0;ctx->batchOutputSmallUses=0;
    int fd=(int)syscall(BROOK_SYS_GPU_OUTPUT_ALLOC,(long)bytes,0L,0L,0L,0L,0L);if(fd<0)return -1;
    off_t size=lseek(fd,0,SEEK_END);
    if(size<0 || (uint64_t)size<bytes || (size&3) || lseek(fd,0,SEEK_SET)!=0){close(fd);return -1;}
    ctx->batchOutput=fd;ctx->batchOutputBytes=(uint64_t)size;return 0;
}
static uint64_t monotonic_ns(void);
/* Returns the generation of the captured content (0 if untracked) through
 * generation. Waits in the kernel for completion where supported. */
static int capture_overlay(int drm,struct Source *output,uint32_t width,uint32_t height,
                           struct BrookDesktopOverlayBands *bands,uint64_t *generation){
    uint64_t start=monotonic_ns(),ready=0,pollEnd=0;unsigned polls=0;*generation=0;
    long token=syscall(BROOK_SYS_DESKTOP_OVERLAY,0,0,0,0,g_overlayTracked==1?BROOK_DESKTOP_OVERLAY_NO_CURSOR:0,0);
    if(token<=0){desktop_stats_overlay(&g_stats,0,0,0);return -1;}
    struct BrookDesktopOverlayInfo overlayInfo={{0},0};
    struct BrookDebugFrameInfo info={0};int result=-1,blocking=1;
    for(unsigned retry=0;retry<5000;++retry){
        ++polls;
        if(blocking){
            if(!syscall(BROOK_SYS_DESKTOP_OVERLAY,BROOK_DESKTOP_OVERLAY_WAIT,token,&overlayInfo,sizeof(overlayInfo),100,0)){
                info=overlayInfo.frame;
                if(info.state!=1)break;
                if(retry>=50)break; /* about 5 s, as before */
                continue;
            }
            if(errno!=EINVAL)goto done;
            blocking=0; /* older kernel: short backoff polling */
        }
        if(syscall(BROOK_SYS_DESKTOP_OVERLAY,1,token,&info,sizeof(info),0,0))goto done;
        if(info.state!=1)break;
        usleep(1000);
    }
    pollEnd=monotonic_ns();
    if(info.state!=2 || info.width!=width || info.height!=height || info.stride!=width ||
       info.bytes!=(uint64_t)width*height*4)goto done;
    ready=monotonic_ns();
    memset(bands,0,sizeof(*bands));
    if(syscall(BROOK_SYS_DESKTOP_OVERLAY,4,token,bands,sizeof(*bands),0,0))goto done;
    if(bands->reserved || bands->count>BROOK_DESKTOP_OVERLAY_MAX_BANDS)goto done;
    for(uint32_t i=0;i<bands->count;++i){
        const struct BrookDesktopOverlayBand *b=&bands->bands[i];
        if(!b->width || !b->height || b->x>=width || b->y>=height || b->width>width-b->x || b->height>height-b->y ||
           (i && b->y<bands->bands[i-1].y+bands->bands[i-1].height))goto done;
        const uint64_t rowBytes=(uint64_t)b->width*4;
        if(source_ensure(drm,rowBytes*b->height,&output[i]))goto done;
        if(desktop_overlay_copy_band(token,i,output[i].map,width,height,b))goto done;
    }
    if(blocking && g_overlayTracked==1)*generation=overlayInfo.generation;
    result=0;
done: (void)syscall(BROOK_SYS_DESKTOP_OVERLAY,3,token,0,0,0,0);
    if(!pollEnd)pollEnd=monotonic_ns();
    desktop_stats_overlay(&g_stats,!result,polls,start && pollEnd>=start?pollEnd-start:0);
    if(getenv("BROOK_RENDER_TIMING") && ready>=start && start){uint64_t end=monotonic_ns();
        if(end>=ready)fprintf(stderr,"DESKTOP_OVERLAY_TIMING wait_ms=%lu copy_ms=%lu polls=%u bands=%u\n",
            (unsigned long)((ready-start)/1000000ull),(unsigned long)((end-ready)/1000000ull),polls,bands->count);}
    return result;
}
/* Kernel overlay generation (op 6), or 0 when untracked (older kernel). */
static uint64_t overlay_generation(void){
    if(!g_overlayTracked)return 0;
    long generation=syscall(BROOK_SYS_DESKTOP_OVERLAY,BROOK_DESKTOP_OVERLAY_GENERATION,0,0,0,0,0);
    if(g_overlayTracked<0)g_overlayTracked=generation>0;
    return generation>0?(uint64_t)generation:0;
}
/* Pointer layer state; refreshes the context's 64x64 image on a new serial. */
static int overlay_cursor(int drm,struct RenderContext *ctx,struct DesktopOverlayCursorState *state){
    *state=(struct DesktopOverlayCursorState){0};
    if(g_overlayTracked!=1)return 0;
    const uint64_t known=ctx->cursor.handle?ctx->cursorSerial:0;
    if(syscall(BROOK_SYS_DESKTOP_OVERLAY,BROOK_DESKTOP_OVERLAY_CURSOR,0,&g_cursorInfo,sizeof(g_cursorInfo),known,0))return -1;
    const struct BrookDesktopOverlayCursor *c=&g_cursorInfo;
    if(!c->visible || !c->width || !c->height || c->width>BROOK_DESKTOP_CURSOR_MAX ||
       c->height>BROOK_DESKTOP_CURSOR_MAX || c->stride!=BROOK_DESKTOP_CURSOR_MAX)return 0;
    if(!ctx->cursor.handle || c->serial!=known){
        if(source_ensure(drm,sizeof(c->pixels),&ctx->cursor))return -1;
        memcpy(ctx->cursor.map,c->pixels,sizeof(c->pixels));ctx->cursorSerial=c->serial;
    }
    *state=(struct DesktopOverlayCursorState){1,c->width,c->height,c->x,c->y,c->serial};
    return 0;
}
static int push(uint32_t *scene,unsigned *count,struct BrookVkExternalSource *sources,
                const struct Source *source,const struct BrookVkSceneLayer *layer){
    if(*count>=BROOK_VK_MAX_LAYERS || source->fd<0)return -1;
    sources[*count]=(struct BrookVkExternalSource){source->fd,source->size/4,-1};
    memcpy(scene+BROOK_VK_SCENE_HEADER+(*count)*BROOK_VK_LAYER_WORDS,layer,sizeof(*layer));++*count;return 0;
}
// Returns 0 for a presented frame, -1 for a safe retry, and never returns if
// GPU completion is uncertain: the retained process allows OS fallback while
// preserving all userspace resources until device recovery.
static uint64_t monotonic_ns(void){struct timespec t={0};return clock_gettime(CLOCK_MONOTONIC,&t)?0:(uint64_t)t.tv_sec*1000000000ull+(uint64_t)t.tv_nsec;}
struct PendingDesktopFence {long ticket;BrookVkRenderer **renderer;long observed;int observeErrno;long released;int releaseErrno;};
static long pending_observe(void *p){struct PendingDesktopFence *f=p;
    f->observed=syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_OBSERVE,f->ticket,0,0,0,0);
    f->observeErrno=f->observed<0?errno:0;return f->observed;}
static int pending_retire(void *p){struct PendingDesktopFence *f=p;return brook_vk_renderer_retire_timeout(f->renderer);}
static long pending_release(void *p){struct PendingDesktopFence *f=p;
    f->released=syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_RELEASE,f->ticket,0,0,0,0);
    f->releaseErrno=f->released<0?errno:0;return f->released;}
struct BlockedRecovery {long ticket,begin,status,abort;};
static long blocked_call(unsigned op,long arg){long r=syscall(BROOK_SYS_GPU_HANDOFF_CTL,op,arg,0,0,0,0);return r<0?-errno:r;}
static long blocked_begin(void *p){struct BlockedRecovery *r=p;return r->begin=blocked_call(BROOK_GPU_HANDOFF_RECOVERY_BEGIN,r->ticket);}
static long blocked_status(void *p){struct BlockedRecovery *r=p;return r->status=blocked_call(BROOK_GPU_HANDOFF_RECOVERY_STATUS,0);}
static long blocked_abort(void *p){struct BlockedRecovery *r=p;return r->abort=blocked_call(BROOK_GPU_HANDOFF_RECOVERY_ABORT,0);}
static void blocked_wait(void *p){(void)p;usleep(250000);}
static int arm_test_block(long ticket){
    const char *marker=getenv("BROOK_RENDER_TEST_BLOCKED_FENCE");
    if(!marker || !*marker || !access(marker,F_OK))return 0;
    if(errno!=ENOENT || syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_TEST_BLOCK_NEXT,ticket,0,0,0,0))return -1;
    const int fd=open(marker,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);if(fd<0)return -1;
    close(fd);fprintf(stderr,"DESKTOP_TEST_BLOCKED_FENCE_ARMED\n");return 1;
}
/* Returns 0 for a presented frame, 1 for a submitted asynchronous frame,
 * 2 when nothing changed since the displayed frame (no GPU work; poll
 * again later) and -1 for a safe retry. quiescent: no other frame is in
 * flight, so the displayed frame is the baseline for change tracking. */
static int render_frame(int drm,struct RenderContext *ctx,uint32_t *visible,uint64_t *presentedNs,struct PendingDesktopFrame *pending,
                        int asyncPartial,uint64_t baselineTicket,int quiescent){
    const int traceEnabled=getenv("BROOK_RENDER_TIMELINE")!=NULL;
    const uint64_t traceStart=traceEnabled?monotonic_ns():0;
    desktop_gpu_channel_poll(&g_gpuChannel);
    const uint64_t traceChannel=traceEnabled?monotonic_ns():0;
    const uint64_t frame=desktop_gpu_channel_begin_frame(&g_gpuChannel);if(!frame)return -1;
    uint64_t timing[6]={traceEnabled?traceStart:monotonic_ns(),0,0,0,0,0},presentStartNs=0,chromeEndNs=0,traceQuery=0;
    struct Source *client=ctx->client,*chrome=ctx->chrome;
    struct Source *background=&ctx->background,*overlay=ctx->overlay;
    struct BrookDesktopLayers layers={.version=BROOK_DESKTOP_LAYERS_VERSION};
    struct BrookDesktopLayers backdrop={.version=BROOK_DESKTOP_LAYERS_VERSION};
    struct BrookVkExternalSource sources[BROOK_VK_MAX_LAYERS];
    uint8_t directSource[BROOK_VK_MAX_LAYERS]={0};
    struct DesktopGpuOffer *directOffer[BROOK_VK_MAX_LAYERS]={0};
    struct DesktopGpuOffer *preselected[BROOK_DESKTOP_LAYER_LIMIT]={0};uint32_t skipMask=0;
    struct AsyncWindowSource frameSources[BROOK_DESKTOP_LAYER_LIMIT]={{0}};
    unsigned clientSourceIndex[BROOK_DESKTOP_LAYER_LIMIT]={0};
    uint64_t traceReceived=0,traceIncarnation=0,traceCommit=0,traceSubmitStart=0;uint32_t traceWindow=0;
    uint32_t scene[SCENE_HEADER]={0};long ticket=-1;int result=-1,attemptPartial=0,savedErrno=0,presentFailed=0;unsigned frameDirect=0;const char *stage="query";
    uint64_t statsSubmitStart=0;
    if(syscall(BROOK_SYS_DESKTOP_LAYERS,BROOK_DESKTOP_LAYERS_QUERY,&layers,sizeof(layers),0,0,0) ||
       layers.count>BROOK_DESKTOP_LAYER_LIMIT ||
       syscall(BROOK_SYS_DESKTOP_LAYERS,BROOK_DESKTOP_BACKDROP_QUERY,&backdrop,sizeof(backdrop),0,0,0))goto done;
    uint32_t width=backdrop.layers[0].width,height=backdrop.layers[0].height;
    if(!width || !height || width>4096 || height>2160 || (uint64_t)width*height*4>BROOK_GPU_OUTPUT_MAX_BYTES)goto done;
    const size_t words=SCENE_HEADER;
    if(context_prepare(drm,ctx,width,height,words))goto done;
    if(traceEnabled)traceQuery=monotonic_ns();
    struct DesktopTrackState *track=ctx->track;int trackOk=g_trackDamage;
    track->valid=0;track->count=0;track->width=width;track->height=height;
    /* Output rectangles a scene layer leaves unchanged (chrome interiors). */
    struct QuadRect holes[BROOK_VK_MAX_LAYERS];memset(holes,0,sizeof(holes));
    scene[0]=width;scene[1]=height;
    for(unsigned i=0;i<layers.count;++i){
        if(getenv("BROOK_RENDER_SELECTIVE_CAPTURE")){
            struct BrookVkSceneLayer candidate;
            preselected[i]=desktop_gpu_channel_select(&g_gpuChannel,&layers.layers[i],width,height,&candidate,frame);
            if(preselected[i]){skipMask|=1u<<i;layers.layers[i].pixels=layers.layers[i].capacity=0;continue;}
        }
        if(source_ensure(drm,layers.layers[i].bytes,&client[i]))goto done;
        layers.layers[i].pixels=(uintptr_t)client[i].map;layers.layers[i].capacity=client[i].size;
    }
    stage="client_capture";
    if(syscall(BROOK_SYS_DESKTOP_LAYERS,skipMask?BROOK_DESKTOP_LAYERS_CAPTURE_TRACKED_SELECTIVE:BROOK_DESKTOP_LAYERS_CAPTURE_TRACKED,
               &layers,sizeof(layers),skipMask,0,0))goto done;
    for(unsigned i=0;i<layers.count;++i)desktop_gpu_channel_unselect(&g_gpuChannel,preselected[i],frame);
    timing[1]=monotonic_ns();
    struct BackdropIdentity identity={backdrop.layers[0].incarnation,backdrop.layers[0].generation,
        backdrop.layers[0].bytes,backdrop.layers[0].width,backdrop.layers[0].height};
    stage="backdrop_capture";
    const int reuseBackdrop=backdrop_cache_matches(ctx->backdropIdentity,identity,background->size,background->handle!=0);
    if(!reuseBackdrop){
        ctx->backdropIdentity=(struct BackdropIdentity){0};
        if(source_ensure(drm,backdrop.layers[0].bytes,background))goto done;
        backdrop.layers[0].pixels=(uintptr_t)background->map;backdrop.layers[0].capacity=background->size;
        if(syscall(BROOK_SYS_DESKTOP_LAYERS,BROOK_DESKTOP_BACKDROP_TRACKED_CAPTURE,&backdrop,sizeof(backdrop),0,0,0))goto done;
        ctx->backdropIdentity=identity;
    }
    track->display=identity.display;track->backdrop=identity.generation;
    timing[2]=monotonic_ns();
    stage="chrome_capture";
    unsigned count=0;uint64_t clientBytes=0,chromeBytes=0;
    struct DesktopDamageSet footprint={0};
    struct BrookVkSceneLayer base={0,width,width,height,0,0,width,height,0,0,width,height,0,0,width,height,255};
    if(push(scene,&count,sources,background,&base))goto done;
    for(unsigned i=0;i<layers.count;++i){
        struct BrookDesktopLayer *l=&layers.layers[i];
        // Unsupported external GPU windows and glass still fail safely.
        struct BrookVkSceneLayer content;
        if(!desktop_content_layer(l,width,height,&content))goto done;
        desktop_damage_add(&footprint,(struct DesktopDamageRect){
            content.x,content.y,(int64_t)content.x+content.width,(int64_t)content.y+content.height},width,height);
        struct DesktopGpuOffer *direct=desktop_gpu_channel_select(&g_gpuChannel,l,width,height,&content,frame);
        if(direct && getenv("BROOK_RENDER_SOURCE_GEOMETRY")){
            static unsigned samples;
            if(samples++<16)fprintf(stderr,"DESKTOP_SOURCE_GEOMETRY window=%u incarnation=%llu commit=%llu source=%ux%u destination=%ux%u fourcc=%08x offset_words=%u stride_words=%u size_words=%llu\n",
                l->window,(unsigned long long)l->incarnation,(unsigned long long)direct->packet.identity.commit,
                direct->packet.width,direct->packet.height,content.width,content.height,direct->packet.fourcc,
                direct->packet.offset,direct->packet.stride,(unsigned long long)direct->packet.words);
        }
        if((skipMask&(1u<<i)) && direct!=preselected[i])goto done;
        uint64_t snapshotCommit=0;
        if(pending && !direct)(void)syscall(BROOK_SYS_WM_QUERY_SOURCE_COMMIT,(long)l->window,(long)l->incarnation,
                                           (long)l->generation,(long)&snapshotCommit,0L,0L);
        frameSources[i]=(struct AsyncWindowSource){l->incarnation,l->generation,direct?direct->packet.identity.commit:0,snapshotCommit,l->window,0};
        clientSourceIndex[i]=count;
        if(direct){
            if(!traceWindow){traceReceived=direct->receivedNs;traceWindow=l->window;traceIncarnation=l->incarnation;traceCommit=direct->packet.identity.commit;}
            if(count>=BROOK_VK_MAX_LAYERS)goto done;
            sources[count]=(struct BrookVkExternalSource){direct->buffer,(size_t)direct->packet.words,direct->fence};
            directSource[count]=1;directOffer[count]=direct;
            memcpy(scene+BROOK_VK_SCENE_HEADER+count*BROOK_VK_LAYER_WORDS,&content,sizeof(content));++count;
        }else if(push(scene,&count,sources,&client[i],&content))goto done;
        clientBytes+=client[i].size;
        struct DesktopTrackWindow *trackWindow=&track->windows[i];
        *trackWindow=(struct DesktopTrackWindow){.window=l->window,.incarnation=l->incarnation,.direct=direct!=NULL,
            .commit=direct?direct->packet.identity.commit:0};
        memcpy(trackWindow->layer,&content,sizeof(content));
        if(trackOk && !direct && !desktop_track_bands(&trackWindow->content,client[i].map,client[i].size/4,content.sourceStrideWords,
                content.sourceX,content.sourceY,content.sourceWidthUsed,content.sourceHeightUsed,NULL))trackOk=0;
        struct BrookDesktopLayers decor={.version=BROOK_DESKTOP_LAYERS_VERSION};decor.layers[0].window=l->window;
        if(syscall(BROOK_SYS_DESKTOP_LAYERS,BROOK_DESKTOP_CHROME_QUERY,&decor,sizeof(decor),0,0,0))goto done;
        struct BrookDesktopLayer *c=&decor.layers[0];
        if(c->bytes){
            if(source_ensure(drm,c->bytes,&chrome[i]))goto done;
            c->pixels=(uintptr_t)chrome[i].map;c->capacity=chrome[i].size;
        }
        if(syscall(BROOK_SYS_DESKTOP_LAYERS,BROOK_DESKTOP_CHROME_TRACKED_CAPTURE,&decor,sizeof(decor),0,0,0))goto done;
        if(c->bytes){
            uint32_t opacity=0;if(!desktop_layer_opacity(l,&opacity))goto done;
            /* The OS leaves the client interior of the chrome transparent. */
            const struct DesktopTrackHole hole=desktop_track_chrome_hole(l,c);
            trackWindow->hasChrome=1;trackWindow->chromeOpacity=opacity;trackWindow->chromeX=c->clientX;trackWindow->chromeY=c->clientY;
            trackWindow->chromeWidth=c->width;trackWindow->chromeHeight=c->height;
            if(trackOk && !desktop_track_bands(&trackWindow->chrome,chrome[i].map,chrome[i].size/4,c->stride,0,0,c->width,c->height,&hole))trackOk=0;
            struct BrookVkSceneLayer layer={0,c->stride,c->width,c->height,0,0,c->width,c->height,
                c->clientX,c->clientY,c->width,c->height,0,0,width,height,opacity|BROOK_VK_STRAIGHT_ALPHA};
            struct DesktopChromeSpan spans[4];
            unsigned n=getenv("BROOK_RENDER_CHROME_SPANS")?desktop_chrome_spans(l,c,spans):0;
            /* Reserve the worst case for later clients/chrome and overlays;
             * otherwise retain the existing full-chrome path. */
            if(n && count+n+2*(layers.count-i-1)+BROOK_DESKTOP_OVERLAY_MAX_BANDS+1<=BROOK_VK_MAX_LAYERS){
                for(unsigned j=0;j<n;++j){const struct DesktopChromeSpan *s=&spans[j];
                    const uint64_t bytes=(uint64_t)s->width*s->height*4;
                    struct Source *part=&ctx->chromeSpan[i][j];if(source_ensure(drm,bytes,part))goto done;
                    if(!desktop_chrome_copy_span(chrome[i].map,c->stride,c->width,c->height,s,part->map,part->size/4))goto done;
                    struct BrookVkSceneLayer span={0,s->width,s->width,s->height,0,0,s->width,s->height,
                        (int32_t)((int64_t)c->clientX+s->x),(int32_t)((int64_t)c->clientY+s->y),s->width,s->height,
                        0,0,width,height,opacity|BROOK_VK_STRAIGHT_ALPHA};
                    if(push(scene,&count,sources,part,&span))goto done;
                    chromeBytes+=part->size;
                }
            }else{
                if(g_chromeHoles && hole.width && hole.height && count<BROOK_VK_MAX_LAYERS)holes[count]=(struct QuadRect){
                    (int32_t)((int64_t)c->clientX+hole.x),(int32_t)((int64_t)c->clientY+hole.y),
                    (int32_t)((int64_t)c->clientX+hole.x+hole.width),(int32_t)((int64_t)c->clientY+hole.y+hole.height)};
                if(push(scene,&count,sources,&chrome[i],&layer))goto done;
                chromeBytes+=chrome[i].size;
            }
            desktop_damage_add(&footprint,(struct DesktopDamageRect){
                c->clientX,c->clientY,(int64_t)c->clientX+c->width,(int64_t)c->clientY+c->height},width,height);
        }
    }
    chromeEndNs=monotonic_ns();
    track->count=layers.count;track->valid=trackOk;
    stage="overlay_capture";
    /* Recapture only when the kernel's overlay content generation changed. */
    const uint64_t overlayGeneration=overlay_generation();
    if(!desktop_overlay_cache_valid(ctx->overlayGeneration,ctx->overlayWidth,ctx->overlayHeight,overlayGeneration,width,height)){
        uint64_t captured=0;ctx->overlayGeneration=0;
        if(capture_overlay(drm,overlay,width,height,&ctx->overlayBands,&captured))goto done;
        ctx->overlayGeneration=captured;ctx->overlayWidth=width;ctx->overlayHeight=height;
    }
    const struct BrookDesktopOverlayBands bands=ctx->overlayBands;
    struct DesktopOverlayFrameState frameOverlay={1,width,height,ctx->overlayGeneration,bands,{0}};
    stage="overlay_cursor";
    if(overlay_cursor(drm,ctx,&frameOverlay.cursor))goto done;
    timing[3]=monotonic_ns();
    for(uint32_t i=0;i<bands.count;++i){const struct BrookDesktopOverlayBand *b=&bands.bands[i];
        struct BrookVkSceneLayer layer={0,b->width,b->width,b->height,0,0,b->width,b->height,
            b->x,b->y,b->width,b->height,0,0,width,height,255|BROOK_VK_STRAIGHT_ALPHA};
        if(push(scene,&count,sources,&overlay[i],&layer))goto done;
    }
    if(frameOverlay.cursor.visible){const struct DesktopOverlayCursorState *c=&frameOverlay.cursor;
        struct BrookVkSceneLayer layer={0,BROOK_DESKTOP_CURSOR_MAX,c->width,c->height,0,0,c->width,c->height,
            c->x,c->y,c->width,c->height,0,0,width,height,255|BROOK_VK_STRAIGHT_ALPHA};
        if(push(scene,&count,sources,&ctx->cursor,&layer))goto done;
    }
    desktop_overlay_damage(&g_overlayDisplayed,&frameOverlay,&footprint);
    scene[2]=count;
    struct DesktopDamageSet damage=desktop_damage_current(ctx->damage,identity.display,identity.generation,width,height,footprint);
    /* Change-based damage against the displayed frame, when that frame is
     * known and no other frame can present first. */
    int tracked=0;
    if(quiescent && !baselineTicket && track->valid && g_trackDisplayed && ctx->damage.display==identity.display &&
       ctx->damage.backdrop==identity.generation && ctx->damage.width==width && ctx->damage.height==height){
        struct DesktopDamageSet changes={0};
        if(desktop_track_diff(g_trackDisplayed,track,&changes)){
            desktop_overlay_damage(&g_overlayDisplayed,&frameOverlay,&changes);
            damage=changes;tracked=1;
        }
    }
    if(tracked && !damage.count){
        const uint64_t now=monotonic_ns();
        if(!g_neverIdle && g_lastPresentNs && now>=g_lastPresentNs && now-g_lastPresentNs<DESKTOP_KEEPALIVE_NS){
            ++g_stats.idleSkips;result=2;stage="idle";goto done;
        }
        /* Renew the presentation lease with a minimal, exact partial frame. */
        damage.count=1;damage.rects[0]=(struct DesktopDamageRect){0,0,width<16?width:16,1};++g_stats.keepalives;
    }
    uint64_t damagePixels=0;for(uint32_t i=0;i<damage.count;++i){const struct DesktopDamageRect *r=&damage.rects[i];
        damagePixels+=(uint64_t)(r->right-r->left)*(uint64_t)(r->bottom-r->top);}
    if(getenv("BROOK_RENDER_TIMING")){
        uint64_t overlayBytes=0;
        for(unsigned i=0;i<bands.count;++i)overlayBytes+=overlay[i].size;
        fprintf(stderr,"DESKTOP_RENDERER_BUFFERS layers=%u background=%lu client=%lu chrome=%lu overlay=%lu output=%lu metadata=%lu\n",
            count,(unsigned long)background->size,(unsigned long)clientBytes,(unsigned long)chromeBytes,
            (unsigned long)overlayBytes,(unsigned long)((uint64_t)width*height*4),
            (unsigned long)(words*sizeof(*scene)));
        fprintf(stderr,"DESKTOP_RENDERER_DAMAGE rectangles=%u pixels=%lu\n",damage.count,(unsigned long)damagePixels);
    }
    attemptPartial=(!getenv("BROOK_RENDER_GRAPHICS_SCENE") || getenv("BROOK_RENDER_GRAPHICS_PARTIAL")) &&
        (!pending || asyncPartial) && ctx->batchRenderer && ctx->damage.display==identity.display && ctx->damage.backdrop==identity.generation &&
        ctx->damage.width==width && ctx->damage.height==height && damage.count && damagePixels<(uint64_t)width*height;
    if(attemptPartial && pending && ctx->graphics && getenv("BROOK_RENDER_GRAPHICS_PARTIAL")){
        struct DesktopDamageSet aligned;
        if(desktop_graphics_damage(&damage,width,height,&aligned)){
            damage=aligned;damagePixels=0;
            for(uint32_t i=0;i<damage.count;++i){const struct DesktopDamageRect *r=&damage.rects[i];
                damagePixels+=(uint64_t)(r->right-r->left)*(uint64_t)(r->bottom-r->top);}
        }
    }
    BrookVkResult status=BROOK_VK_REJECTED;int testArmed=0,graphicsUsed=0,quadUsed=0;uint32_t traceLayers=count;
    uint32_t statsDraws=0;uint64_t statsShaded=0;
    struct BrookGpuPresentRegions regionBatch={0};
    /* Default path: per-layer quads limited to the (16-pixel aligned)
     * damage, drawn into the full-size output; present only those regions.
     * Rejection falls back to the compute paths below. */
    if(pending && ctx->quad && !getenv("BROOK_RENDER_TEST_BLOCKED_FENCE")){
        const int partialEligible=(!pending || asyncPartial) && ctx->damage.display==identity.display &&
            ctx->damage.backdrop==identity.generation && ctx->damage.width==width && ctx->damage.height==height &&
            damage.count && damagePixels<(uint64_t)width*height;
        struct DesktopDamageSet qdamage={1,{{0,0,width,height}}};int qpartial=0;
        if(partialEligible && desktop_graphics_damage(&damage,width,height,&qdamage)){
            uint64_t pixels=0;for(uint32_t i=0;i<qdamage.count;++i){const struct DesktopDamageRect *r=&qdamage.rects[i];
                pixels+=(uint64_t)(r->right-r->left)*(uint64_t)(r->bottom-r->top);}
            qpartial=pixels<(uint64_t)width*height;
        }
        if(!qpartial)qdamage=(struct DesktopDamageSet){1,{{0,0,width,height}}};
        struct QuadPlan *plan=ctx->plan;
        stage="quad_plan";
        if(quad_plan_build_auto(scene,words,holes,&qdamage,g_quadOcclusion,plan) &&
           async_sources_mark_omitted(frameSources,layers.count,clientSourceIndex,plan->used,count)){
            stage="quad_reserve";
            const int dependent=qpartial && baselineTicket;
            ticket=syscall(BROOK_SYS_GPU_HANDOFF_CTL,dependent?BROOK_GPU_HANDOFF_RESERVE_DEPENDENT:BROOK_GPU_HANDOFF_RESERVE,
                           ctx->output,dependent?baselineTicket:0,0,0,0);
            if(ticket<=0)goto done;
            struct BrookVkQuadSource inputs[BROOK_VK_MAX_LAYERS];
            for(unsigned i=0;i<count;++i){const uint32_t *l=scene+BROOK_VK_SCENE_HEADER+i*BROOK_VK_LAYER_WORDS;
                inputs[i]=(struct BrookVkQuadSource){{sources[i].fd,sources[i].syncFd,l[2],l[3],l[0],l[1],(uint64_t)sources[i].words*4u},
                    !directSource[i]};}
            const struct BrookVkQuadSource output={{ctx->output,-1,width,height,0,width,ctx->outputBytes},1};
            stage="quad_submit";
            statsSubmitStart=monotonic_ns();
            if(getenv("BROOK_RENDER_TIMELINE"))traceSubmitStart=statsSubmitStart;
            status=brook_vk_quad_pass_submit(ctx->quad,scene,words,inputs,count,&output,plan);
            if(status==BROOK_VK_UNCERTAIN){fprintf(stderr,"DESKTOP_QUAD_UNCERTAIN resources retained\n");for(;;)pause();}
            if(status==BROOK_VK_PENDING){
                quadUsed=1;++g_quadSubmitted;attemptPartial=qpartial;damage=qdamage;damagePixels=plan->damagePixels;
                traceLayers=plan->layersDrawn;statsDraws=plan->drawCount;statsShaded=plan->pixelsShaded;
                /* Only content actually sampled advances commit barriers;
                 * release culled (for example occluded) direct offers. */
                for(unsigned i=0;i<count;++i){frameDirect+=directSource[i] && plan->used[i];
                    if(!directOffer[i] || plan->used[i])continue;
                    const struct BrookGpuWindowIdentity *id=&directOffer[i]->packet.identity;
                    for(unsigned j=0;j<layers.count;++j)if(frameSources[j].window==id->window &&
                        frameSources[j].incarnation==id->incarnation && frameSources[j].commit==id->commit)frameSources[j].commit=0;
                    desktop_gpu_channel_unselect(&g_gpuChannel,directOffer[i],frame);
                }
                if(qpartial){regionBatch.count=damage.count;
                    for(uint32_t i=0;i<damage.count;++i){const struct DesktopDamageRect *r=&damage.rects[i];
                        regionBatch.regions[i]=(struct BrookGpuPresentRegion){((uint64_t)r->top*width+(uint64_t)r->left)*4u,
                            (uint32_t)r->left,(uint32_t)r->top,(uint32_t)(r->right-r->left),(uint32_t)(r->bottom-r->top),width,0};}
                }
            }else{
                ++g_quadFallback;
                if(syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_RELEASE,ticket,0,0,0,0)){
                    fprintf(stderr,"DESKTOP_QUAD_RELEASE_UNCERTAIN resources retained\n");for(;;)pause();}
                ticket=-1;
                if(getenv("BROOK_RENDER_DIAGNOSTIC"))fprintf(stderr,"DESKTOP_QUAD_REJECTED compute fallback\n");
            }
        }else ++g_quadFallback;
        if(!quadUsed)for(unsigned i=0;i<layers.count;++i)frameSources[i].omitted=0;
    }
    if(quadUsed){}
    else if(attemptPartial){
        stage="batch_prepare";
        const uint64_t bytes=(damagePixels+(uint64_t)(damage.count-1)*(DESKTOP_TILE_ALIGNMENT_WORDS-1))*4;
        size_t sourceWords[BROOK_VK_MAX_LAYERS],culledWords[BROOK_VK_MAX_LAYERS];
        struct DesktopPackedTile tiles[BROOK_VK_MAX_TILES];struct BrookVkExternalSource batchSources[BROOK_VK_MAX_LAYERS];
        uint32_t culledScene[SCENE_HEADER],selected[BROOK_VK_MAX_LAYERS];size_t batchCount=0;
        for(unsigned i=0;i<count;++i)sourceWords[i]=sources[i].words;
        if(!desktop_tile_cull_policy(scene,words,sourceWords,count,&damage,culledScene,SCENE_HEADER,selected,&batchCount,
                                     getenv("BROOK_RENDER_OCCLUSION")!=NULL))goto done;
        traceLayers=(uint32_t)batchCount;
        if(getenv("BROOK_RENDER_TIMING"))fprintf(stderr,"DESKTOP_RENDERER_CULL input=%u retained=%zu\n",count,batchCount);
        if(getenv("BROOK_RENDER_TRACE_OCCLUSION")){
            uint32_t ignored[SCENE_HEADER],referenceIndices[BROOK_VK_MAX_LAYERS];size_t referenceCount=0;
            if(!desktop_tile_cull_policy(scene,words,sourceWords,count,&damage,ignored,SCENE_HEADER,referenceIndices,&referenceCount,0))goto done;
            if(referenceCount>batchCount)fprintf(stderr,"DESKTOP_RENDERER_OCCLUDED input=%u retained=%zu baseline=%zu\n",count,batchCount,referenceCount);
        }
        uint8_t used[BROOK_VK_MAX_LAYERS]={0};
        for(size_t i=0;i<batchCount;++i){batchSources[i]=sources[selected[i]];culledWords[i]=sourceWords[selected[i]];
            used[selected[i]]=1;frameDirect+=directSource[selected[i]];}
        // A partial frame must only advance commit barriers for content that
        // was actually sampled by at least one submitted tile.
        if(!async_sources_mark_omitted(frameSources,layers.count,clientSourceIndex,used,count))goto done;
        for(unsigned i=0;i<count;++i)if(directOffer[i] && !used[i]){
            const struct BrookGpuWindowIdentity *id=&directOffer[i]->packet.identity;
            if(getenv("BROOK_RENDER_TRACE_OCCLUSION"))fprintf(stderr,"DESKTOP_RENDERER_CULLED_DIRECT window=%u incarnation=%llu commit=%llu\n",
                id->window,(unsigned long long)id->incarnation,(unsigned long long)id->commit);
            for(unsigned j=0;j<layers.count;++j)if(frameSources[j].window==id->window &&
                frameSources[j].incarnation==id->incarnation && frameSources[j].commit==id->commit)
                frameSources[j].commit=0;
            desktop_gpu_channel_unselect(&g_gpuChannel,directOffer[i],frame);
        }
        status=BROOK_VK_REJECTED;statsDraws=damage.count;
        if(pending && ctx->graphics && getenv("BROOK_RENDER_GRAPHICS_PARTIAL") && !(width&15u) &&
           batchCount<=BROOK_VK_GRAPHICS_MAX_LAYERS && !getenv("BROOK_RENDER_TEST_BLOCKED_FENCE")){
            struct BrookVkGraphicsBuffer inputs[BROOK_VK_GRAPHICS_MAX_LAYERS];
            struct BrookVkGraphicsRegion regions[BROOK_VK_GRAPHICS_MAX_REGIONS];int validInputs=1;
            for(size_t i=0;i<batchCount;++i){
                struct BrookVkSceneLayer l;memcpy(&l,culledScene+BROOK_VK_SCENE_HEADER+i*BROOK_VK_LAYER_WORDS,sizeof(l));
                if(batchSources[i].words>UINT64_MAX/4u)validInputs=0;
                inputs[i]=(struct BrookVkGraphicsBuffer){batchSources[i].fd,batchSources[i].syncFd,l.sourceWidth,l.sourceHeight,
                    l.sourceOffsetWords,l.sourceStrideWords,(uint64_t)batchSources[i].words*4u};
            }
            regionBatch.count=damage.count;
            for(uint32_t i=0;i<damage.count;++i){const struct DesktopDamageRect *r=&damage.rects[i];
                regions[i]=(struct BrookVkGraphicsRegion){(uint32_t)r->left,(uint32_t)r->top,
                    (uint32_t)(r->right-r->left),(uint32_t)(r->bottom-r->top)};
                const uint64_t offset=((uint64_t)r->top*width+(uint64_t)r->left)*4u;
                regionBatch.regions[i]=(struct BrookGpuPresentRegion){offset,(uint32_t)r->left,(uint32_t)r->top,
                    (uint32_t)(r->right-r->left),(uint32_t)(r->bottom-r->top),width,0};
                if(offset&63u)validInputs=0;
            }
            if(validInputs){
                stage="graphics_partial_reserve";
                ticket=syscall(BROOK_SYS_GPU_HANDOFF_CTL,baselineTicket?BROOK_GPU_HANDOFF_RESERVE_DEPENDENT:BROOK_GPU_HANDOFF_RESERVE,
                    ctx->output,baselineTicket,0,0,0);if(ticket<=0)goto done;
                struct BrookVkGraphicsBuffer output={ctx->output,-1,width,height,0,width,(uint64_t)width*height*4u};
                statsSubmitStart=monotonic_ns();
                if(getenv("BROOK_RENDER_TIMELINE"))traceSubmitStart=statsSubmitStart;
                status=brook_vk_graphics_pass_submit_regions(ctx->graphics,culledScene,SCENE_HEADER,inputs,batchCount,&output,regions,damage.count);
                if(status==BROOK_VK_PENDING){graphicsUsed=1;++g_graphicsSubmitted;}
                else if(status==BROOK_VK_UNCERTAIN){fprintf(stderr,"DESKTOP_GRAPHICS_PARTIAL_UNCERTAIN resources retained\n");for(;;)pause();}
                else {
                    ++g_graphicsFallback;
                    if(syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_RELEASE,ticket,0,0,0,0)){
                        fprintf(stderr,"DESKTOP_GRAPHICS_RELEASE_UNCERTAIN resources retained\n");for(;;)pause();}
                    ticket=-1;
                }
            }
        }
        if(!graphicsUsed){
        if(source_ensure(drm,bytes,&ctx->batchBackdrop) || batch_output_ensure(ctx,bytes))goto done;
        if(!desktop_tile_pack(culledScene,SCENE_HEADER,culledWords,batchCount,&damage,background->map,ctx->batchBackdrop.map,ctx->batchBackdrop.size/4,
                              ctx->batchScenes,(size_t)BROOK_VK_MAX_TILES*BROOK_VK_TILE_STRIDE,tiles,BROOK_VK_MAX_TILES))goto done;
        if(getenv("BROOK_RENDER_TEST_EXPIRE_ONCE") && !ctx->expiryInjected){
            ctx->expiryInjected=1;fprintf(stderr,"DESKTOP_RENDERER_INJECT_LEASE_EXPIRY\n");usleep(650000);
        }
        batchSources[0]=(struct BrookVkExternalSource){ctx->batchBackdrop.fd,ctx->batchBackdrop.size/4,-1};
        regionBatch.count=damage.count;
        for(uint32_t i=0;i<damage.count;++i){const struct DesktopPackedTile *t=&tiles[i];
            regionBatch.regions[i]=(struct BrookGpuPresentRegion){t->offsetBytes,t->x,t->y,t->width,t->height,t->width,0};}
        stage="batch_reserve";ticket=syscall(BROOK_SYS_GPU_HANDOFF_CTL,baselineTicket?BROOK_GPU_HANDOFF_RESERVE_DEPENDENT:BROOK_GPU_HANDOFF_RESERVE,ctx->batchOutput,baselineTicket,0,0,0);if(ticket<=0)goto done;
        stage="batch_test_arm";testArmed=arm_test_block(ticket);if(testArmed<0)goto done;
        stage="batch_compose";
        statsSubmitStart=monotonic_ns();
        if(pending && getenv("BROOK_RENDER_TIMELINE"))traceSubmitStart=statsSubmitStart;
        status=pending?brook_vk_renderer_submit_batch_output(ctx->batchRenderer,ctx->batchScenes,(size_t)damage.count*BROOK_VK_TILE_STRIDE,
                   damage.count,batchSources,batchCount,ctx->batchOutput,ctx->batchOutputBytes/4):
            brook_vk_renderer_compose_batch_output(ctx->batchRenderer,ctx->batchScenes,(size_t)damage.count*BROOK_VK_TILE_STRIDE,
                   damage.count,batchSources,batchCount,ctx->batchOutput,ctx->batchOutputBytes/4,testArmed?100000000ULL:10000000000ULL);
        }
    }else{
        for(unsigned i=0;i<count;++i)frameDirect+=directSource[i];
        statsDraws=1;
        stage="full_reserve";ticket=syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_RESERVE,ctx->output,0,0,0,0);if(ticket<=0)goto done;
        stage="full_test_arm";testArmed=arm_test_block(ticket);if(testArmed<0)goto done;
        stage="full_compose";
        statsSubmitStart=monotonic_ns();
        if(pending && getenv("BROOK_RENDER_TIMELINE"))traceSubmitStart=statsSubmitStart;
        status=BROOK_VK_REJECTED;
        if(pending && ctx->graphics && !testArmed && count<=BROOK_VK_GRAPHICS_MAX_LAYERS){
            struct BrookVkGraphicsBuffer inputs[BROOK_VK_GRAPHICS_MAX_LAYERS];
            int validInputs=1;
            for(unsigned i=0;i<count;++i){
                struct BrookVkSceneLayer l;memcpy(&l,scene+BROOK_VK_SCENE_HEADER+i*BROOK_VK_LAYER_WORDS,sizeof(l));
                if(sources[i].words>UINT64_MAX/4u)validInputs=0;
                inputs[i]=(struct BrookVkGraphicsBuffer){sources[i].fd,sources[i].syncFd,l.sourceWidth,l.sourceHeight,
                    l.sourceOffsetWords,l.sourceStrideWords,(uint64_t)sources[i].words*4u};
            }
            struct BrookVkGraphicsBuffer output={ctx->output,-1,width,height,0,width,(uint64_t)width*height*4u};
            if(validInputs)status=brook_vk_graphics_pass_submit_scene(ctx->graphics,scene,words,inputs,count,&output);
            if(status==BROOK_VK_PENDING){graphicsUsed=1;++g_graphicsSubmitted;}
            else if(status==BROOK_VK_REJECTED)++g_graphicsFallback;
            if(status==BROOK_VK_UNCERTAIN){fprintf(stderr,"DESKTOP_GRAPHICS_SUBMIT_UNCERTAIN resources retained\n");for(;;)pause();}
            if(status==BROOK_VK_REJECTED && getenv("BROOK_RENDER_DIAGNOSTIC"))fprintf(stderr,"DESKTOP_GRAPHICS_REJECTED compute fallback\n");
        }
        if(status==BROOK_VK_REJECTED)
            status=pending?brook_vk_renderer_submit_batch_output(ctx->renderer,scene,words,1,sources,count,ctx->output,(size_t)width*height):
                brook_vk_renderer_compose_multi_output(ctx->renderer,scene,words,sources,count,ctx->output,(size_t)width*height,testArmed?100000000ULL:10000000000ULL);
    }
    /* Compute shades every layer at every damaged pixel. */
    const uint64_t statsDamage=attemptPartial?damagePixels:(uint64_t)width*height;
    if(!quadUsed)statsShaded=statsDamage*traceLayers;
    if(pending && status==BROOK_VK_PENDING){
        uint32_t skipped=0;uint64_t skippedBytes=0;
        for(unsigned i=0;i<layers.count;++i)if(skipMask&(1u<<i)){++skipped;skippedBytes+=layers.layers[i].bytes;}
        uint64_t now=monotonic_ns();
        *pending=(struct PendingDesktopFrame){.ticket=ticket,.frame=frame,.started=now,.traceStart=timing[0],
            .traceSubmitStart=traceSubmitStart,.statsSubmitStart=statsSubmitStart,.traceReceived=traceReceived,.traceWindow=traceWindow,.traceIncarnation=traceIncarnation,.traceCommit=traceCommit,
            .traceChannel=traceChannel,.traceQuery=traceQuery,.traceClients=timing[1],.traceBackdrop=timing[2],.traceChrome=chromeEndNs,.traceOverlay=timing[3],
            .tracePixels=attemptPartial?damagePixels:(uint64_t)width*height,.traceLayers=traceLayers,.traceRegions=attemptPartial?damage.count:1,
            .nextReport=now && now<=UINT64_MAX-30000000000ull?now+30000000000ull:0,
            .skippedBytes=skippedBytes,.width=width,.height=height,.visible=layers.count,.skippedLayers=skipped,.direct=frameDirect,
            .partial=attemptPartial,.graphics=graphicsUsed,.quad=quadUsed,.regions=regionBatch,
            .statsLayers=traceLayers,.statsDraws=statsDraws,.statsShaded=statsShaded,.statsDamage=statsDamage,
            .damage={identity.display,identity.generation,width,height,footprint},.overlay=frameOverlay};
        memcpy(pending->sources,frameSources,sizeof(frameSources));
        return 1;
    }
    if(status==BROOK_VK_UNCERTAIN){
        struct BrookVkFailure failure=brook_vk_renderer_failure(attemptPartial?ctx->batchRenderer:ctx->renderer);
        fprintf(stderr,"DESKTOP_RENDERER_UNCERTAIN stage=%d vk_result=%d resources retained\n",failure.stage,failure.result);
        // A timeout is not completion. Keep all GPU resources untouched until
        // both the kernel handoff fence and the Vulkan fence confirm success.
        struct PendingDesktopFence pending={.ticket=ticket,.renderer=attemptPartial?&ctx->batchRenderer:&ctx->renderer};
        const struct DesktopFenceRecoveryOps ops={&pending,pending_observe,pending_retire,pending_release};
        const uint64_t reportDelay=testArmed?1000000000ull:30000000000ull;
        uint64_t started=monotonic_ns(),nextReport=started && started<=UINT64_MAX-reportDelay?started+reportDelay:0;
        for(;;){
            enum DesktopFenceRecovery recovery=desktop_fence_recovery_step(&ops);
            if(recovery==DESKTOP_FENCE_RECOVERED){
                ticket=-1;context_close(drm,ctx);
                fprintf(stderr,"DESKTOP_RENDERER_LATE_FENCE_RECOVERED\n");
                goto done;
            }
            if(recovery==DESKTOP_FENCE_RETAIN){
                fprintf(stderr,"DESKTOP_RENDERER_RELEASE_UNCERTAIN result=%ld errno=%d resources retained\n",pending.released,pending.releaseErrno);
                for(;;)pause();
            }
            uint64_t now=monotonic_ns();
            if(nextReport && now>=nextReport){
                failure=brook_vk_renderer_failure(*pending.renderer);
                long cancelled=-1;int cancelErrno=0;
                if(pending.observed==0){
                    struct BlockedRecovery attempt={.ticket=ticket};
                    const struct DesktopBlockedRecoveryOps recover={&attempt,blocked_begin,blocked_status,blocked_abort,blocked_wait};
                    const enum DesktopBlockedRecovery outcome=desktop_blocked_recovery(&recover,40);
                    fprintf(stderr,"DESKTOP_BLOCKED_RECOVERY begin=%ld status=%ld abort=%ld outcome=%d\n",attempt.begin,attempt.status,attempt.abort,outcome);
                    if(outcome==DESKTOP_BLOCKED_EXIT){fflush(stderr);_exit(75);}
                    cancelled=attempt.begin;if(cancelled<0)cancelErrno=(int)-cancelled;
                }
                fprintf(stderr,"DESKTOP_RENDERER_STILL_UNCERTAIN elapsed_s=%lu vk_stage=%d vk_result=%d vk_fence_valid=%d vk_fence=%d handoff=%ld errno=%d cancel=%ld cancel_errno=%d resources retained\n",
                        (unsigned long)((now-started)/1000000000ull),failure.stage,failure.result,failure.fence_status_valid,failure.fence_status,
                        pending.observed,pending.observeErrno,cancelled,cancelErrno);
                nextReport=now<=UINT64_MAX-60000000000ull?now+60000000000ull:0;
            }
            usleep(250000);
        }
    }
    if(status!=BROOK_VK_COMPLETE)goto done;
    stage="observe";
    long observed=syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_OBSERVE,ticket,0,0,0,0);
    if(observed!=1){
        const int observeErrno=observed<0?errno:0;
        fprintf(stderr,"DESKTOP_RENDERER_FENCE_UNKNOWN result=%ld errno=%d resources retained\n",observed,observeErrno);for(;;)pause();
    }
    timing[4]=monotonic_ns();
    stage=attemptPartial?"region_present":"full_present";
    presentStartNs=monotonic_ns();
    if(attemptPartial){regionBatch.ticket=(uint64_t)ticket;
        if(syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_PRESENT_REGIONS,&regionBatch,0,0,0,0)){presentFailed=1;goto done;}
    }else if(present_full((uint64_t)ticket,width,height)){presentFailed=1;goto done;}
    *presentedNs=monotonic_ns();ctx->lastPresentNs=*presentedNs;timing[5]=*presentedNs;ticket=-1;*visible=layers.count;result=0;
    desktop_stats_presented(&g_stats,&(struct DesktopStatsFrame){frameSources,layers.count,frameDirect,attemptPartial,graphicsUsed,
        *presentedNs,timing[0],statsSubmitStart,timing[4],0,0,graphicsUsed?DESKTOP_STATS_PATH_GRAPHICS:DESKTOP_STATS_PATH_COMPUTE,
        traceLayers,statsDraws,statsShaded,statsDamage});
    g_lastPresentNs=*presentedNs;
    desktop_gpu_channel_presented(&g_gpuChannel,frame);
    if(getenv("BROOK_RENDER_TRACE_OCCLUSION"))for(unsigned i=0;i<count;++i){
        const struct DesktopGpuOffer *offer=directOffer[i];if(!offer || offer->state!=2)continue;
        const struct BrookGpuWindowIdentity *id=&offer->packet.identity;
        fprintf(stderr,"DESKTOP_RENDERER_PRESENTED_DIRECT window=%u incarnation=%llu commit=%llu\n",
            id->window,(unsigned long long)id->incarnation,(unsigned long long)id->commit);
    }
    g_gpuDirectLayers+=frameDirect;
    for(unsigned i=0;i<layers.count;++i)if(skipMask&(1u<<i)){
        ++g_gpuSkippedLayers;g_gpuSkippedBytes+=layers.layers[i].bytes;
    }
    if(getenv("BROOK_RENDER_GPU_SOURCE_TRACE"))for(unsigned i=0;i<count;++i){
        const struct DesktopGpuOffer *offer=directOffer[i];
        if(!offer || offer->state!=2)continue;
        const struct BrookGpuWindowIdentity *id=&offer->packet.identity;
        if(id->window && id->window<=BROOK_DESKTOP_LAYER_LIMIT && g_gpuDirectTraced[id->window-1]!=id->incarnation){
            g_gpuDirectTraced[id->window-1]=id->incarnation;
            fprintf(stderr,"DESKTOP_RENDERER_DIRECT_WINDOW window=%u incarnation=%llu\n",id->window,(unsigned long long)id->incarnation);
        }
    }
#ifdef BROOK_RENDER_TEST_SOURCE_CRASH
    if(frameDirect){
        int marker=open("/tmp/brook-source-crash-once",O_WRONLY|O_CREAT|O_EXCL,0600);
        if(marker>=0){close(marker);fprintf(stderr,"DESKTOP_RENDERER_TEST_SOURCE_CRASH after present before ACK\n");kill(getpid(),SIGKILL);}
    }
#endif
    desktop_damage_commit(&ctx->damage,identity.display,identity.generation,width,height,footprint);
    if(g_trackDisplayed){
        if(track->valid){struct DesktopTrackState *shown=g_trackDisplayed;g_trackDisplayed=ctx->track;ctx->track=shown;}
        else g_trackDisplayed->valid=0;
    }
    desktop_overlay_presented(&g_overlayDisplayed,&g_overlayDisplayedTicket,&frameOverlay,0);
    ctx->lastPartial=attemptPartial;
    if(getenv("BROOK_RENDER_TIMING_AGGREGATE") && timing[0] && timing[5]>=timing[4]){
        const uint64_t stageNs[6]={timing[1]-timing[0],timing[2]-timing[1],chromeEndNs-timing[2],
            timing[3]-chromeEndNs,timing[4]-timing[3],timing[5]-timing[4]};
        for(unsigned i=0;i<6;++i)g_stageTiming.sum[i]+=stageNs[i];
        uint64_t total=timing[5]-timing[0];if(total>g_stageTiming.maxTotal)g_stageTiming.maxTotal=total;
        if(++g_stageTiming.frames==120){
            struct BrookVkTiming full=brook_vk_renderer_take_timing(ctx->renderer);
            struct BrookVkTiming batch=brook_vk_renderer_take_timing(ctx->batchRenderer);
            uint64_t samples=full.samples+batch.samples;
            if(samples)fprintf(stderr,"DESKTOP_VK_TIMING_AVG samples=%llu import_ms=%llu compose_ms=%llu cleanup_ms=%llu\n",
                (unsigned long long)samples,(unsigned long long)((full.import_ns+batch.import_ns)/samples/1000000ull),
                (unsigned long long)((full.compose_ns+batch.compose_ns)/samples/1000000ull),
                (unsigned long long)((full.cleanup_ns+batch.cleanup_ns)/samples/1000000ull));
            if(full.cache_hits+batch.cache_hits+full.cache_inserts+batch.cache_inserts)
                fprintf(stderr,"DESKTOP_VK_IMPORT_CACHE hits=%llu inserts=%llu evictions=%llu\n",
                    (unsigned long long)(full.cache_hits+batch.cache_hits),(unsigned long long)(full.cache_inserts+batch.cache_inserts),
                    (unsigned long long)(full.cache_evictions+batch.cache_evictions));
            fprintf(stderr,"DESKTOP_RENDERER_TIMING_AVG frames=120 client_ms=%llu backdrop_ms=%llu chrome_ms=%llu overlay_ms=%llu gpu_ms=%llu present_ms=%llu max_total_ms=%llu\n",
                (unsigned long long)(g_stageTiming.sum[0]/120000000ull),(unsigned long long)(g_stageTiming.sum[1]/120000000ull),
                (unsigned long long)(g_stageTiming.sum[2]/120000000ull),(unsigned long long)(g_stageTiming.sum[3]/120000000ull),
                (unsigned long long)(g_stageTiming.sum[4]/120000000ull),(unsigned long long)(g_stageTiming.sum[5]/120000000ull),
                (unsigned long long)(g_stageTiming.maxTotal/1000000ull));
            memset(&g_stageTiming,0,sizeof(g_stageTiming));
        }
    }
    if(getenv("BROOK_RENDER_TIMING") && timing[0] && timing[5]>=timing[4])
        fprintf(stderr,"DESKTOP_RENDERER_TIMING client_ms=%lu backdrop_ms=%lu chrome_ms=%lu overlay_ms=%lu gpu_ms=%lu present_ms=%lu\n",
            (unsigned long)((timing[1]-timing[0])/1000000ull),(unsigned long)((timing[2]-timing[1])/1000000ull),
            (unsigned long)((chromeEndNs-timing[2])/1000000ull),(unsigned long)((timing[3]-chromeEndNs)/1000000ull),
            (unsigned long)((timing[4]-timing[3])/1000000ull),
            (unsigned long)((timing[5]-timing[4])/1000000ull));
done:
    savedErrno=errno;
    if(result<0){if(presentFailed){++g_stats.presentFailures;if(savedErrno==ESTALE)++g_stats.presentStale;}else ++g_stats.prepareFailures;}
    /* A failed present may have changed what is on screen. */
    if(result<0 && presentFailed && g_trackDisplayed)g_trackDisplayed->valid=0;
    desktop_gpu_channel_finish(&g_gpuChannel,frame);
    if(ticket>0)(void)syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_RELEASE,ticket,0,0,0,0);
    if(attemptPartial && result<0)ctx->damage=(struct DesktopDamageState){0};
    if(result<0 && getenv("BROOK_RENDER_DIAGNOSTIC")){
        uint64_t now=monotonic_ns(),age=ctx->lastPresentNs && now>=ctx->lastPresentNs?(now-ctx->lastPresentNs)/1000000ull:0;
        uint64_t startAge=ctx->lastPresentNs && presentStartNs>=ctx->lastPresentNs?
            (presentStartNs-ctx->lastPresentNs)/1000000ull:0;
        fprintf(stderr,"DESKTOP_RENDERER_FAILURE stage=%s errno=%d age_ms=%lu present_start_age_ms=%lu\n",
                stage,savedErrno,(unsigned long)age,(unsigned long)startAge);
    }
    errno=savedErrno;
    return result;
}
static int composition_ready(struct RenderContext *ctx,BrookVkRenderer *renderer,struct PendingDesktopFrame *p){
    if(p->compositionReady)return 1;
    BrookVkResult status=p->quad?brook_vk_quad_pass_poll(ctx->quad):p->graphics?brook_vk_graphics_pass_poll(ctx->graphics):
        brook_vk_renderer_poll(renderer);
    if(status==BROOK_VK_PENDING){
        uint64_t now=monotonic_ns();
        if(!g_presentingTicket && p->nextReport && now>=p->nextReport){
            struct BlockedRecovery attempt={.ticket=p->ticket};
            const struct DesktopBlockedRecoveryOps recover={&attempt,blocked_begin,blocked_status,blocked_abort,blocked_wait};
            enum DesktopBlockedRecovery outcome=desktop_blocked_recovery(&recover,40);
            fprintf(stderr,"DESKTOP_ASYNC_BLOCKED_RECOVERY ticket=%ld begin=%ld status=%ld abort=%ld outcome=%d\n",
                    p->ticket,attempt.begin,attempt.status,attempt.abort,outcome);
            if(outcome==DESKTOP_BLOCKED_EXIT){fflush(stderr);_exit(75);}
            p->nextReport=now<=UINT64_MAX-60000000000ull?now+60000000000ull:0;
        }
        return 0;
    }
    if(status!=BROOK_VK_COMPLETE){
        if(p->graphics || p->quad){fprintf(stderr,"DESKTOP_%s_UNCERTAIN result=%d resources retained\n",p->quad?"QUAD":"GRAPHICS",status);for(;;)pause();}
        struct BrookVkFailure failure=brook_vk_renderer_failure(renderer);
        fprintf(stderr,"DESKTOP_ASYNC_UNCERTAIN stage=%d vk_result=%d resources retained\n",failure.stage,failure.result);
        for(;;)pause();
    }
    p->compositionReady=1;p->statsFence=monotonic_ns();
    p->traceCompleted=getenv("BROOK_RENDER_TIMELINE")?monotonic_ns():0;
    return 1;
}

static int submit_present(struct PendingDesktopFrame *p){
    long observed=syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_OBSERVE,p->ticket,0,0,0,0);
    if(observed!=1){fprintf(stderr,"DESKTOP_ASYNC_FENCE_UNKNOWN result=%ld errno=%d resources retained\n",observed,errno);for(;;)pause();}
    if(!async_sources_presentable(g_asyncSourceBarriers,p->sources,p->visible)){
        if(getenv("BROOK_RENDER_ASYNC_TRACE"))fprintf(stderr,"DESKTOP_ASYNC_STALE_CPU ticket=%ld\n",p->ticket);
        errno=ESTALE;return -1;
    }
    p->regions.ticket=(uint64_t)p->ticket;
    if(p->partial){
        const long op=g_asyncPresent?BROOK_GPU_HANDOFF_QUEUE_REGIONS:BROOK_GPU_HANDOFF_PRESENT_REGIONS;
        return (int)syscall(BROOK_SYS_GPU_HANDOFF_CTL,op,&p->regions,0,0,0,0);
    }
    if(!g_asyncPresent)return present_full((uint64_t)p->ticket,p->width,p->height);
    struct BrookGpuPresentFrame frame={(uint64_t)p->ticket,p->width,p->height,p->width,0};
    return (int)syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_QUEUE_FRAME,&frame,0,0,0,0);
}

/* A submitted frame owns its context, sources, ticket and producer offers
 * until both Vulkan and the OS handoff report completion. */
static int retire_async_frame(struct RenderContext *ctx,struct PendingDesktopFrame *p,uint64_t *presented,struct DesktopDamageState *displayedDamage,uint64_t lastPresentedTicket){
    BrookVkRenderer *renderer=p->partial?ctx->batchRenderer:ctx->renderer;
    if(!composition_ready(ctx,renderer,p))return 0;
    int result;
    if(g_presentingTicket){
        if(g_presentingTicket!=p->ticket)return 0;
        const long status=syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_PRESENT_RESULT,p->ticket,0,0,0,0);
        if(!status)return 0;
        if(status<0){
            fprintf(stderr,"DESKTOP_ASYNC_PRESENT_UNKNOWN ticket=%ld errno=%d resources retained\n",p->ticket,errno);
            for(;;)pause();
        }
        g_presentingTicket=0;
        result=status==1?0:-1;
        if(result)errno=(int)(status-1);
    }else {
        result=submit_present(p);
        if(g_asyncPresent){
            if(!result){g_presentingTicket=p->ticket;return 0;}
            if(errno==EBUSY){++g_stats.presentBusy;return 0;}
        }
    }
    int savedErrno=errno;
    if(!result){
        *presented=monotonic_ns();ctx->lastPresentNs=*presented;ctx->lastPartial=p->partial;
        desktop_async_damage_retire(displayedDamage,p->damage,p->partial,1);
        g_lastPresentNs=*presented;
        if(g_trackDisplayed){
            if(ctx->track->valid && (!lastPresentedTicket || (uint64_t)p->ticket>lastPresentedTicket)){
                struct DesktopTrackState *shown=g_trackDisplayed;g_trackDisplayed=ctx->track;ctx->track=shown;
            }else g_trackDisplayed->valid=0;
        }
        desktop_overlay_presented(&g_overlayDisplayed,&g_overlayDisplayedTicket,&p->overlay,(uint64_t)p->ticket);
        async_sources_committed(g_asyncSourceBarriers,p->sources,p->visible);
        desktop_gpu_channel_presented(&g_gpuChannel,p->frame);
        g_gpuDirectLayers+=p->direct;g_gpuSkippedLayers+=p->skippedLayers;g_gpuSkippedBytes+=p->skippedBytes;
        uint64_t gpuNs=0;const int gpuValid=p->quad?brook_vk_quad_pass_gpu_timestamp_ns(ctx->quad,&gpuNs):
            p->graphics?brook_vk_graphics_pass_gpu_timestamp_ns(ctx->graphics,&gpuNs):brook_vk_renderer_gpu_timestamp_ns(renderer,&gpuNs);
        desktop_stats_presented(&g_stats,&(struct DesktopStatsFrame){p->sources,p->visible,p->direct,p->partial,p->graphics,
            *presented,p->traceStart,p->statsSubmitStart,p->statsFence,gpuNs,gpuValid,
            p->quad?DESKTOP_STATS_PATH_QUAD:p->graphics?DESKTOP_STATS_PATH_GRAPHICS:DESKTOP_STATS_PATH_COMPUTE,
            p->statsLayers,p->statsDraws,p->statsShaded,p->statsDamage});
        if(getenv("BROOK_RENDER_ASYNC_TRACE"))for(unsigned i=0;i<p->visible;++i){const struct AsyncWindowSource *s=&p->sources[i];
            fprintf(stderr,"DESKTOP_ASYNC_SOURCE ticket=%ld window=%u incarnation=%llu generation=%llu source=%s commit=%llu snapshot_commit=%llu omitted=%u\n",
                p->ticket,s->window,(unsigned long long)s->incarnation,(unsigned long long)s->generation,
                s->commit?"gpu":"cpu",(unsigned long long)s->commit,(unsigned long long)s->snapshotCommit,s->omitted);}
    }else {++g_stats.presentFailures;if(savedErrno==ESTALE)++g_stats.presentStale;
        if(g_trackDisplayed)g_trackDisplayed->valid=0;
        if(!lastPresentedTicket || (uint64_t)p->ticket>lastPresentedTicket)
            desktop_async_damage_retire(displayedDamage,p->damage,p->partial,0);
        (void)syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_RELEASE,p->ticket,0,0,0,0);}
    desktop_gpu_channel_finish(&g_gpuChannel,p->frame);
    if(!result && p->graphics && getenv("BROOK_RENDER_GRAPHICS_TIMING") && p->traceStart && p->traceSubmitStart &&
       p->traceSubmitStart>=p->traceStart && p->started>=p->traceSubmitStart && p->traceCompleted>=p->started && *presented>=p->traceCompleted){
        static unsigned samples;
        if(samples++<128){
            uint64_t gpuNs=0;const int valid=brook_vk_graphics_pass_gpu_timestamp_ns(ctx->graphics,&gpuNs);
            fprintf(stderr,"DESKTOP_GRAPHICS_TIMING prepare_ns=%llu submit_ns=%llu wait_ns=%llu present_ns=%llu gpu_ns=%llu gpu_valid=%d direct=%u\n",
                (unsigned long long)(p->traceSubmitStart-p->traceStart),(unsigned long long)(p->started-p->traceSubmitStart),
                (unsigned long long)(p->traceCompleted-p->started),(unsigned long long)(*presented-p->traceCompleted),
                (unsigned long long)gpuNs,valid,p->direct);
        }
    }
    const int timelineAll=getenv("BROOK_RENDER_TIMELINE_ALL")!=NULL;
    if(timelineAll && !result && p->direct==1 && p->traceReceived)g_asyncTimelineAllArmed=1;
    if(!result && p->traceSubmitStart && p->traceCompleted &&
       ((p->direct==1 && p->traceReceived) || (timelineAll && g_asyncTimelineAllArmed))){
        const uint64_t finish=monotonic_ns();
        uint64_t gpuNs=0;const int gpuValid=p->quad?brook_vk_quad_pass_gpu_timestamp_ns(ctx->quad,&gpuNs):
            p->graphics?brook_vk_graphics_pass_gpu_timestamp_ns(ctx->graphics,&gpuNs):brook_vk_renderer_gpu_timestamp_ns(renderer,&gpuNs);
        const unsigned limit=timelineAll?512:64;
        if(g_asyncTimelineCount<limit)g_asyncTimeline[g_asyncTimelineCount++]=(struct AsyncTimelineRow){
            p->frame,(uint64_t)p->ticket,p->traceWindow,p->traceIncarnation,p->traceCommit,p->traceReceived,
            p->traceStart,p->traceSubmitStart,p->started,p->traceCompleted,*presented,finish,gpuNs,(uint64_t)gpuValid,
            p->traceChannel,p->traceQuery,p->traceClients,p->traceBackdrop,p->traceChrome,p->traceOverlay,p->tracePixels,p->traceLayers,p->traceRegions,
            p->quad?2u:(uint32_t)p->graphics,(uint32_t)p->partial};
        else ++g_asyncTimelineDropped;
    }
    *p=(struct PendingDesktopFrame){0};errno=savedErrno;return result?-1:1;
}
/* Publish the counters as JSON: write a sibling temporary file and rename it
 * over the target, at most about once per second, on SIGUSR1, and at exit.
 * BROOK_RENDER_STATS_PATH overrides the path; an empty value disables it. */
static struct {const char *path;char temporary[256];uint64_t seq,startedNs,lastNs,failures;unsigned depth;int ready,enabled;} g_statsOutput;
static char g_statsJson[32768];
static void stats_init(unsigned depth){
    const char *path=getenv("BROOK_RENDER_STATS_PATH");if(!path)path="/tmp/brook-desktop-stats.json";
    g_statsOutput.path=path;g_statsOutput.depth=depth;g_statsOutput.startedNs=monotonic_ns();g_statsOutput.ready=1;
    const int n=snprintf(g_statsOutput.temporary,sizeof(g_statsOutput.temporary),"%s.tmp",path);
    g_statsOutput.enabled=*path && n>0 && (size_t)n<sizeof(g_statsOutput.temporary);
    struct sigaction request={0};request.sa_handler=stats_request_signal;request.sa_flags=SA_RESTART;sigemptyset(&request.sa_mask);
    (void)sigaction(SIGUSR1,&request,NULL);
}
static void stats_publish(uint64_t now){
    const struct DesktopStatsMeta meta={++g_statsOutput.seq,now,g_statsOutput.startedNs,g_statsOutput.failures,(long)getpid(),
        g_statsOutput.depth,g_asyncPresent,getenv("BROOK_RENDER_GPU_TIMESTAMPS")!=NULL};
    const size_t length=desktop_stats_format(&g_stats,&meta,g_statsJson,sizeof(g_statsJson));
    const int fd=length?open(g_statsOutput.temporary,O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC,0644):-1;
    int ok=fd>=0;
    for(size_t done=0;ok && done<length;){const ssize_t n=write(fd,g_statsJson+done,length-done);
        if(n>0)done+=(size_t)n;else if(n<0 && errno==EINTR)continue;else ok=0;}
    if(fd>=0 && close(fd))ok=0;
    if(ok && rename(g_statsOutput.temporary,g_statsOutput.path))ok=0;
    if(!ok){++g_statsOutput.failures;if(fd>=0)(void)unlink(g_statsOutput.temporary);}
}
static void stats_tick(int force){
    if(!g_statsOutput.ready || !g_statsOutput.enabled)return;
    const uint64_t now=monotonic_ns();
    if(!force && !g_statsRequested && g_statsOutput.lastNs && now>=g_statsOutput.lastNs && now-g_statsOutput.lastNs<1000000000ull)return;
    const int savedErrno=errno;
    g_statsRequested=0;g_statsOutput.lastNs=now;stats_publish(now);errno=savedErrno;
}
/* Wait up to timeoutMs for a producer connection or message. */
static int desktop_channel_wait(const struct DesktopGpuChannel *c,int timeoutMs){
    struct pollfd fds[2];nfds_t n=0;
    if(c->peer>=0)fds[n++]=(struct pollfd){.fd=c->peer,.events=POLLIN};
    else if(c->listener>=0)fds[n++]=(struct pollfd){.fd=c->listener,.events=POLLIN};
    if(!n){if(timeoutMs>0)usleep((useconds_t)timeoutMs*1000u);return 0;}
    return poll(fds,n,timeoutMs)>0;
}
static int run_buffered(int drm,unsigned limit,int crash,unsigned depth){
    struct RenderContext ctx[BROOK_GPU_HANDOFF_MAX_DEPTH];
    struct PendingDesktopFrame pending[BROOK_GPU_HANDOFF_MAX_DEPTH]={{0}};
    struct DesktopDamageState displayedDamage={0};
    struct DesktopRetry retry={0};
    const char *partialGrace=getenv("BROOK_RENDER_ASYNC_PARTIAL_GRACE");
    const int overlapPartial=g_dependentPartial && getenv("BROOK_RENDER_EXPERIMENTAL_OVERLAP_PARTIAL")!=NULL &&
        getenv("BROOK_RENDER_ASYNC_PARTIAL")!=NULL;
    uint64_t lastPresentedTicket=0;
    for(unsigned i=0;i<depth;++i)context_init(&ctx[i]);
    uint64_t fullWaitNs=0;unsigned fullWaits=0;
    unsigned frames=0,partialFrames=0,retries=0,unsupportedRetries=0,idleBusyRetries=0,active=0,maxActive=0;uint64_t previous=0,maxInterval=0,failureStart=0;unsigned gaps=0;
    uint64_t idleUntil=0;unsigned idlePolls=0;
    while((!g_asyncStop && (!limit || frames<limit)) || active){
        stats_tick(0);
        const uint64_t fullSince=active==depth?monotonic_ns():0;
        int progress=0;
        for(unsigned i=0;i<depth;++i)if(pending[i].ticket){
            uint64_t presented=0;const int wasPartial=pending[i].partial;const uint64_t retiringTicket=(uint64_t)pending[i].ticket;
            int result=retire_async_frame(&ctx[i],&pending[i],&presented,&displayedDamage,lastPresentedTicket);
            if(!result)continue;
            --active;progress=1;
            if(result<0){++retries;continue;}
            lastPresentedTicket=retiringTicket;
            ++frames;if(wasPartial)++partialFrames;uint64_t interval=previous && presented>=previous?(presented-previous)/1000000ull:0;
            if(interval>maxInterval)maxInterval=interval;
            if(interval>250)++gaps;
            if(presented)previous=presented;
            if(limit || (getenv("BROOK_RENDER_PROGRESS") && frames%30==0))
                fprintf(stderr,"DESKTOP_ASYNC_FRAME frame=%u interval_ms=%lu active=%u direct_layers=%llu mode=%s\n",
                    frames,(unsigned long)interval,active,(unsigned long long)g_gpuDirectLayers,wasPartial?"partial":"full");
            if(crash && frames==limit){fprintf(stderr,"DESKTOP_RENDERER_INJECT_CRASH frame=%u\n",frames);(void)kill(getpid(),SIGKILL);return 3;}
        }
        // Frame preparation can be expensive. Check all outstanding fences
        // before starting it, and check again before filling another slot,
        // so a completed frame can release its producer without waiting for
        // an unrelated frame's preparation. Pending frames may still overlap.
        int grace=0;
        if(!overlapPartial && partialGrace && strcmp(partialGrace,"0")){
            const uint64_t now=monotonic_ns();
            for(unsigned i=0;i<depth;++i)if(pending[i].ticket &&
                desktop_async_damage_grace(1,pending[i].partial,pending[i].started,now))grace=1;
        }
        const int retryWaiting=desktop_retry_waiting(&retry,monotonic_ns());
        /* After a poll found nothing to draw, wait for the next poll or a
         * producer offer instead of recapturing the desktop at once. */
        const uint64_t loopNow=monotonic_ns();
        if(idleUntil && (loopNow>=idleUntil || desktop_channel_wait(&g_gpuChannel,0)))idleUntil=0;
        for(unsigned i=0;i<depth && !g_asyncStop && (!limit || frames+active<limit) && !idleUntil &&
            !g_presentingTicket && !grace && !retryWaiting && (overlapPartial || !getenv("BROOK_RENDER_ASYNC_PARTIAL_SERIAL") || !active);++i)if(!pending[i].ticket){
            uint32_t visible=0;uint64_t presented=0;
            ctx[i].damage=displayedDamage;
            int overlapSafe=overlapPartial && active && lastPresentedTicket;
            for(unsigned j=0;j<depth && overlapSafe;++j)if(pending[j].ticket &&
                !desktop_damage_include_pending(&ctx[i].damage,pending[j].damage))overlapSafe=0;
            if(overlapPartial && active && !overlapSafe)break;
            const int partial=getenv("BROOK_RENDER_ASYNC_PARTIAL")!=NULL && (!active || overlapSafe);
            int result=render_frame(drm,&ctx[i],&visible,&presented,&pending[i],partial,overlapSafe?lastPresentedTicket:0,!active);
            if(result==2){++idlePolls;idleUntil=monotonic_ns()+g_idlePollNs;desktop_retry_success(&retry);break;}
            if(result==1){++active;if(active>maxActive)maxActive=active;failureStart=0;desktop_retry_success(&retry);progress=1;break;}
            else if(result<0){const int failure=errno;const uint64_t now=monotonic_ns();++retries;if(!failureStart)failureStart=now;
                if(failure==ENOTSUP)++unsupportedRetries;
                if(failure==EBUSY && !active)++idleBusyRetries;
                desktop_retry_failure(&retry,failure,active,now);
                if(retries==1 || retries%100==0)fprintf(stderr,"DESKTOP_ASYNC_RETRY errno=%d\n",failure);
                if(failure==ENOTSUP || (failure==EBUSY && !active))break;}
        }
        if(limit && !active && failureStart){uint64_t now=monotonic_ns();
            if(now>=failureStart && now-failureStart>=5000000000ull)break;}
        if(!progress){
            if(!active && idleUntil){const uint64_t t=monotonic_ns();
                (void)desktop_channel_wait(&g_gpuChannel,t<idleUntil?(int)((idleUntil-t+999999ull)/1000000ull):0);}
            else usleep(!active && retryWaiting?10000:1000);
            if(fullSince){
                const uint64_t now=monotonic_ns();
                if(now>=fullSince)fullWaitNs+=now-fullSince;
                ++fullWaits;
            }
        }
    }
    for(unsigned i=0;i<depth;++i)context_close(drm,&ctx[i]);
    stats_tick(1);
    if(getenv("BROOK_RENDER_GRAPHICS_SCENE"))fprintf(stderr,"DESKTOP_GRAPHICS_COUNTS submitted=%llu fallback=%llu\n",
        (unsigned long long)g_graphicsSubmitted,(unsigned long long)g_graphicsFallback);
    fprintf(stderr,"DESKTOP_QUAD_COUNTS submitted=%llu fallback=%llu idle_polls=%u keepalives=%llu\n",(unsigned long long)g_quadSubmitted,
        (unsigned long long)g_quadFallback,idlePolls,(unsigned long long)g_stats.keepalives);
    if(getenv("BROOK_RENDER_TIMELINE")){
        for(unsigned i=0;i<g_asyncTimelineCount;++i){const struct AsyncTimelineRow *r=&g_asyncTimeline[i];
            fprintf(stderr,"DESKTOP_TIMELINE frame=%llu ticket=%llu window=%llu incarnation=%llu commit=%llu received=%llu start=%llu submit_start=%llu submitted=%llu completed=%llu presented=%llu finish=%llu gpu_ns=%llu gpu_valid=%llu\n",
                (unsigned long long)r->frame,(unsigned long long)r->ticket,(unsigned long long)r->window,(unsigned long long)r->incarnation,(unsigned long long)r->commit,
                (unsigned long long)r->received,(unsigned long long)r->start,(unsigned long long)r->submitStart,(unsigned long long)r->submitted,
                (unsigned long long)r->completed,(unsigned long long)r->presented,(unsigned long long)r->finish,
                (unsigned long long)r->gpuNs,(unsigned long long)r->gpuValid);
            fprintf(stderr,"DESKTOP_TIMELINE_DETAIL frame=%llu ticket=%llu channel=%llu query=%llu clients=%llu backdrop=%llu chrome=%llu overlay=%llu pixels=%llu layers=%u regions=%u graphics=%u partial=%u\n",
                (unsigned long long)r->frame,(unsigned long long)r->ticket,(unsigned long long)r->channel,(unsigned long long)r->query,(unsigned long long)r->clients,(unsigned long long)r->backdrop,
                (unsigned long long)r->chrome,(unsigned long long)r->overlay,(unsigned long long)r->pixels,
                r->layers,r->regions,r->graphics,r->partial);}
        fprintf(stderr,"DESKTOP_TIMELINE_SUMMARY rows=%u dropped=%u scope=%s\n",g_asyncTimelineCount,g_asyncTimelineDropped,
            getenv("BROOK_RENDER_TIMELINE_ALL")?"presented_after_first_direct":"selected_single_direct_presented_finish_attempt");
    }
    fprintf(stderr,"DESKTOP_ASYNC_STOP frames=%u partial=%u retries=%u max_active=%u interval_over_250ms=%u max_interval_ms=%lu unsupported_retries=%u idle_busy_retries=%u depth=%u full_waits=%u full_wait_ns=%llu\n",
        frames,partialFrames,retries,maxActive,gaps,(unsigned long)maxInterval,unsupportedRetries,idleBusyRetries,depth,fullWaits,(unsigned long long)fullWaitNs);
    return limit && frames!=limit?1:0;
}
int main(int argc,char **argv){
    unsigned limit=0;int crash=0;if(argc==2 && !strcmp(argv[1],"--watch"))limit=0;
    else if(argc==3 && (!strcmp(argv[1],"--frames") || !strcmp(argv[1],"--crash-after"))){
        char *end=NULL;unsigned long n=strtoul(argv[2],&end,10);if(!end || *end || !n || n>10000)return 2;
        limit=(unsigned)n;crash=!strcmp(argv[1],"--crash-after");
    }
    else {fprintf(stderr,"usage: brook-desktop-renderer --watch | --frames N | --crash-after N\n");return 2;}
    unsigned depth=getenv("BROOK_RENDER_DOUBLE_BUFFER")?BROOK_GPU_HANDOFF_DEFAULT_DEPTH:0;
    const char *bufferCount=getenv("BROOK_RENDER_BUFFER_COUNT");
    if(bufferCount){
        if(!strcmp(bufferCount,"2"))depth=BROOK_GPU_HANDOFF_DEFAULT_DEPTH;
        else if(!strcmp(bufferCount,"3"))depth=BROOK_GPU_HANDOFF_MAX_DEPTH;
        else {fprintf(stderr,"BROOK_RENDER_BUFFER_COUNT must be 2 or 3\n");return 2;}
    }
    int drm=open("/dev/dri/renderD129",O_RDWR|O_CLOEXEC);if(drm<0){perror("desktop render node");return 1;}
    const unsigned attachDepth=depth>BROOK_GPU_HANDOFF_DEFAULT_DEPTH?depth:0;
    if(syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_ATTACH,attachDepth,0,0,0,0)){perror("desktop attach");close(drm);return 1;}
    const long caps=syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_PRESENT_CAPS,0,0,0,0,0);
    g_presentFrame=caps>0 && (caps&BROOK_GPU_PRESENT_CAP_FRAME);
    g_dependentPartial=caps>0 && (caps&BROOK_GPU_PRESENT_CAP_DEPENDENT);
    const char *asyncPresent=getenv("BROOK_RENDER_ASYNC_PRESENT");
    g_asyncPresent=depth && asyncPresent && !strcmp(asyncPresent,"1") &&
        caps>0 && (caps&BROOK_GPU_PRESENT_CAP_ASYNC);
    desktop_gpu_channel_init(&g_gpuChannel);
    stats_init(depth);
    /* Composition: per-layer quads by default (asynchronous frames only);
     * BROOK_RENDER_COMPOSE=compute keeps the compute shader. Damage is
     * change-based unless BROOK_RENDER_FOOTPRINT_DAMAGE is set. */
    const char *compose=getenv("BROOK_RENDER_COMPOSE");
    g_quadMode=!(compose && !strcmp(compose,"compute"));
    g_quadOcclusion=!getenv("BROOK_RENDER_QUAD_NO_OCCLUSION");
    g_chromeHoles=!getenv("BROOK_RENDER_CHROME_FULL");
    g_trackDamage=!getenv("BROOK_RENDER_FOOTPRINT_DAMAGE");
    const char *idlePoll=getenv("BROOK_RENDER_IDLE_POLL_MS");
    if(idlePoll){char *end=NULL;unsigned long ms=strtoul(idlePoll,&end,10);if(end && !*end && ms && ms<=100)g_idlePollNs=ms*1000000ull;}
    g_trackDisplayed=calloc(1,sizeof(*g_trackDisplayed));
    g_neverIdle=limit!=0 && !getenv("BROOK_RENDER_ALLOW_IDLE");
    if(depth){
        struct sigaction stop={0};stop.sa_handler=async_stop_signal;sigemptyset(&stop.sa_mask);
        if(sigaction(SIGTERM,&stop,NULL) || sigaction(SIGINT,&stop,NULL))return 1;
        int result=run_buffered(drm,limit,crash,depth);desktop_gpu_channel_close(&g_gpuChannel);
        (void)syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_DETACH,0,0,0,0,0);close(drm);return result;
    }
    struct RenderContext ctx;context_init(&ctx);
    unsigned frames=0,failures=0,gaps=0,retries=0;uint64_t previous=0,maxInterval=0,failureStart=0;
    while(!limit || frames<limit){
        uint32_t visible=0;uint64_t presented=0;
        stats_tick(0);
        const int rendered=render_frame(drm,&ctx,&visible,&presented,NULL,0,0,1);
        if(rendered==2){(void)desktop_channel_wait(&g_gpuChannel,(int)(g_idlePollNs/1000000ull));continue;}
        if(!rendered){
            ++frames;failures=0;failureStart=0;
            uint64_t interval=previous && presented>=previous?(presented-previous)/1000000ull:0;
            if(interval>maxInterval)maxInterval=interval;
            if(interval>250)++gaps;
            if(presented)previous=presented;
            if(limit || (getenv("BROOK_RENDER_PROGRESS") && frames%30==0))
                fprintf(stderr,"DESKTOP_RENDERER_FRAME frame=%u windows=%u interval_ms=%lu mode=%s direct_layers=%llu skipped_layers=%llu skipped_bytes=%llu\n",frames,visible,(unsigned long)interval,ctx.lastPartial?"partial":"full",(unsigned long long)g_gpuDirectLayers,(unsigned long long)g_gpuSkippedLayers,(unsigned long long)g_gpuSkippedBytes);
            if(crash && frames==limit){
                fprintf(stderr,"DESKTOP_RENDERER_INJECT_CRASH frame=%u\n",frames);
                (void)kill(getpid(),SIGKILL);return 3;
            }
        }
        else {
            const int failure=errno;uint64_t now=monotonic_ns();++retries;
            if(++failures==1){failureStart=now;fprintf(stderr,"DESKTOP_RENDERER_RETRY errno=%d\n",failure);}
            else if(failures%100==0)fprintf(stderr,"DESKTOP_RENDERER_RETRY errno=%d\n",failure);
            if(limit && ((failureStart && now>=failureStart && now-failureStart>=5000000000ull) || failures>=500))break;
            usleep(desktop_sync_retry_delay_us(failure));
        }
    }
    context_close(drm,&ctx);stats_tick(1);
    desktop_gpu_channel_close(&g_gpuChannel);
    (void)syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_DETACH,0,0,0,0,0);close(drm);
    fprintf(stderr,"DESKTOP_RENDERER_STOP frames=%u failures=%u retries=%u interval_over_250ms=%u max_interval_ms=%lu\n",frames,failures,retries,gaps,(unsigned long)maxInterval);
    if(g_gpuDirectLayers)fprintf(stderr,"DESKTOP_RENDERER_GPU_SOURCE_LAYERS=%llu\n",(unsigned long long)g_gpuDirectLayers);
    return limit && frames!=limit?1:0;
}
