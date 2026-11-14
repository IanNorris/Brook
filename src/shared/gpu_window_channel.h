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
       (p->version!=BROOK_GPU_WINDOW_WIRE_VERSION && p->version!=BROOK_GPU_WINDOW_WIRE_VERSION_RETAIN) ||
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
       count!=(p->type==BROOK_GPU_WINDOW_OFFER?2:0) ||
       (count==2 && !brook_gpu_window_fd_matches(p,fds[0]))){
        for(int i=0;i<count;++i)close(fds[i]);
        errno=EPROTO;return -1;
    }
    if(count==2){out[0]=fds[0];out[1]=fds[1];}return 1;
}
