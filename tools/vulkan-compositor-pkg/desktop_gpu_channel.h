#pragma once
/* One optional provider connection. Called only by the renderer thread and
 * never while a GPU submission is uncertain. An acknowledgement means this
 * renderer has no remaining GPU use of the exact offered source. */
#include "gpu_window_channel.h"
#include "desktop_layers_abi.h"
#include "scene.h"
#include <sys/un.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <limits.h>
#define DESKTOP_GPU_OFFERS 64
struct DesktopGpuOffer {struct BrookGpuWindowPacket packet;int buffer,fence,state;uint64_t frame,receivedNs;};
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
    for(unsigned i=0;i<DESKTOP_GPU_OFFERS;++i){struct DesktopGpuOffer *o=&c->offers[i];if(o->state!=3)continue;
        /* An ACK can let the producer rewrite the underlying DMA-BUF, even
         * when a different offer for that same buffer is still in flight. */
        int busy=0;for(unsigned j=0;j<DESKTOP_GPU_OFFERS;++j){const struct DesktopGpuOffer *other=&c->offers[j];
            if(other->state==2 && other->packet.identity.device==o->packet.identity.device &&
               other->packet.identity.inode==o->packet.identity.inode){busy=1;break;}}
        if(busy)continue;
        int sent=brook_gpu_window_send_safe(c->peer,&o->packet.identity);if(!sent)return;
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
        if(o->state!=1 || o->packet.identity.window!=l->window || o->packet.identity.incarnation!=l->incarnation)continue;
        if(c->presented[l->window-1].incarnation==l->incarnation && o->packet.identity.commit<=c->presented[l->window-1].commit)continue;
        if(!best || o->packet.identity.commit>best->packet.identity.commit)best=o;
    }
    if(!best)return NULL;
    if(syscall(BROOK_SYS_GPU_WINDOW_VALIDATE,(long)c->peer,(long)l->window,(long)l->incarnation,0L,0L,0L))return NULL;
    uint32_t opacity=(l->flags&BROOK_DESKTOP_LAYER_HAS_OPACITY)?(l->flags&BROOK_DESKTOP_LAYER_OPACITY_MASK)>>BROOK_DESKTOP_LAYER_OPACITY_SHIFT:255;
    uint32_t flags=best->packet.fourcc==0x34325258?(BROOK_VK_STRAIGHT_ALPHA|BROOK_VK_OPAQUE_SOURCE):0;
    *out=(struct BrookVkSceneLayer){best->packet.offset,best->packet.stride,best->packet.width,best->packet.height,
        0,0,best->packet.width,best->packet.height,l->clientX,l->clientY,l->clientWidth,l->clientHeight,
        0,0,screenW,screenH,opacity|flags};
    best->state=2;best->frame=frame;return best;
}
/* Call only after the OS accepted the frame. Prevent later delivery of an
 * older commit from replacing a newer frame for the same window incarnation. */
static inline void desktop_gpu_channel_presented(struct DesktopGpuChannel *c,uint64_t frame){
    if(!frame)return;
    for(unsigned i=0;i<DESKTOP_GPU_OFFERS;++i){struct DesktopGpuOffer *o=&c->offers[i];if(o->state!=2 || o->frame!=frame)continue;
        uint32_t slot=o->packet.identity.window;if(!slot || slot>BROOK_DESKTOP_LAYER_LIMIT)continue;
        if(c->presented[slot-1].incarnation!=o->packet.identity.incarnation || c->presented[slot-1].commit<o->packet.identity.commit){
            c->presented[slot-1].incarnation=o->packet.identity.incarnation;c->presented[slot-1].commit=o->packet.identity.commit;}
    }
}
/* Safe only before GPU submission or after its completion has been proved. */
static inline void desktop_gpu_channel_finish(struct DesktopGpuChannel *c,uint64_t frame){
    if(!frame)return;
    for(unsigned i=0;i<DESKTOP_GPU_OFFERS;++i)if(c->offers[i].state==1 || (c->offers[i].state==2 && c->offers[i].frame==frame))c->offers[i].state=3;
    desktop_gpu_channel_flush(c);
}
