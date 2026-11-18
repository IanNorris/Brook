#pragma once
/* One optional provider connection. Called only by the renderer thread and
 * never while a GPU submission is uncertain. An acknowledgement means this
 * renderer has no remaining GPU use of the exact offered source.
 * Version 2 offers (see gpu_window_channel.h) are retained: the newest
 * presented offer of a window stays selectable in later frames, so the
 * provider need not copy it into the window's CPU buffer. */
#include "gpu_window_channel.h"
#include "desktop_layers_abi.h"
#include "scene.h"
#include "gpu_output_abi.h"
#include <sys/un.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <limits.h>
#include <sys/ioctl.h>
#include <string.h>
#define DESKTOP_GPU_OFFERS 64
/* Frames that may sample one retained offer concurrently; at least the
 * handoff's maximum depth, plus the selective-capture preselection. */
#define DESKTOP_GPU_OFFER_USES 4
/* uses: in-flight frames sampling a version 2 offer. current: the newest
 * presented offer of its window, kept for later frames. notify: 1 queued,
 * 2 sent PRESENTED. */
/* Version 3 offers carry layers (count>0): layer 0's buffer and fence are
 * buffer/fence; extraBuffer/extraFence[i-1] hold layer i. */
struct DesktopGpuOffer {struct BrookGpuWindowPacket packet;int buffer,fence,state;uint64_t frame,receivedNs;
    uint64_t uses[DESKTOP_GPU_OFFER_USES];int current,notify,failed,retire;
    unsigned layers;struct BrookGpuWindowLayer layer[BROOK_GPU_WINDOW_LAYERS_MAX];
    int extraBuffer[BROOK_GPU_WINDOW_LAYERS_MAX-1],extraFence[BROOK_GPU_WINDOW_LAYERS_MAX-1];};
static inline void desktop_gpu_offer_free(struct DesktopGpuOffer *o){
    if(o->buffer>=0)close(o->buffer);
    if(o->fence>=0)close(o->fence);
    for(unsigned i=1;i<o->layers && i<BROOK_GPU_WINDOW_LAYERS_MAX;++i){
        if(o->extraBuffer[i-1]>=0)close(o->extraBuffer[i-1]);
        if(o->extraFence[i-1]>=0)close(o->extraFence[i-1]);}
    *o=(struct DesktopGpuOffer){.buffer=-1,.fence=-1};
}
/* Buffer and fence of layer i (i < max(1, layers)). */
static inline int desktop_gpu_offer_buffer(const struct DesktopGpuOffer *o,unsigned i){return i?o->extraBuffer[i-1]:o->buffer;}
static inline int desktop_gpu_offer_fence(const struct DesktopGpuOffer *o,unsigned i){return i?o->extraFence[i-1]:o->fence;}
static inline uint64_t desktop_gpu_offer_inode(const struct DesktopGpuOffer *o,unsigned i,uint64_t *device){
    if(!o->layers){*device=o->packet.identity.device;return o->packet.identity.inode;}
    *device=o->layer[i].device;return o->layer[i].inode;
}
/* Two offers that sample a common DMA-BUF (any layer). */
static inline int desktop_gpu_offers_share(const struct DesktopGpuOffer *a,const struct DesktopGpuOffer *b){
    const unsigned na=a->layers?a->layers:1,nb=b->layers?b->layers:1;
    for(unsigned i=0;i<na;++i){uint64_t da,ia=desktop_gpu_offer_inode(a,i,&da);
        for(unsigned j=0;j<nb;++j){uint64_t db,ib=desktop_gpu_offer_inode(b,j,&db);if(ia==ib && da==db)return 1;}}
    return 0;
}
/* A producer fence that completed with an error (SYNC_IOC_FILE_INFO status
 * < 0). Its frame never rendered, and a compositor job sampling or waiting
 * on it would fail too, so such offers are never selected; the window falls
 * back to its CPU source. 0 = pending/ok, 1 = failed. */
