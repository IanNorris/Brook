#define _GNU_SOURCE
#include "gpu_window_channel.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
int main(void){
    int socket[2],pipefd[2];assert(!socketpair(AF_UNIX,SOCK_SEQPACKET,0,socket) && !pipe(pipefd));
    char name[]="/tmp/brook-gpu-channel-XXXXXX";int source=mkstemp(name);assert(source>=0);unlink(name);assert(!ftruncate(source,4096));
    struct stat s;assert(!fstat(source,&s));
    struct BrookGpuWindowPacket offer={.magic=BROOK_GPU_WINDOW_WIRE_MAGIC,.version=BROOK_GPU_WINDOW_WIRE_VERSION,
      .type=BROOK_GPU_WINDOW_OFFER,.identity={2,0,99,3,(uint64_t)s.st_dev,(uint64_t)s.st_ino},
      .words=1024,.stride=16,.width=16,.height=16,.fourcc=0x34325258};
    struct BrookGpuWindowPacket received={0};int fds[2];
    assert(brook_gpu_window_receive(socket[1],&received,fds)==0);
    assert(brook_gpu_window_send_offer(socket[0],&offer,source,pipefd[0])==1);
    assert(brook_gpu_window_receive(socket[1],&received,fds)==1 && brook_gpu_window_identity_equal(&offer.identity,&received.identity));
    assert((fcntl(fds[0],F_GETFD)&FD_CLOEXEC) && (fcntl(fds[1],F_GETFD)&FD_CLOEXEC));
    close(fds[0]);close(fds[1]);
    struct BrookGpuWindowIdentity wrong=offer.identity;++wrong.commit;
    assert(brook_gpu_window_send_safe(socket[1],&wrong)==1 && brook_gpu_window_receive(socket[0],&received,fds)==1);
    assert(received.type==BROOK_GPU_WINDOW_SAFE && !brook_gpu_window_identity_equal(&offer.identity,&received.identity));
    assert(brook_gpu_window_send_safe(socket[1],&offer.identity)==1 && brook_gpu_window_receive(socket[0],&received,fds)==1);
    assert(brook_gpu_window_identity_equal(&offer.identity,&received.identity));
    struct BrookGpuWindowPacket invalid=offer;invalid.stride=1;assert(brook_gpu_window_send_offer(socket[0],&invalid,source,pipefd[0])==-1);
    invalid=offer;++invalid.identity.inode;assert(brook_gpu_window_send_offer(socket[0],&invalid,source,pipefd[0])==-1);
    invalid=offer;invalid.offset=1024;assert(brook_gpu_window_send_offer(socket[0],&invalid,source,pipefd[0])==-1);
    union {struct cmsghdr align;char data[CMSG_SPACE(sizeof(int))];} control={0};struct iovec io={&offer,sizeof(offer)};
    struct msghdr msg={.msg_iov=&io,.msg_iovlen=1,.msg_control=control.data,.msg_controllen=sizeof(control.data)};
    struct cmsghdr *c=CMSG_FIRSTHDR(&msg);c->cmsg_level=SOL_SOCKET;c->cmsg_type=SCM_RIGHTS;c->cmsg_len=CMSG_LEN(sizeof(int));memcpy(CMSG_DATA(c),&source,sizeof(source));
    assert(sendmsg(socket[0],&msg,0)==sizeof(offer));assert(brook_gpu_window_receive(socket[1],&received,fds)==-1 && errno==EPROTO);
    close(source);close(pipefd[0]);close(pipefd[1]);close(socket[0]);close(socket[1]);puts("GPU_WINDOW_CHANNEL_PASS");return 0;
}
