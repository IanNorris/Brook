#define _GNU_SOURCE
#include "gpu_buffer_abi.h"
#include "gpu_output_abi.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#define IOC(n,s) ((3u<<30)|((unsigned)(s)<<16)|('d'<<8)|(n))
#define CHECK(x) do {if(!(x)){fprintf(stderr,"GPU_TRY_CPU_FAIL line=%d errno=%d\n",__LINE__,errno);return 1;}} while(0)
struct create {uint64_t size;uint32_t handle,pad;};
struct prime {uint32_t handle,flags;int32_t fd;};
struct gem_close {uint32_t handle,pad;};
struct map_offset {uint32_t handle,pad;uint64_t offset,flags,extensions;};
struct object {uint32_t handle,reloc_count;uint64_t relocs,alignment,offset,flags,reserved1,reserved2;};
struct exec {uint64_t objects;uint32_t count,start,length,dr1,dr4,clip_count;uint64_t clips,flags,context,fences;};
struct wait_gem {uint32_t handle,flags;int64_t timeout;};
int main(void){
    int drm=open("/dev/dri/renderD129",O_RDWR|O_CLOEXEC);CHECK(drm>=0);
    errno=0;CHECK(syscall(BROOK_SYS_GPU_BUFFER_TRY_CPU_READ,-1L,0L,0L,0L,0L,0L)==-1 && errno==EBADF);
    errno=0;CHECK(syscall(BROOK_SYS_GPU_BUFFER_TRY_CPU_READ,(long)drm,0L,0L,0L,0L,0L)==-1 && errno==ENOTSUP);
    struct create b={8192,0,0};CHECK(!ioctl(drm,IOC(0x5b,sizeof(b)),&b));
    struct prime p={b.handle,O_CLOEXEC|O_RDWR,-1};CHECK(!ioctl(drm,IOC(0x2d,sizeof(p)),&p) && p.fd>=0);
    errno=0;CHECK(syscall(BROOK_SYS_GPU_BUFFER_TRY_CPU_READ,(long)p.fd,1L,0L,0L,0L,0L)==-1 && errno==EINVAL);
    CHECK(!syscall(BROOK_SYS_GPU_BUFFER_TRY_CPU_READ,(long)p.fd,0L,0L,0L,0L,0L));
    struct create batch={4096,0,0};CHECK(!ioctl(drm,IOC(0x5b,sizeof(batch)),&batch));
    struct map_offset map={batch.handle,0,0,2,0};CHECK(!ioctl(drm,IOC(0x64,sizeof(map)),&map));
    uint32_t *commands=mmap(NULL,batch.size,PROT_READ|PROT_WRITE,MAP_SHARED,drm,(off_t)map.offset);CHECK(commands!=MAP_FAILED);
    commands[0]=0x05000000;commands[1]=0;
    struct object objects[]={{batch.handle,0,0,4096,0x10000,16,0,0},{b.handle,0,0,4096,0x20000,20,0,0}};
    struct exec request={(uintptr_t)objects,2,0,8,0,0,0,0,(1u<<11)|(1u<<18),0,0};
    unsigned busy=0;
    for(unsigned i=0;i<8 && !busy;++i){
        CHECK(!ioctl(drm,IOC(0x69,sizeof(request)),&request));
        errno=0;long status=syscall(BROOK_SYS_GPU_BUFFER_TRY_CPU_READ,(long)p.fd,0L,0L,0L,0L,0L);
        CHECK(status==0 || (status==-1 && errno==EAGAIN));if(status==-1)++busy;
        struct wait_gem wait={b.handle,0,5000000000ll};CHECK(!ioctl(drm,IOC(0x6c,sizeof(wait)),&wait));
        CHECK(!syscall(BROOK_SYS_GPU_BUFFER_TRY_CPU_READ,(long)p.fd,0L,0L,0L,0L,0L));
    }
    CHECK(busy);CHECK(!munmap(commands,batch.size));
    struct gem_close batchClose={batch.handle,0};CHECK(!ioctl(drm,IOC(9,sizeof(batchClose)),&batchClose));
    int protectedFd=(int)syscall(BROOK_SYS_GPU_OUTPUT_ALLOC,4096L,0L,0L,0L,0L,0L);CHECK(protectedFd>=0);
    errno=0;CHECK(syscall(BROOK_SYS_GPU_BUFFER_TRY_CPU_READ,(long)protectedFd,0L,0L,0L,0L,0L)==-1 && errno==EACCES);
    CHECK(!close(protectedFd) && !close(p.fd));struct gem_close c={b.handle,0};CHECK(!ioctl(drm,IOC(9,sizeof(c)),&c));
    CHECK(!close(drm));puts("GPU_TRY_CPU_PASS idle/busy/retired/invalid/unsupported/protected");return 0;
}