struct DesktopSyncFileInfo {char name[32];int32_t status;uint32_t flags,numFences,pad;uint64_t fences;};
#define DESKTOP_SYNC_IOC_FILE_INFO 0xc0383e04u
static inline int desktop_gpu_fence_failed(int fd){
    if(fd<0)return 0;
    struct DesktopSyncFileInfo info;memset(&info,0,sizeof(info));
    return !ioctl(fd,DESKTOP_SYNC_IOC_FILE_INFO,&info) && info.status<0;
}
#ifndef DESKTOP_GPU_FENCE_FAILED
#define DESKTOP_GPU_FENCE_FAILED(fd) desktop_gpu_fence_failed(fd)
#endif
/* A producer buffer that a later GPU job failed on is sticky-failed in the
 * kernel even when the offer's own fence succeeded (a retained offer whose
 * buffer the client rendered into again). Any job referencing it is
 * refused, so the compositor must not sample it. Older kernels: 0. */
#ifndef DESKTOP_GPU_BUFFER_FAILED
#define DESKTOP_GPU_BUFFER_FAILED(fd) ((fd)>=0 && syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_BUFFER_FAILED,(long)(fd),0,0,0,0)==1)
#endif
static inline int desktop_gpu_offer_retained(const struct DesktopGpuOffer *o){
    return o->packet.version>=BROOK_GPU_WINDOW_WIRE_VERSION_RETAIN;
}
static inline int desktop_gpu_offer_used_by(const struct DesktopGpuOffer *o,uint64_t frame){
    if(!frame)return 0;
    for(unsigned i=0;i<DESKTOP_GPU_OFFER_USES;++i)if(o->uses[i]==frame)return 1;
    return 0;
}
static inline int desktop_gpu_offer_in_use(const struct DesktopGpuOffer *o){
    for(unsigned i=0;i<DESKTOP_GPU_OFFER_USES;++i)if(o->uses[i])return 1;
    return 0;
}
struct DesktopGpuChannel {int listener,peer;char path[sizeof(((struct sockaddr_un *)0)->sun_path)];struct DesktopGpuOffer offers[DESKTOP_GPU_OFFERS];
    struct {uint64_t incarnation,commit;} presented[BROOK_DESKTOP_LAYER_LIMIT];uint64_t nextFrame;
    /* Newest tagged CPU snapshot seen for each window (selection time). An
     * offer at or below it can never be shown again, presented or not. */
    struct {uint64_t incarnation,commit;} snapshot[BROOK_DESKTOP_LAYER_LIMIT];};
