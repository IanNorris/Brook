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
#include <sys/un.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <limits.h>
#define DESKTOP_GPU_OFFERS 64
/* Frames that may sample one retained offer concurrently; at least the
 * handoff's maximum depth, plus the selective-capture preselection. */
#define DESKTOP_GPU_OFFER_USES 4
/* uses: in-flight frames sampling a version 2 offer. current: the newest
 * presented offer of its window, kept for later frames. notify: 1 queued,
 * 2 sent PRESENTED. */
struct DesktopGpuOffer {struct BrookGpuWindowPacket packet;int buffer,fence,state;uint64_t frame,receivedNs;
    uint64_t uses[DESKTOP_GPU_OFFER_USES];int current,notify;};
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
    struct {uint64_t incarnation,commit;} presented[BROOK_DESKTOP_LAYER_LIMIT];uint64_t nextFrame;};
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
    for(unsigned i=0;i<DESKTOP_GPU_OFFERS;++i){struct DesktopGpuOffer *o=&c->offers[i];
        if(o->buffer>=0)close(o->buffer);
        if(o->fence>=0)close(o->fence);
        *o=(struct DesktopGpuOffer){.buffer=-1,.fence=-1};}
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
            if(other->state==2 && other->packet.identity.device==o->packet.identity.device &&
               other->packet.identity.inode==o->packet.identity.inode){busy=1;break;}}
        if(busy)continue;
        int sent=brook_gpu_window_send_reply(c->peer,o->packet.version,BROOK_GPU_WINDOW_SAFE,&o->packet.identity);if(!sent)return;
        if(sent<0){desktop_gpu_channel_disconnect(c);return;}
        if(o->buffer>=0)close(o->buffer);
        if(o->fence>=0)close(o->fence);
        *o=(struct DesktopGpuOffer){.buffer=-1,.fence=-1};
    }
}
static inline void desktop_gpu_channel_poll(struct DesktopGpuChannel *c){
    desktop_gpu_channel_flush(c);
    if(c->listener<0)return;
    if(c->peer<0){int fd=accept4(c->listener,NULL,NULL,SOCK_NONBLOCK|SOCK_CLOEXEC);if(fd<0)return;c->peer=fd;}
    for(unsigned n=0;n<DESKTOP_GPU_OFFERS;++n){unsigned slot=0;
        while(slot<DESKTOP_GPU_OFFERS && c->offers[slot].state)++slot;
        if(slot==DESKTOP_GPU_OFFERS)return;
        struct BrookGpuWindowPacket packet={0};int fds[2];int got=brook_gpu_window_receive(c->peer,&packet,fds);
        if(!got)return;
        if(got<0 || packet.type!=BROOK_GPU_WINDOW_OFFER){
            if(got>0){if(fds[0]>=0)close(fds[0]);if(fds[1]>=0)close(fds[1]);}
            desktop_gpu_channel_disconnect(c);return;
        }
        uint64_t received=0;
        if(getenv("BROOK_RENDER_TIMELINE")){struct timespec t={0};
            if(!clock_gettime(CLOCK_MONOTONIC,&t))received=(uint64_t)t.tv_sec*1000000000ull+(uint64_t)t.tv_nsec;}
        c->offers[slot]=(struct DesktopGpuOffer){.packet=packet,.buffer=fds[0],.fence=fds[1],.state=1,.receivedNs=received};
    }
}
/* Selection uses the OS capture's non-reused window identity, never a title.
 * Native toplevels only; the offered full source scales to the client VFB. */
static inline struct DesktopGpuOffer *desktop_gpu_channel_select(struct DesktopGpuChannel *c,const struct BrookDesktopLayer *l,
                                                                  uint32_t screenW,uint32_t screenH,struct BrookVkSceneLayer *out,uint64_t frame){
    if(!frame || !l || !out || !l->window || l->window>BROOK_DESKTOP_LAYER_LIMIT || (l->flags&BROOK_DESKTOP_LAYER_LEGACY_OPAQUE) || !l->clientWidth || !l->clientHeight ||
       l->width!=l->clientWidth || l->height!=l->clientHeight)return NULL;
    struct DesktopGpuOffer *best=NULL;
    for(unsigned i=0;i<DESKTOP_GPU_OFFERS;++i){struct DesktopGpuOffer *o=&c->offers[i];
        if(o->packet.identity.window!=l->window || o->packet.identity.incarnation!=l->incarnation)continue;
        const int newer=c->presented[l->window-1].incarnation!=l->incarnation || o->packet.identity.commit>c->presented[l->window-1].commit;
        if(desktop_gpu_offer_retained(o)){
            /* In-flight offers stay selectable so consecutive frames never
             * step back to an older source while a newer one is retiring. */
            if((o->state!=1 && o->state!=2) || (!newer && !o->current))continue;
        }else if(o->state!=1 || !newer)continue;
        if(!best || o->packet.identity.commit>best->packet.identity.commit)best=o;
    }
    if(!best)return NULL;
    if(desktop_gpu_offer_retained(best) && best->current){
        /* The provider repaints its CPU buffer only when the GPU source can
         * no longer represent the window (for example a new subsurface or a
         * CPU fallback). A tagged snapshot at least as new replaces it. */
        uint64_t snapshot=0;
        if(!syscall(BROOK_SYS_WM_QUERY_SOURCE_COMMIT,(long)l->window,(long)l->incarnation,(long)l->generation,(long)&snapshot,0L,0L) &&
           snapshot>=best->packet.identity.commit){best->current=0;return NULL;}
    }
    if(syscall(BROOK_SYS_GPU_WINDOW_VALIDATE,(long)c->peer,(long)l->window,(long)l->incarnation,0L,0L,0L))return NULL;
    if(desktop_gpu_offer_retained(best) && !desktop_gpu_offer_used_by(best,frame)){
        unsigned slot=0;while(slot<DESKTOP_GPU_OFFER_USES && best->uses[slot])++slot;
        if(slot==DESKTOP_GPU_OFFER_USES)return NULL;
        best->uses[slot]=frame;
    }
    uint32_t opacity=(l->flags&BROOK_DESKTOP_LAYER_HAS_OPACITY)?(l->flags&BROOK_DESKTOP_LAYER_OPACITY_MASK)>>BROOK_DESKTOP_LAYER_OPACITY_SHIFT:255;
    uint32_t flags=best->packet.fourcc==0x34325258?(BROOK_VK_STRAIGHT_ALPHA|BROOK_VK_OPAQUE_SOURCE):0;
    *out=(struct BrookVkSceneLayer){best->packet.offset,best->packet.stride,best->packet.width,best->packet.height,
        0,0,best->packet.width,best->packet.height,l->clientX,l->clientY,l->clientWidth,l->clientHeight,
        0,0,screenW,screenH,opacity|flags};
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
/* A retained offer that is idle can still be shown only while it is the
 * window's current source or the newest unpresented commit. */
static inline int desktop_gpu_offer_dead(struct DesktopGpuChannel *c,const struct DesktopGpuOffer *o,uint64_t frame){
    const struct BrookGpuWindowIdentity *id=&o->packet.identity;
    if(!id->window || id->window>BROOK_DESKTOP_LAYER_LIMIT)return 1;
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
