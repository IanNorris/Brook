#pragma once
/* Experimental provider/renderer transport. The owner must hold the source
 * unchanged until a matching SAFE acknowledgement; EOF, bad messages and
 * GPU uncertainty are not acknowledgements. Window identity still requires
 * validation against the OS desktop capture before use or presentation. */
#include "gpu_window_source_abi.h"
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#define BROOK_GPU_WINDOW_WIRE_MAGIC 0x42575047u
#define BROOK_GPU_WINDOW_WIRE_VERSION 1u
/* Version 2 (retained sources). The provider marks each OFFER with version 2
 * to request these semantics; a renderer that answers keeps every reply in
 * the version of the offer it refers to:
 *  - The renderer may keep sampling the newest presented offer of a window in
 *    later frames. It sends SAFE only once no in-flight GPU work reads the
 *    offer AND the offer can no longer be shown: a newer commit of the window
 *    was presented, a newer offer superseded it before use, the provider's
 *    tagged CPU snapshot is at least as new, or the window incarnation ended.
 *    SAFE keeps its version 1 meaning: no remaining renderer GPU use.
 *  - PRESENTED is sent at most once per offer, before its SAFE, after the OS
 *    accepted the first frame that sampled it. It carries no fds and grants
 *    nothing; the provider may use it to pace frame callbacks.
 * Version 1 offers keep the original behaviour: SAFE after the frame that
 * received them, no PRESENTED. */
#define BROOK_GPU_WINDOW_WIRE_VERSION_RETAIN 2u
/* Version 3 (layered windows) keeps every version 2 rule and adds a layer
 * list: one OFFER describes a window commit as up to 8 buffers, each with a
 * source rectangle (buffer pixels) and a destination rectangle (window VFB
 * pixels, before the renderer's 1:1 crop to the client rectangle). The
 * header describes layer 0 exactly as a version 2 offer would, and its
 * identity names layer 0's buffer. SCM_RIGHTS carries buffer0, fence0,
 * buffer1, fence1, ... The commit is one unit: PRESENTED and SAFE refer to
 * the header identity and cover every layer. A renderer must understand
 * version 3 before a provider sends it (they ship together in a pair). */
#define BROOK_GPU_WINDOW_WIRE_VERSION_LAYERS 3u
#define BROOK_GPU_WINDOW_LAYERS_MAX 8u
/* The layer's pixels all have alpha 255 (XRGB or known-opaque). */
#define BROOK_GPU_WINDOW_LAYER_OPAQUE 1u
/* The layer's buffer is Intel X- or Y-tiled (I915_FORMAT_MOD_X_TILED /
 * Y_TILED, no bit-6 swizzle on Gen8+), not linear. stride is the pitch in
 * pixels (a multiple of 128 for X, 32 for Y) and words covers whole tile
 * rows (8 for X, 32 for Y). At most one of the two is set. Version 3 only;
 * a renderer that does not sample the layout must not select the offer. */