static inline uint64_t desktop_gpu_channel_begin_frame(struct DesktopGpuChannel *c){
    if(c->nextFrame==UINT64_MAX)return 0;
    return ++c->nextFrame;
}
static inline void desktop_gpu_channel_init(struct DesktopGpuChannel *c){
    memset(c,0,sizeof(*c));c->listener=c->peer=-1;
    for(unsigned i=0;i<DESKTOP_GPU_OFFERS;++i)c->offers[i].buffer=c->offers[i].fence=-1;
    const char *path=getenv("BROOK_DESKTOP_GPU_SOCKET");if(!path || !*path)return;
    size_t len=strlen(path);if(len>=sizeof(c->path))return;
    int fd=socket(AF_UNIX,SOCK_SEQPACKET|SOCK_NONBLOCK|SOCK_CLOEXEC,0);if(fd<0)return;
    struct sockaddr_un addr={.sun_family=AF_UNIX};memcpy(addr.sun_path,path,len+1);
    if(bind(fd,(struct sockaddr *)&addr,sizeof(addr))){
        /* A crashed renderer can leave its pathname behind. Only remove a
         * socket owned by this uid after a connection is refused and a second
         * stat confirms the same inode. Never replace a live listener. */
        struct stat before={0},after={0};int probe=-1,stale=0;
        if(errno==EADDRINUSE && !lstat(path,&before) && S_ISSOCK(before.st_mode) && before.st_uid==geteuid()){
            probe=socket(AF_UNIX,SOCK_SEQPACKET|SOCK_NONBLOCK|SOCK_CLOEXEC,0);
            if(probe>=0 && connect(probe,(struct sockaddr *)&addr,sizeof(addr))<0 && errno==ECONNREFUSED &&
               !lstat(path,&after) && before.st_dev==after.st_dev && before.st_ino==after.st_ino)stale=1;
        }
        if(probe>=0)close(probe);
        if(!stale || unlink(path) || bind(fd,(struct sockaddr *)&addr,sizeof(addr))){close(fd);return;}
    }
    /* The renderer and provider can run under different users. An explicitly
     * configured provider group gets access without making the socket writable
     * by every local user. Keep ownership unchanged for stale-socket recovery. */
    const char *group=getenv("BROOK_DESKTOP_GPU_GID");
    if(group){
        char *end=NULL;errno=0;unsigned long gid=strtoul(group,&end,10);
        if(!*group || *group=='-' || !end || *end || errno || gid>=UINT_MAX ||
           chown(path,(uid_t)-1,(gid_t)gid) || chmod(path,0770)){
            unlink(path);close(fd);return;
        }
    }
    if(listen(fd,1)){unlink(path);close(fd);return;}
    memcpy(c->path,path,len+1);c->listener=fd;
}
static inline void desktop_gpu_channel_disconnect(struct DesktopGpuChannel *c){
    if(c->peer>=0)close(c->peer);
    c->peer=-1;
    for(unsigned i=0;i<DESKTOP_GPU_OFFERS;++i)desktop_gpu_offer_free(&c->offers[i]);
}
static inline void desktop_gpu_channel_close(struct DesktopGpuChannel *c){
    desktop_gpu_channel_disconnect(c);if(c->listener>=0)close(c->listener);c->listener=-1;
    if(c->path[0])unlink(c->path);
    c->path[0]=0;
}
/* state: 0 free, 1 available, 2 selected, 3 safe acknowledgement queued. */
static inline void desktop_gpu_channel_flush(struct DesktopGpuChannel *c){
    if(c->peer<0)return;
    /* PRESENTED precedes the SAFE of the same offer: SAFE ends the identity. */
    for(unsigned i=0;i<DESKTOP_GPU_OFFERS;++i){struct DesktopGpuOffer *o=&c->offers[i];if(!o->state || o->notify!=1)continue;
        int sent=brook_gpu_window_send_reply(c->peer,o->packet.version,BROOK_GPU_WINDOW_PRESENTED,&o->packet.identity);
        if(!sent)return;
        if(sent<0){desktop_gpu_channel_disconnect(c);return;}
        o->notify=2;
    }
    for(unsigned i=0;i<DESKTOP_GPU_OFFERS;++i){struct DesktopGpuOffer *o=&c->offers[i];if(o->state!=3)continue;
        /* An ACK can let the producer rewrite the underlying DMA-BUF, even
         * when a different offer for that same buffer is still in flight. */
        int busy=0;for(unsigned j=0;j<DESKTOP_GPU_OFFERS;++j){const struct DesktopGpuOffer *other=&c->offers[j];
            if(other->state==2 && desktop_gpu_offers_share(other,o)){busy=1;break;}}
        if(busy)continue;
        int sent=brook_gpu_window_send_reply(c->peer,o->packet.version,BROOK_GPU_WINDOW_SAFE,&o->packet.identity);if(!sent)return;
        if(sent<0){desktop_gpu_channel_disconnect(c);return;}
        desktop_gpu_offer_free(o);
    }
}
static inline void desktop_gpu_channel_poll(struct DesktopGpuChannel *c){
    desktop_gpu_channel_flush(c);
    if(c->listener<0)return;
    if(c->peer<0){int fd=accept4(c->listener,NULL,NULL,SOCK_NONBLOCK|SOCK_CLOEXEC);if(fd<0)return;c->peer=fd;}
    for(unsigned n=0;n<DESKTOP_GPU_OFFERS;++n){unsigned slot=0;
        while(slot<DESKTOP_GPU_OFFERS && c->offers[slot].state)++slot;
        if(slot==DESKTOP_GPU_OFFERS)return;
        struct BrookGpuWindowLayersPacket wire;int fds[2*BROOK_GPU_WINDOW_LAYERS_MAX];unsigned nfds=0;
        int got=brook_gpu_window_receive_layers(c->peer,&wire,fds,&nfds);
        if(!got)return;
        if(got<0 || wire.header.type!=BROOK_GPU_WINDOW_OFFER){
            for(unsigned i=0;got>0 && i<nfds;++i)close(fds[i]);
            desktop_gpu_channel_disconnect(c);return;
        }
        const struct BrookGpuWindowPacket packet=wire.header;
        uint64_t received=0;
        if(getenv("BROOK_RENDER_TIMELINE")){struct timespec t={0};
            if(!clock_gettime(CLOCK_MONOTONIC,&t))received=(uint64_t)t.tv_sec*1000000000ull+(uint64_t)t.tv_nsec;}
        struct DesktopGpuOffer *o=&c->offers[slot];
        *o=(struct DesktopGpuOffer){.packet=packet,.buffer=fds[0],.fence=fds[1],.state=1,.receivedNs=received,.layers=wire.count};
        for(unsigned i=0;i<BROOK_GPU_WINDOW_LAYERS_MAX-1;++i)o->extraBuffer[i]=o->extraFence[i]=-1;
        for(unsigned i=0;i<wire.count;++i){o->layer[i]=wire.layers[i];
            if(i){o->extraBuffer[i-1]=fds[2*i];o->extraFence[i-1]=fds[2*i+1];}}
    }
}
/* Selection uses the OS capture's non-reused window identity, never a title.
 * Native toplevels only; the offered full source scales to the client VFB. */
