/* Cross-process producer ownership: pass the actual DMA-BUF and pending
 * completion fence, and acknowledge only after Vulkan has finished reading. */
#define _GNU_SOURCE
#include "renderer.h"
#include "compositor_probe.h"
#include "gpu_buffer_abi.h"
#include "gpu_window_channel.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#define IOC(n,s) ((3u<<30)|((unsigned)(s)<<16)|('d'<<8)|(n))
#define CHECK(c) do {if(!(c)){fprintf(stderr,"CROSS_PROCESS_FAIL line=%d errno=%d\n",__LINE__,errno);return 1;}} while(0)
struct create {uint64_t size;uint32_t handle,pad;};
struct map_offset {uint32_t handle,pad;uint64_t offset,flags,extensions;};
struct prime {uint32_t handle,flags;int32_t fd;};
struct gem_close {uint32_t handle,pad;};
struct context {uint32_t id,pad;};
struct object {uint32_t handle,reloc_count;uint64_t relocs,alignment,offset,flags,reserved1,reserved2;};
struct exec {uint64_t objects;uint32_t count,start,length,dr1,dr4,clip_count;uint64_t clips,flags,context,fences;};
struct sync_object {uint32_t handle,flags;};
struct exec_fence {uint32_t handle,flags;};
struct sync_handle {uint32_t handle,flags;int32_t fd;uint32_t pad;};
static void uncertain(const char *why){fprintf(stderr,"CROSS_PROCESS_UNCERTAIN %s resources retained\n",why);for(;;)pause();}
static int snapshot_fd(int fd){
    for(unsigned tries=0;tries<100;++tries){
        int result=(int)syscall(BROOK_SYS_GPU_BUFFER_SNAPSHOT_FENCE,(long)fd,0L,0L,0L,0L,0L);
        if(result>=0 || errno!=EAGAIN)return result;
        usleep(1000);
    }
    return -1;
}
static int receive_packet(int socket,struct BrookGpuWindowPacket *p,int fds[2]){
    for(;;){int rc=brook_gpu_window_receive(socket,p,fds);if(rc)return rc;
        struct pollfd pending={.fd=socket,.events=POLLIN};if(poll(&pending,1,10000)!=1)return -1;}
}
static int consume(int socket){
    BrookVkRenderer *renderer=brook_vk_renderer_create_external(16384,0);CHECK(renderer);
    fprintf(stderr,"CROSS_PROCESS_DEVICE %s\n",brook_vk_renderer_device(renderer));
    for(unsigned char expected=0;expected<3;++expected){
        int fds[2]={-1,-1};struct BrookGpuWindowPacket offer={0};
        CHECK(receive_packet(socket,&offer,fds)==1 && offer.type==BROOK_GPU_WINDOW_OFFER && offer.identity.commit==(uint64_t)expected+1);
        unsigned char round=expected;
        uint32_t scene[16384],pixels[4096];compositor_probe_prepare(scene,16384,round);scene[64]=0xff123456;
        struct pollfd f={.fd=fds[1],.events=POLLIN};int initially=poll(&f,1,0);CHECK(initially>=0);
        BrookVkResult status=brook_vk_renderer_compose_external_sync(renderer,scene,16384,fds[0],16384,fds[1],pixels,4096,10000000000ULL);
        if(status!=BROOK_VK_COMPLETE)uncertain("consumer completion");
        for(uint32_t i=0;i<4096;++i)if(pixels[i]!=compositor_probe_pixel(scene,i%64,i/64))uncertain("pixel mismatch after submission");
        close(fds[0]);close(fds[1]);
        if(brook_gpu_window_send_safe(socket,&offer.identity)!=1)uncertain("acknowledgement");
        fprintf(stderr,"CROSS_PROCESS_ROUND round=%u initially_pending=%d\n",round,initially==0);
    }
    CHECK(brook_vk_renderer_destroy(renderer));close(socket);return 0;
}
int main(void){
    const int snapshotFence=getenv("BROOK_SNAPSHOT_FENCE")!=NULL;
    int pair[2];CHECK(!socketpair(AF_UNIX,SOCK_SEQPACKET,0,pair));pid_t child=fork();CHECK(child>=0);
    if(!child){close(pair[0]);_exit(consume(pair[1]));}close(pair[1]);
    int drm=open("/dev/dri/renderD129",O_RDWR|O_CLOEXEC);CHECK(drm>=0);
    struct create gem={65536,0,0},batch={4096,0,0};CHECK(!ioctl(drm,IOC(0x5b,sizeof(gem)),&gem));
    struct map_offset map={gem.handle,0,0,2,0};CHECK(!ioctl(drm,IOC(0x64,sizeof(map)),&map));
    uint32_t *source=mmap(NULL,65536,PROT_READ|PROT_WRITE,MAP_SHARED,drm,(off_t)map.offset);CHECK(source!=MAP_FAILED);
    struct prime shared={gem.handle,O_CLOEXEC|O_RDWR,-1};CHECK(!ioctl(drm,IOC(0x2d,sizeof(shared)),&shared) && shared.fd>=0);
    struct stat sourceStat;CHECK(!fstat(shared.fd,&sourceStat) && sourceStat.st_ino);
    if(snapshotFence){
        errno=0;CHECK(syscall(BROOK_SYS_GPU_BUFFER_SNAPSHOT_FENCE,-1L,0L,0L,0L,0L,0L)==-1 && errno==EBADF);
        errno=0;CHECK(syscall(BROOK_SYS_GPU_BUFFER_SNAPSHOT_FENCE,(long)drm,0L,0L,0L,0L,0L)==-1 && errno==ENOTSUP);
        errno=0;CHECK(syscall(BROOK_SYS_GPU_BUFFER_SNAPSHOT_FENCE,(long)shared.fd,1L,0L,0L,0L,0L)==-1 && errno==EINVAL);
        int idle=snapshot_fd(shared.fd);CHECK(idle>=0);
        struct pollfd idlePoll={.fd=idle,.events=POLLIN};CHECK((fcntl(idle,F_GETFD)&FD_CLOEXEC) && poll(&idlePoll,1,0)==1 && (idlePoll.revents&POLLIN));
        CHECK(!close(idle));
    }
    struct context ctx={0};CHECK(!ioctl(drm,IOC(0x6d,sizeof(ctx)),&ctx) && ctx.id);
    CHECK(!ioctl(drm,IOC(0x5b,sizeof(batch)),&batch));struct map_offset bm={batch.handle,0,0,2,0};CHECK(!ioctl(drm,IOC(0x64,sizeof(bm)),&bm));
    uint32_t *cmd=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED,drm,(off_t)bm.offset);CHECK(cmd!=MAP_FAILED);
    for(unsigned char round=0;round<3;++round){
        uint32_t scene[16384];compositor_probe_prepare(scene,16384,round);memcpy(source,scene,65536);
        struct sync_object sync={0,0};CHECK(!ioctl(drm,IOC(0xbf,sizeof(sync)),&sync) && sync.handle);
        cmd[0]=0x10000002;cmd[1]=0x40011100;cmd[2]=0;cmd[3]=0xff123456;cmd[4]=0x05000000;cmd[5]=0;
        struct object objects[]={{gem.handle,0,0,4096,0x40011000,20,0,0},{batch.handle,0,0,4096,0x10000,16,0,0}};
        struct exec_fence ef={sync.handle,2};struct exec request={(uintptr_t)objects,2,0,24,0,0,
            snapshotFence?0u:1u,snapshotFence?0u:(uintptr_t)&ef,(1u<<11)|(snapshotFence?0u:(1u<<19)),ctx.id,0};
        if(ioctl(drm,IOC(0x69,sizeof(request)),&request))uncertain("producer submission");
        struct sync_handle fence={sync.handle,1,-1,0};
        if(snapshotFence){
            fence.fd=snapshot_fd(shared.fd);
            if(fence.fd<0)uncertain("snapshot fence");
        }else if(ioctl(drm,IOC(0xc1,sizeof(fence)),&fence) || fence.fd<0)uncertain("producer fence");
        struct BrookGpuWindowPacket offer={.magic=BROOK_GPU_WINDOW_WIRE_MAGIC,.version=BROOK_GPU_WINDOW_WIRE_VERSION,
            .type=BROOK_GPU_WINDOW_OFFER,.identity={1,0,1,(uint64_t)round+1,(uint64_t)sourceStat.st_dev,(uint64_t)sourceStat.st_ino},
            .words=16384,.offset=64,.stride=64,.width=64,.height=64,.fourcc=0x34325258};
        if(brook_gpu_window_send_offer(pair[0],&offer,shared.fd,fence.fd)!=1)uncertain("transfer");
        struct BrookGpuWindowPacket ack={0};int ackFds[2];
        if(receive_packet(pair[0],&ack,ackFds)!=1 || ack.type!=BROOK_GPU_WINDOW_SAFE ||
           !brook_gpu_window_identity_equal(&offer.identity,&ack.identity))uncertain("consumer acknowledgement");
        close(fence.fd);CHECK(!ioctl(drm,IOC(0xc0,sizeof(sync)),&sync));
    }
    close(pair[0]);int status=0;CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
    close(shared.fd);munmap(source,65536);munmap(cmd,4096);
    struct gem_close bc={batch.handle,0},gc={gem.handle,0};CHECK(!ioctl(drm,IOC(9,sizeof(bc)),&bc));
    CHECK(!ioctl(drm,IOC(0x6e,sizeof(ctx)),&ctx));CHECK(!ioctl(drm,IOC(9,sizeof(gc)),&gc));close(drm);
    puts(snapshotFence?"CROSS_PROCESS_SNAPSHOT_FENCE_PASS":"CROSS_PROCESS_VULKAN_HANDOFF_PASS");return 0;
}