#define BROOK_GPU_WINDOW_LAYER_TILED_X 2u
#define BROOK_GPU_WINDOW_LAYER_TILED_Y 4u
#define BROOK_GPU_WINDOW_LAYER_TILING (BROOK_GPU_WINDOW_LAYER_TILED_X|BROOK_GPU_WINDOW_LAYER_TILED_Y)
#define BROOK_DRM_MOD_I915_X_TILED 0x0100000000000001ull
#define BROOK_DRM_MOD_I915_Y_TILED 0x0100000000000002ull
static inline uint64_t brook_gpu_window_layer_modifier(uint32_t flags){
    return (flags&BROOK_GPU_WINDOW_LAYER_TILED_X)?BROOK_DRM_MOD_I915_X_TILED:
        (flags&BROOK_GPU_WINDOW_LAYER_TILED_Y)?BROOK_DRM_MOD_I915_Y_TILED:0;
}
#define BROOK_GPU_WINDOW_OFFER 1u
#define BROOK_GPU_WINDOW_SAFE 2u
#define BROOK_GPU_WINDOW_PRESENTED 3u
struct BrookGpuWindowPacket {
    uint32_t magic,version,type,reserved;
    struct BrookGpuWindowIdentity identity;
    uint64_t words;
    uint32_t offset,stride,width,height,fourcc,reserved2;
};
static inline int brook_gpu_window_packet_valid(const struct BrookGpuWindowPacket *p){
    if(!p || p->magic!=BROOK_GPU_WINDOW_WIRE_MAGIC ||
       (p->version!=BROOK_GPU_WINDOW_WIRE_VERSION && p->version!=BROOK_GPU_WINDOW_WIRE_VERSION_RETAIN &&
        p->version!=BROOK_GPU_WINDOW_WIRE_VERSION_LAYERS) ||
       p->reserved || p->reserved2 || !brook_gpu_window_identity_valid(&p->identity))return 0;
    if(p->type==BROOK_GPU_WINDOW_PRESENTED && p->version<BROOK_GPU_WINDOW_WIRE_VERSION_RETAIN)return 0;
    if(p->type==BROOK_GPU_WINDOW_SAFE || p->type==BROOK_GPU_WINDOW_PRESENTED)
        return !p->words && !p->offset && !p->stride && !p->width && !p->height && !p->fourcc;
    if(p->type!=BROOK_GPU_WINDOW_OFFER || !p->words || p->words>UINT32_MAX || !p->width || !p->height ||
       p->width>16384 || p->height>16384 || p->stride<p->width || p->stride>16384 ||
       (p->fourcc!=0x34325241 && p->fourcc!=0x34325258))return 0;
    return (uint64_t)p->offset+(uint64_t)(p->height-1)*p->stride+p->width<=p->words;
}
static inline int brook_gpu_window_fd_matches(const struct BrookGpuWindowPacket *p,int fd){
    struct stat s;return p && fd>=0 && !fstat(fd,&s) && s.st_ino && (uint64_t)s.st_dev==p->identity.device &&
        (uint64_t)s.st_ino==p->identity.inode && s.st_size>=0 && (uint64_t)s.st_size==p->words*4;
}
/* SOCK_SEQPACKET is required: one offer plus exactly two SCM_RIGHTS fds is
 * atomic. Return 1 sent/received, 0 would block, -1 closed/invalid/error.
 * Sender retains both fds; receiver owns them only on successful receipt. */