/* Why a window that has an offer was not drawn from it this frame (stats
 * JSON "gpu_select"). Counted only for windows with at least one offer. */
enum {DESKTOP_GPU_SELECTED,DESKTOP_GPU_REFUSE_SHAPE,DESKTOP_GPU_REFUSE_NO_CANDIDATE,DESKTOP_GPU_REFUSE_FAILED,
      DESKTOP_GPU_REFUSE_SNAPSHOT_NEWER,DESKTOP_GPU_REFUSE_VALIDATE,DESKTOP_GPU_REFUSE_USES,DESKTOP_GPU_REFUSE_TILED,
      DESKTOP_GPU_SELECT_REASONS};
static uint64_t g_desktopGpuSelect[DESKTOP_GPU_SELECT_REASONS];
static const char *const desktop_gpu_select_names[DESKTOP_GPU_SELECT_REASONS]={"selected","shape","no_candidate","failed",
    "snapshot_newer","validate","uses_full","tiled"};
/* Set by the renderer for each frame: the frame is drawn by the quad pass on
 * a device that samples DRM format modifiers. Only then may an offer with an
 * X/Y-tiled layer be selected; other paths read sources as linear, so such
 * a window keeps the provider's (detiled) CPU source. */
static int g_desktopGpuTiledOk;
static inline uint64_t desktop_gpu_offer_modifier(const struct DesktopGpuOffer *o,unsigned i){
    return o && o->layers && i<o->layers?brook_gpu_window_layer_modifier(o->layer[i].flags):0;
}
static inline int desktop_gpu_offer_tiled(const struct DesktopGpuOffer *o){
    for(unsigned i=0;o && i<o->layers;++i)if(o->layer[i].flags&BROOK_GPU_WINDOW_LAYER_TILING)return 1;
    return 0;
}
/* Layer i of a version 3 offer as a scene layer: window VFB coordinates are
 * placed at the client origin and clipped to the client rectangle cropped to
 * the VFB (the CPU source's 1:1 crop), intersected with the screen. */
