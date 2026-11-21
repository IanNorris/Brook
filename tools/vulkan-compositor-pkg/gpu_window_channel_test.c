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
    /* Version 2 replies: PRESENTED is valid only in version 2. */
    assert(brook_gpu_window_send_reply(socket[1],BROOK_GPU_WINDOW_WIRE_VERSION,BROOK_GPU_WINDOW_PRESENTED,&offer.identity)==-1);
    assert(brook_gpu_window_send_reply(socket[1],BROOK_GPU_WINDOW_WIRE_VERSION_RETAIN,BROOK_GPU_WINDOW_PRESENTED,&offer.identity)==1);
    assert(brook_gpu_window_receive(socket[0],&received,fds)==1 && received.type==BROOK_GPU_WINDOW_PRESENTED &&
           received.version==BROOK_GPU_WINDOW_WIRE_VERSION_RETAIN && brook_gpu_window_identity_equal(&offer.identity,&received.identity));
    assert(brook_gpu_window_send_reply(socket[1],4,BROOK_GPU_WINDOW_SAFE,&offer.identity)==-1);
    struct BrookGpuWindowPacket retained=offer;retained.version=BROOK_GPU_WINDOW_WIRE_VERSION_RETAIN;
    assert(brook_gpu_window_send_offer(socket[0],&retained,source,pipefd[0])==1);
    assert(brook_gpu_window_receive(socket[1],&received,fds)==1 && received.version==BROOK_GPU_WINDOW_WIRE_VERSION_RETAIN);
    close(fds[0]);close(fds[1]);
    struct BrookGpuWindowPacket invalid=offer;invalid.stride=1;assert(brook_gpu_window_send_offer(socket[0],&invalid,source,pipefd[0])==BROOK_GPU_WINDOW_SEND_REJECTED);
    invalid=offer;++invalid.identity.inode;assert(brook_gpu_window_send_offer(socket[0],&invalid,source,pipefd[0])==BROOK_GPU_WINDOW_SEND_REJECTED);
    invalid=offer;invalid.offset=1024;assert(brook_gpu_window_send_offer(socket[0],&invalid,source,pipefd[0])==BROOK_GPU_WINDOW_SEND_REJECTED);
    union {struct cmsghdr align;char data[CMSG_SPACE(sizeof(int))];} control={0};struct iovec io={&offer,sizeof(offer)};
    struct msghdr msg={.msg_iov=&io,.msg_iovlen=1,.msg_control=control.data,.msg_controllen=sizeof(control.data)};
    struct cmsghdr *c=CMSG_FIRSTHDR(&msg);c->cmsg_level=SOL_SOCKET;c->cmsg_type=SCM_RIGHTS;c->cmsg_len=CMSG_LEN(sizeof(int));memcpy(CMSG_DATA(c),&source,sizeof(source));
    assert(sendmsg(socket[0],&msg,0)==sizeof(offer));assert(brook_gpu_window_receive(socket[1],&received,fds)==-1 && errno==EPROTO);
    /* Version 3: a layered offer of two buffers, then replies in version 3. */
    char name2[]="/tmp/brook-gpu-channel-XXXXXX";int second=mkstemp(name2);assert(second>=0);unlink(name2);assert(!ftruncate(second,8*8*4));
    struct stat s2;assert(!fstat(second,&s2));
    struct BrookGpuWindowLayersPacket layered;memset(&layered,0,sizeof(layered));
    layered.header=offer;layered.header.version=BROOK_GPU_WINDOW_WIRE_VERSION_LAYERS;layered.count=2;
    layered.layers[0]=(struct BrookGpuWindowLayer){1024,(uint64_t)s.st_dev,(uint64_t)s.st_ino,0,16,16,16,0x34325241,0,0,0,16,16,0,0,16,16};
    layered.layers[1]=(struct BrookGpuWindowLayer){64,(uint64_t)s2.st_dev,(uint64_t)s2.st_ino,0,8,8,8,0x34325258,BROOK_GPU_WINDOW_LAYER_OPAQUE,
        1,1,6,6,4,5,12,12};
    layered.header.fourcc=0x34325241;
    int buffers[2]={source,second},fences[2]={pipefd[0],pipefd[0]};
    assert(brook_gpu_window_send_layers(socket[0],&layered,buffers,fences)==1);
    struct BrookGpuWindowLayersPacket got;int lfds[16];unsigned nfds=0;
    assert(brook_gpu_window_receive_layers(socket[1],&got,lfds,&nfds)==1 && nfds==4 && got.count==2 &&
           got.layers[1].dstX==4 && got.layers[1].srcWidth==6 && brook_gpu_window_identity_equal(&got.header.identity,&offer.identity));
    for(unsigned i=0;i<nfds;++i){assert(fcntl(lfds[i],F_GETFD)&FD_CLOEXEC);close(lfds[i]);}
    /* The old receive refuses a layered offer (an old renderer drops the channel rather than misreading it). */
    assert(brook_gpu_window_send_layers(socket[0],&layered,buffers,fences)==1);
    assert(brook_gpu_window_receive(socket[1],&received,fds)==-1 && errno==EPROTO);
    /* A version 2 single offer through the new receive fills layer 0. */
    assert(brook_gpu_window_send_offer(socket[0],&retained,source,pipefd[0])==1);
    assert(brook_gpu_window_receive_layers(socket[1],&got,lfds,&nfds)==1 && nfds==2 && got.count==0 &&
           got.layers[0].width==16 && got.layers[0].srcWidth==16 && got.layers[0].dstWidth==16);
    close(lfds[0]);close(lfds[1]);
    assert(brook_gpu_window_send_reply(socket[1],BROOK_GPU_WINDOW_WIRE_VERSION_LAYERS,BROOK_GPU_WINDOW_PRESENTED,&offer.identity)==1);
    assert(brook_gpu_window_receive(socket[0],&received,fds)==1 && received.version==BROOK_GPU_WINDOW_WIRE_VERSION_LAYERS);
    /* Invalid layered offers are refused before sending. */
    struct BrookGpuWindowLayersPacket bad=layered;bad.layers[1].srcX=4;assert(brook_gpu_window_send_layers(socket[0],&bad,buffers,fences)==BROOK_GPU_WINDOW_SEND_REJECTED);
    bad=layered;bad.count=9;assert(brook_gpu_window_send_layers(socket[0],&bad,buffers,fences)==BROOK_GPU_WINDOW_SEND_REJECTED);
    bad=layered;bad.layers[0].stride=32;assert(brook_gpu_window_send_layers(socket[0],&bad,buffers,fences)==BROOK_GPU_WINDOW_SEND_REJECTED);
    bad=layered;bad.layers[1].flags=2;assert(brook_gpu_window_send_layers(socket[0],&bad,buffers,fences)==BROOK_GPU_WINDOW_SEND_REJECTED);
    int swapped[2]={second,source};assert(brook_gpu_window_send_layers(socket[0],&layered,swapped,fences)==BROOK_GPU_WINDOW_SEND_REJECTED);
    assert(brook_gpu_window_receive(socket[1],&received,fds)==0);
    assert(brook_gpu_window_send_offer(socket[0],&retained,source,pipefd[0])==1);
    assert(brook_gpu_window_receive(socket[1],&received,fds)==1);
    close(fds[0]);close(fds[1]);
    close(second);
    close(source);close(pipefd[0]);close(pipefd[1]);close(socket[0]);close(socket[1]);puts("GPU_WINDOW_CHANNEL_PASS");return 0;
}