static inline int brook_gpu_window_send_offer(int socket,const struct BrookGpuWindowPacket *p,int buffer,int fence){
    if(!brook_gpu_window_packet_valid(p) || p->type!=BROOK_GPU_WINDOW_OFFER ||
       !brook_gpu_window_fd_matches(p,buffer) || fence<0){errno=EINVAL;return -1;}
    union {struct cmsghdr align;char data[CMSG_SPACE(2*sizeof(int))];} control={0};struct iovec io={(void *)p,sizeof(*p)};
    struct msghdr msg={.msg_iov=&io,.msg_iovlen=1,.msg_control=control.data,.msg_controllen=sizeof(control.data)};
    struct cmsghdr *c=CMSG_FIRSTHDR(&msg);c->cmsg_level=SOL_SOCKET;c->cmsg_type=SCM_RIGHTS;c->cmsg_len=CMSG_LEN(2*sizeof(int));
    int fds[2]={buffer,fence};memcpy(CMSG_DATA(c),fds,sizeof(fds));
    ssize_t n=sendmsg(socket,&msg,MSG_DONTWAIT|MSG_NOSIGNAL);
    if(n==(ssize_t)sizeof(*p))return 1;
    if(n<0 && (errno==EAGAIN || errno==EWOULDBLOCK || errno==EINTR))return 0;
    return -1;
}
/* Renderer replies (SAFE or PRESENTED) without fds, in the offer's version. */
static inline int brook_gpu_window_send_reply(int socket,uint32_t version,uint32_t type,const struct BrookGpuWindowIdentity *identity){
    struct BrookGpuWindowPacket p={.magic=BROOK_GPU_WINDOW_WIRE_MAGIC,.version=version,.type=type};
    if(identity)p.identity=*identity;
    if((type!=BROOK_GPU_WINDOW_SAFE && type!=BROOK_GPU_WINDOW_PRESENTED) || !brook_gpu_window_packet_valid(&p)){errno=EINVAL;return -1;}
    ssize_t n=send(socket,&p,sizeof(p),MSG_DONTWAIT|MSG_NOSIGNAL);
    if(n==(ssize_t)sizeof(p))return 1;
    if(n<0 && (errno==EAGAIN || errno==EWOULDBLOCK || errno==EINTR))return 0;
    return -1;
}
static inline int brook_gpu_window_send_safe(int socket,const struct BrookGpuWindowIdentity *identity){
    struct BrookGpuWindowPacket p={.magic=BROOK_GPU_WINDOW_WIRE_MAGIC,.version=BROOK_GPU_WINDOW_WIRE_VERSION,.type=BROOK_GPU_WINDOW_SAFE};
    if(identity)p.identity=*identity;
    if(!brook_gpu_window_packet_valid(&p)){errno=EINVAL;return -1;}
    ssize_t n=send(socket,&p,sizeof(p),MSG_DONTWAIT|MSG_NOSIGNAL);
    if(n==(ssize_t)sizeof(p))return 1;
    if(n<0 && (errno==EAGAIN || errno==EWOULDBLOCK || errno==EINTR))return 0;
    return -1;
}
static inline int brook_gpu_window_receive(int socket,struct BrookGpuWindowPacket *p,int out[2]){
    if(!p || !out){errno=EINVAL;return -1;}out[0]=out[1]=-1;
    union {struct cmsghdr align;char data[CMSG_SPACE(8*sizeof(int))];} control={0};struct iovec io={p,sizeof(*p)};
    struct msghdr msg={.msg_iov=&io,.msg_iovlen=1,.msg_control=control.data,.msg_controllen=sizeof(control.data)};
    ssize_t n=recvmsg(socket,&msg,MSG_DONTWAIT|MSG_CMSG_CLOEXEC);
    if(n<0 && (errno==EAGAIN || errno==EWOULDBLOCK || errno==EINTR))return 0;
    if(n<=0)return -1;
    int fds[8],count=0,bad=0;
    for(struct cmsghdr *c=CMSG_FIRSTHDR(&msg);c;c=CMSG_NXTHDR(&msg,c)){
        if(c->cmsg_level!=SOL_SOCKET || c->cmsg_type!=SCM_RIGHTS || c->cmsg_len<CMSG_LEN(0) ||
           (c->cmsg_len-CMSG_LEN(0))%sizeof(int)){bad=1;continue;}
        size_t nfd=(c->cmsg_len-CMSG_LEN(0))/sizeof(int);const unsigned char *data=CMSG_DATA(c);
        for(size_t i=0;i<nfd;++i){int fd;memcpy(&fd,data+i*sizeof(int),sizeof(fd));if(count<8)fds[count++]=fd;else {close(fd);bad=1;}}
    }
    if(n!=(ssize_t)sizeof(*p) || (msg.msg_flags&(MSG_TRUNC|MSG_CTRUNC)) || bad || !brook_gpu_window_packet_valid(p) ||
       (p->type==BROOK_GPU_WINDOW_OFFER && p->version==BROOK_GPU_WINDOW_WIRE_VERSION_LAYERS) ||
       count!=(p->type==BROOK_GPU_WINDOW_OFFER?2:0) ||
       (count==2 && !brook_gpu_window_fd_matches(p,fds[0]))){
        for(int i=0;i<count;++i)close(fds[i]);
        errno=EPROTO;return -1;
    }
    if(count==2){out[0]=fds[0];out[1]=fds[1];}return 1;
}