static inline unsigned desktop_gpu_offer_layer_count(const struct DesktopGpuOffer *o){return o && o->layers?o->layers:1;}
static inline void desktop_gpu_offer_scene_layer(const struct DesktopGpuOffer *o,const struct BrookDesktopLayer *l,
                                                 uint32_t screenW,uint32_t screenH,unsigned i,struct BrookVkSceneLayer *out){
    const struct BrookGpuWindowLayer *g=&o->layer[i];
    uint32_t opacity=(l->flags&BROOK_DESKTOP_LAYER_HAS_OPACITY)?(l->flags&BROOK_DESKTOP_LAYER_OPACITY_MASK)>>BROOK_DESKTOP_LAYER_OPACITY_SHIFT:255;
    const uint32_t flags=(g->fourcc==0x34325258 || (g->flags&BROOK_GPU_WINDOW_LAYER_OPAQUE))?(BROOK_VK_STRAIGHT_ALPHA|BROOK_VK_OPAQUE_SOURCE):0;
    int64_t cx0=l->clientX,cy0=l->clientY;
    int64_t cx1=cx0+(l->width<l->clientWidth?l->width:l->clientWidth),cy1=cy0+(l->height<l->clientHeight?l->height:l->clientHeight);
    if(cx0<0)cx0=0;
    if(cy0<0)cy0=0;
    if(cx1>(int64_t)screenW)cx1=screenW;
    if(cy1>(int64_t)screenH)cy1=screenH;
    if(cx1<cx0)cx1=cx0;
    if(cy1<cy0)cy1=cy0;
    *out=(struct BrookVkSceneLayer){g->offset,g->stride,g->width,g->height,g->srcX,g->srcY,g->srcWidth,g->srcHeight,
        (int32_t)(l->clientX+g->dstX),(int32_t)(l->clientY+g->dstY),g->dstWidth,g->dstHeight,
        (int32_t)cx0,(int32_t)cy0,(uint32_t)(cx1-cx0),(uint32_t)(cy1-cy0),opacity|flags};
}
static inline struct DesktopGpuOffer *desktop_gpu_channel_select(struct DesktopGpuChannel *c,const struct BrookDesktopLayer *l,
                                                                  uint32_t screenW,uint32_t screenH,struct BrookVkSceneLayer *out,uint64_t frame){
    if(!frame || !l || !out || !l->window || l->window>BROOK_DESKTOP_LAYER_LIMIT)return NULL;
    int offered=0;
    for(unsigned i=0;i<DESKTOP_GPU_OFFERS && !offered;++i)
        offered=c->offers[i].packet.identity.window==l->window && c->offers[i].packet.identity.incarnation==l->incarnation;
    /* Native VFBs are drawn 1:1 and cropped to the client rectangle by the
     * CPU-source path (desktop_content_layer.h), including while the client
     * size and the committed buffer differ (resize in flight, or a client
     * that picks its own size). The offer is the same pixels, so it is drawn
     * the same way below. Legacy upscaled VFBs stay on the CPU source. */
    if((l->flags&BROOK_DESKTOP_LAYER_LEGACY_OPAQUE) || !l->clientWidth || !l->clientHeight){
        if(offered){
            ++g_desktopGpuSelect[DESKTOP_GPU_REFUSE_SHAPE];
            static unsigned logged;
            if(logged<8){++logged;fprintf(stderr,"DESKTOP_GPU_SOURCE_SHAPE window=%u flags=0x%x layer=%ux%u client=%ux%u\n",
                l->window,l->flags,l->width,l->height,l->clientWidth,l->clientHeight);}
        }
        return NULL;
    }
    /* The provider composes on the CPU (and tags the snapshot) whenever an
     * offer cannot represent a commit. Offers at or below that tag are
     * older than what the window shows: never select them, and let finish
     * retire them (SAFE) even if they were never presented. Without this an
     * unpresented offer superseded only by CPU content is held forever and
     * the client runs out of buffers. */
    if(offered){uint64_t tag=0;
        if(!syscall(BROOK_SYS_WM_QUERY_SOURCE_COMMIT,(long)l->window,(long)l->incarnation,(long)l->generation,(long)&tag,0L,0L) && tag &&
           (c->snapshot[l->window-1].incarnation!=l->incarnation || tag>c->snapshot[l->window-1].commit)){
            c->snapshot[l->window-1].incarnation=l->incarnation;c->snapshot[l->window-1].commit=tag;}}
    const uint64_t tagged=c->snapshot[l->window-1].incarnation==l->incarnation?c->snapshot[l->window-1].commit:0;
    struct DesktopGpuOffer *best=NULL;
    for(unsigned i=0;i<DESKTOP_GPU_OFFERS;++i){struct DesktopGpuOffer *o=&c->offers[i];
        if(o->packet.identity.window!=l->window || o->packet.identity.incarnation!=l->incarnation || o->failed)continue;
        if(o->packet.identity.commit<=tagged){
            if(o->state==1 || o->state==2){o->current=0;++g_desktopGpuSelect[DESKTOP_GPU_REFUSE_SNAPSHOT_NEWER];}
            continue;}
        const int newer=c->presented[l->window-1].incarnation!=l->incarnation || o->packet.identity.commit>c->presented[l->window-1].commit;
        if(desktop_gpu_offer_retained(o)){
            /* In-flight offers stay selectable so consecutive frames never
             * step back to an older source while a newer one is retiring. */
            if((o->state!=1 && o->state!=2) || (!newer && !o->current))continue;
        }else if(o->state!=1 || !newer)continue;
        if(!best || o->packet.identity.commit>best->packet.identity.commit)best=o;
    }
    if(!best){if(offered)++g_desktopGpuSelect[DESKTOP_GPU_REFUSE_NO_CANDIDATE];return NULL;}
    /* A client/VFB size mismatch is drawn only when the offer is the VFB
     * itself (cropped 1:1 below); anything else keeps the CPU source. */
    if(!best->layers && (l->width!=l->clientWidth || l->height!=l->clientHeight) &&
       (best->packet.width!=l->width || best->packet.height!=l->height)){++g_desktopGpuSelect[DESKTOP_GPU_REFUSE_SHAPE];return NULL;}
    int layerFailed=0;
    for(unsigned i=1;i<best->layers;++i)layerFailed|=DESKTOP_GPU_FENCE_FAILED(best->extraFence[i-1]) || DESKTOP_GPU_BUFFER_FAILED(best->extraBuffer[i-1]);
    if(layerFailed || DESKTOP_GPU_FENCE_FAILED(best->fence) || DESKTOP_GPU_BUFFER_FAILED(best->buffer)){
        best->failed=1;best->current=0;
        fprintf(stderr,"DESKTOP_GPU_SOURCE_FAILED window=%u incarnation=%llu commit=%llu (producer fence or buffer failed; CPU source)\n",
            best->packet.identity.window,(unsigned long long)best->packet.identity.incarnation,(unsigned long long)best->packet.identity.commit);
        ++g_desktopGpuSelect[DESKTOP_GPU_REFUSE_FAILED];return NULL;
    }
    if(desktop_gpu_offer_tiled(best) && !g_desktopGpuTiledOk){
        best->failed=best->retire=1;best->current=0;++g_desktopGpuSelect[DESKTOP_GPU_REFUSE_TILED];return NULL;}
    if(desktop_gpu_offer_retained(best) && best->current){
        /* The provider repaints its CPU buffer only when the GPU source can
         * no longer represent the window (for example a new subsurface or a
         * CPU fallback). A tagged snapshot at least as new replaces it. */
        uint64_t snapshot=0;
        if(!syscall(BROOK_SYS_WM_QUERY_SOURCE_COMMIT,(long)l->window,(long)l->incarnation,(long)l->generation,(long)&snapshot,0L,0L) &&
           snapshot>=best->packet.identity.commit){best->current=0;++g_desktopGpuSelect[DESKTOP_GPU_REFUSE_SNAPSHOT_NEWER];return NULL;}
    }
    if(syscall(BROOK_SYS_GPU_WINDOW_VALIDATE,(long)c->peer,(long)l->window,(long)l->incarnation,0L,0L,0L)){
        ++g_desktopGpuSelect[DESKTOP_GPU_REFUSE_VALIDATE];return NULL;}
    if(desktop_gpu_offer_retained(best) && !desktop_gpu_offer_used_by(best,frame)){
        unsigned slot=0;while(slot<DESKTOP_GPU_OFFER_USES && best->uses[slot])++slot;
        if(slot==DESKTOP_GPU_OFFER_USES){++g_desktopGpuSelect[DESKTOP_GPU_REFUSE_USES];return NULL;}
        best->uses[slot]=frame;
    }
    uint32_t opacity=(l->flags&BROOK_DESKTOP_LAYER_HAS_OPACITY)?(l->flags&BROOK_DESKTOP_LAYER_OPACITY_MASK)>>BROOK_DESKTOP_LAYER_OPACITY_SHIFT:255;
    uint32_t flags=best->packet.fourcc==0x34325258?(BROOK_VK_STRAIGHT_ALPHA|BROOK_VK_OPAQUE_SOURCE):0;
    if(best->packet.width==l->width && best->packet.height==l->height &&
       (l->width!=l->clientWidth || l->height!=l->clientHeight)){
        /* The offer is the VFB's pixels: crop 1:1 like the CPU source. */
        const uint32_t w=l->width<l->clientWidth?l->width:l->clientWidth,h=l->height<l->clientHeight?l->height:l->clientHeight;
        *out=(struct BrookVkSceneLayer){best->packet.offset,best->packet.stride,best->packet.width,best->packet.height,
            0,0,w,h,l->clientX,l->clientY,w,h,0,0,screenW,screenH,opacity|flags};
    }else *out=(struct BrookVkSceneLayer){best->packet.offset,best->packet.stride,best->packet.width,best->packet.height,
        0,0,best->packet.width,best->packet.height,l->clientX,l->clientY,l->clientWidth,l->clientHeight,
        0,0,screenW,screenH,opacity|flags};
    if(best->layers)desktop_gpu_offer_scene_layer(best,l,screenW,screenH,0,out);
    ++g_desktopGpuSelect[DESKTOP_GPU_SELECTED];
    best->state=2;best->frame=frame;return best;
}
static inline int desktop_gpu_offer_in_frame(const struct DesktopGpuOffer *o,uint64_t frame){
    return desktop_gpu_offer_retained(o)?(o->state==2 && desktop_gpu_offer_used_by(o,frame)):(o->state==2 && o->frame==frame);
}
/* Undo a selection that the frame will not sample (culled, or preselected
 * before capture and selected again). */
static inline void desktop_gpu_channel_unselect(struct DesktopGpuChannel *c,struct DesktopGpuOffer *o,uint64_t frame){
    (void)c;if(!o || o->state!=2)return;
    if(!desktop_gpu_offer_retained(o)){o->state=1;return;}
    for(unsigned i=0;i<DESKTOP_GPU_OFFER_USES;++i)if(o->uses[i]==frame)o->uses[i]=0;
    if(!desktop_gpu_offer_in_use(o))o->state=1;
}
/* Call only after the OS accepted the frame. Prevent later delivery of an
 * older commit from replacing a newer frame for the same window incarnation. */
static inline void desktop_gpu_channel_presented(struct DesktopGpuChannel *c,uint64_t frame){
    if(!frame)return;
    for(unsigned i=0;i<DESKTOP_GPU_OFFERS;++i){struct DesktopGpuOffer *o=&c->offers[i];if(!desktop_gpu_offer_in_frame(o,frame))continue;
        uint32_t slot=o->packet.identity.window;if(!slot || slot>BROOK_DESKTOP_LAYER_LIMIT)continue;
        if(c->presented[slot-1].incarnation!=o->packet.identity.incarnation || c->presented[slot-1].commit<o->packet.identity.commit){
            c->presented[slot-1].incarnation=o->packet.identity.incarnation;c->presented[slot-1].commit=o->packet.identity.commit;}
    }
    for(unsigned i=0;i<DESKTOP_GPU_OFFERS;++i){struct DesktopGpuOffer *o=&c->offers[i];
        if(!desktop_gpu_offer_retained(o) || !desktop_gpu_offer_in_frame(o,frame))continue;
        uint32_t slot=o->packet.identity.window;if(!slot || slot>BROOK_DESKTOP_LAYER_LIMIT)continue;
        if(c->presented[slot-1].incarnation!=o->packet.identity.incarnation || c->presented[slot-1].commit!=o->packet.identity.commit)continue;
        if(!o->notify)o->notify=1;
        if(o->current)continue;
        for(unsigned j=0;j<DESKTOP_GPU_OFFERS;++j)if(c->offers[j].packet.identity.window==o->packet.identity.window)c->offers[j].current=0;
        o->current=1;
    }
    desktop_gpu_channel_flush(c);
}
/* App-driven cadence (desktop_app_present.h): the frame that samples the
 * dominant window's offer has been submitted, so tell the provider now
 * instead of after the OS present. PRESENTED only releases the client's
 * frame callbacks; the presented watermark, current source and SAFE still
 * wait for desktop_gpu_channel_presented/finish, so buffer reuse is
 * unchanged. A frame that then fails to present costs the client one early
 * callback, never a buffer. Returns the offers notified. */