/* ---- Version 3 layered offers ---------------------------------------- */
struct BrookGpuWindowLayer {
    uint64_t words,device,inode;              /* buffer size and identity */
    uint32_t offset,stride,width,height,fourcc,flags;
    uint32_t srcX,srcY,srcWidth,srcHeight;    /* buffer pixels */
    int32_t dstX,dstY;uint32_t dstWidth,dstHeight; /* window VFB pixels */
};
struct BrookGpuWindowLayersPacket {
    struct BrookGpuWindowPacket header;       /* type OFFER, version 3 */
    uint32_t count,reserved;
    struct BrookGpuWindowLayer layers[BROOK_GPU_WINDOW_LAYERS_MAX];
};
static inline int brook_gpu_window_layer_valid(const struct BrookGpuWindowLayer *l){
    if(!l || !l->words || l->words>UINT32_MAX || !l->width || !l->height || l->width>16384 || l->height>16384 ||
       l->stride<l->width || l->stride>16384 || (l->fourcc!=0x34325241 && l->fourcc!=0x34325258) ||
       (l->flags&~(BROOK_GPU_WINDOW_LAYER_OPAQUE|BROOK_GPU_WINDOW_LAYER_TILING)) || !l->inode ||
       (uint64_t)l->offset+(uint64_t)(l->height-1)*l->stride+l->width>l->words)return 0;
    if(l->flags&BROOK_GPU_WINDOW_LAYER_TILING){
        const int x=(l->flags&BROOK_GPU_WINDOW_LAYER_TILED_X)!=0;
        if((l->flags&BROOK_GPU_WINDOW_LAYER_TILING)==BROOK_GPU_WINDOW_LAYER_TILING || (l->offset&1023u) ||
           (l->stride&(x?127u:31u)))return 0;
        const uint64_t rows=((uint64_t)l->height+(x?7u:31u))&~(uint64_t)(x?7u:31u);
        if((uint64_t)l->offset+rows*l->stride>l->words)return 0;
    }
    if(!l->srcWidth || !l->srcHeight || (uint64_t)l->srcX+l->srcWidth>l->width || (uint64_t)l->srcY+l->srcHeight>l->height)return 0;
    return l->dstWidth && l->dstHeight && l->dstWidth<=16384 && l->dstHeight<=16384 &&
        l->dstX>-16384 && l->dstX<16384 && l->dstY>-16384 && l->dstY<16384;
}
static inline int brook_gpu_window_layers_valid(const struct BrookGpuWindowLayersPacket *p){
    if(!p || !brook_gpu_window_packet_valid(&p->header) || p->header.type!=BROOK_GPU_WINDOW_OFFER ||
       p->header.version!=BROOK_GPU_WINDOW_WIRE_VERSION_LAYERS || !p->count || p->count>BROOK_GPU_WINDOW_LAYERS_MAX || p->reserved)return 0;
    for(uint32_t i=0;i<p->count;++i)if(!brook_gpu_window_layer_valid(&p->layers[i]))return 0;
    for(uint32_t i=p->count;i<BROOK_GPU_WINDOW_LAYERS_MAX;++i){
        static const struct BrookGpuWindowLayer zero;if(memcmp(&p->layers[i],&zero,sizeof(zero)))return 0;}
    const struct BrookGpuWindowLayer *l0=&p->layers[0];const struct BrookGpuWindowPacket *h=&p->header;
    return l0->words==h->words && l0->offset==h->offset && l0->stride==h->stride && l0->width==h->width &&
        l0->height==h->height && l0->fourcc==h->fourcc && l0->device==h->identity.device && l0->inode==h->identity.inode;
}
static inline int brook_gpu_window_layer_fd_matches(const struct BrookGpuWindowLayer *l,int fd){
    struct stat s;return l && fd>=0 && !fstat(fd,&s) && s.st_ino && (uint64_t)s.st_dev==l->device &&
        (uint64_t)s.st_ino==l->inode && s.st_size>=0 && (uint64_t)s.st_size==l->words*4;
}
/* Sender retains all fds. buffers[i]/fences[i] for i < p->count. */
static inline int brook_gpu_window_send_layers(int socket,const struct BrookGpuWindowLayersPacket *p,const int *buffers,const int *fences){
    if(!brook_gpu_window_layers_valid(p) || !buffers || !fences){errno=EINVAL;return -1;}
    int fds[2*BROOK_GPU_WINDOW_LAYERS_MAX];
    for(uint32_t i=0;i<p->count;++i){
        if(!brook_gpu_window_layer_fd_matches(&p->layers[i],buffers[i]) || fences[i]<0){errno=EINVAL;return -1;}
        fds[2*i]=buffers[i];fds[2*i+1]=fences[i];
    }
    union {struct cmsghdr align;char data[CMSG_SPACE(sizeof(fds))];} control;memset(&control,0,sizeof(control));
    struct iovec io={(void *)p,sizeof(*p)};
    struct msghdr msg={.msg_iov=&io,.msg_iovlen=1,.msg_control=control.data,.msg_controllen=CMSG_SPACE(2*p->count*sizeof(int))};
    struct cmsghdr *c=CMSG_FIRSTHDR(&msg);c->cmsg_level=SOL_SOCKET;c->cmsg_type=SCM_RIGHTS;c->cmsg_len=CMSG_LEN(2*p->count*sizeof(int));
    memcpy(CMSG_DATA(c),fds,2*p->count*sizeof(int));
    ssize_t n=sendmsg(socket,&msg,MSG_DONTWAIT|MSG_NOSIGNAL);
    if(n==(ssize_t)sizeof(*p))return 1;
    if(n<0 && (errno==EAGAIN || errno==EWOULDBLOCK || errno==EINTR))return 0;
    return -1;
}
/* Receives a version 1/2 packet (out->count=0 on return for replies and
 * single offers, which also fill out->layers[0] for offers) or a version 3
 * layered offer. fds[2*i] / fds[2*i+1] are the buffer and fence of layer i;
 * *nfds is the number owned by the caller on success. */
static inline int brook_gpu_window_receive_layers(int socket,struct BrookGpuWindowLayersPacket *out,int fds[2*BROOK_GPU_WINDOW_LAYERS_MAX],unsigned *nfds){
    if(!out || !fds || !nfds){errno=EINVAL;return -1;}
    *nfds=0;memset(out,0,sizeof(*out));
    union {struct cmsghdr align;char data[CMSG_SPACE(2*BROOK_GPU_WINDOW_LAYERS_MAX*sizeof(int))];} control;memset(&control,0,sizeof(control));
    struct iovec io={out,sizeof(*out)};
    struct msghdr msg={.msg_iov=&io,.msg_iovlen=1,.msg_control=control.data,.msg_controllen=sizeof(control.data)};
    ssize_t n=recvmsg(socket,&msg,MSG_DONTWAIT|MSG_CMSG_CLOEXEC);
    if(n<0 && (errno==EAGAIN || errno==EWOULDBLOCK || errno==EINTR))return 0;
    if(n<=0)return -1;
    int got[2*BROOK_GPU_WINDOW_LAYERS_MAX];unsigned count=0;int bad=0;
    for(struct cmsghdr *c=CMSG_FIRSTHDR(&msg);c;c=CMSG_NXTHDR(&msg,c)){
        if(c->cmsg_level!=SOL_SOCKET || c->cmsg_type!=SCM_RIGHTS || c->cmsg_len<CMSG_LEN(0) ||
           (c->cmsg_len-CMSG_LEN(0))%sizeof(int)){bad=1;continue;}
        size_t nfd=(c->cmsg_len-CMSG_LEN(0))/sizeof(int);const unsigned char *data=CMSG_DATA(c);
        for(size_t i=0;i<nfd;++i){int fd;memcpy(&fd,data+i*sizeof(int),sizeof(fd));
            if(count<2*BROOK_GPU_WINDOW_LAYERS_MAX)got[count++]=fd;else {close(fd);bad=1;}}
    }
    int ok=!bad && !(msg.msg_flags&(MSG_TRUNC|MSG_CTRUNC));
    if(ok && n==(ssize_t)sizeof(out->header)){
        const struct BrookGpuWindowPacket *p=&out->header;
        ok=brook_gpu_window_packet_valid(p) && p->version!=BROOK_GPU_WINDOW_WIRE_VERSION_LAYERS &&
           count==(p->type==BROOK_GPU_WINDOW_OFFER?2u:0u) && (count!=2 || brook_gpu_window_fd_matches(p,got[0]));
        if(ok && count==2){
            out->layers[0]=(struct BrookGpuWindowLayer){p->words,p->identity.device,p->identity.inode,p->offset,p->stride,
                p->width,p->height,p->fourcc,0,0,0,p->width,p->height,0,0,p->width,p->height};
        }
    }else if(ok && n==(ssize_t)sizeof(*out)){
        ok=brook_gpu_window_layers_valid(out) && count==2*out->count;
        for(uint32_t i=0;ok && i<out->count;++i)ok=brook_gpu_window_layer_fd_matches(&out->layers[i],got[2*i]);
    }else ok=0;
    if(!ok){for(unsigned i=0;i<count;++i)close(got[i]);errno=EPROTO;return -1;}
    for(unsigned i=0;i<count;++i)fds[i]=got[i];
    *nfds=count;return 1;
}