static inline unsigned desktop_gpu_channel_notify_early(struct DesktopGpuChannel *c,uint64_t frame,uint32_t window){
    unsigned n=0;
    if(!frame || !window)return 0;
    for(unsigned i=0;i<DESKTOP_GPU_OFFERS;++i){struct DesktopGpuOffer *o=&c->offers[i];
        if(o->notify || o->packet.identity.window!=window || !desktop_gpu_offer_retained(o) || !desktop_gpu_offer_in_frame(o,frame))continue;
        o->notify=1;++n;}
    if(n)desktop_gpu_channel_flush(c);
    return n;
}
/* A retained offer that is idle can still be shown only while it is the
 * window's current source or the newest unpresented commit. */
static inline int desktop_gpu_offer_dead(struct DesktopGpuChannel *c,const struct DesktopGpuOffer *o,uint64_t frame){
    const struct BrookGpuWindowIdentity *id=&o->packet.identity;
    if(!id->window || id->window>BROOK_DESKTOP_LAYER_LIMIT)return 1;
    /* A tiled offer this renderer cannot draw is never selected again; once
     * no frame uses it, SAFE lets the provider fall back to its detiled CPU
     * source instead of leaving the window on a stale VFB. */
    if(o->retire)return 1;
    if(c->snapshot[id->window-1].incarnation==id->incarnation && id->commit<=c->snapshot[id->window-1].commit)return 1;
    if(!o->current){
        if(c->presented[id->window-1].incarnation==id->incarnation && id->commit<=c->presented[id->window-1].commit)return 1;
        for(unsigned i=0;i<DESKTOP_GPU_OFFERS;++i){const struct DesktopGpuOffer *p=&c->offers[i];
            if(p!=o && desktop_gpu_offer_retained(p) && (p->state==1 || p->state==2) && p->packet.identity.window==id->window &&
               p->packet.identity.incarnation==id->incarnation && p->packet.identity.commit>id->commit)return 1;}
    }
    /* A window that is not composed (closed, minimized, occluded) is checked
     * against the OS; only a closed or replaced incarnation releases it. */
    if(o->frame!=frame && c->peer>=0 &&
       syscall(BROOK_SYS_GPU_WINDOW_VALIDATE,(long)c->peer,(long)id->window,(long)id->incarnation,0L,0L,0L))return 1;
    return 0;
}
/* Safe only before GPU submission or after its completion has been proved. */
static inline void desktop_gpu_channel_finish(struct DesktopGpuChannel *c,uint64_t frame){
    if(!frame)return;
    for(unsigned i=0;i<DESKTOP_GPU_OFFERS;++i){struct DesktopGpuOffer *o=&c->offers[i];
        if(desktop_gpu_offer_retained(o)){
            if(o->state!=1 && o->state!=2)continue;
            for(unsigned j=0;j<DESKTOP_GPU_OFFER_USES;++j)if(o->uses[j]==frame)o->uses[j]=0;
            if(o->state==2 && !desktop_gpu_offer_in_use(o))o->state=1;
        }else if(o->state==1 || (o->state==2 && o->frame==frame))o->state=3;
    }
    for(unsigned i=0;i<DESKTOP_GPU_OFFERS;++i){struct DesktopGpuOffer *o=&c->offers[i];
        if(!desktop_gpu_offer_retained(o) || o->state!=1 || !desktop_gpu_offer_dead(c,o,frame))continue;
        o->current=0;o->state=3;
    }
    desktop_gpu_channel_flush(c);
}
