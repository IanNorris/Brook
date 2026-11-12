// Marker-gated, non-shadowed userspace render-queue smoke test.
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#define IOC(nr,size) ((3u<<30)|((unsigned)(size)<<16)|('d'<<8)|(nr))
#define CHECK(c) do {if(!(c)){fprintf(stderr,"INTEL_RENDER_DIRECT_FAIL line=%d errno=%d\n",__LINE__,errno);return 1;}} while(0)
struct getparam {int32_t param,pad;uint64_t value;};
struct create {uint64_t size;uint32_t handle,pad;};
struct map_offset {uint32_t handle,pad;uint64_t offset,flags,extensions;};
struct object {uint32_t handle,reloc_count;uint64_t relocs,alignment,offset,flags,reserved1,reserved2;};
struct exec {uint64_t objects;uint32_t count,start,length,dr1,dr4,clip_count;uint64_t clips,flags,context,fences;};
struct wait_gem {uint32_t handle,flags;int64_t timeout;};
int main(void){
    int fd=open("/dev/dri/renderD129",O_RDWR|O_CLOEXEC);CHECK(fd>=0);
    int cap=0;struct getparam gp={9,0,(uintptr_t)&cap};CHECK(!ioctl(fd,IOC(0x46,sizeof(gp)),&gp) && cap==1);
    struct create data={4096,0,0},batch={4096,0,0};
    CHECK(!ioctl(fd,IOC(0x5b,sizeof(data)),&data) && !ioctl(fd,IOC(0x5b,sizeof(batch)),&batch));
    struct map_offset dm={data.handle,0,0,2,0},bm={batch.handle,0,0,2,0};
    CHECK(!ioctl(fd,IOC(0x64,sizeof(dm)),&dm) && !ioctl(fd,IOC(0x64,sizeof(bm)),&bm));
    volatile uint32_t* dst=mmap(0,4096,PROT_READ|PROT_WRITE,MAP_SHARED,fd,dm.offset);
    volatile uint32_t* cmd=mmap(0,4096,PROT_READ|PROT_WRITE,MAP_SHARED,fd,bm.offset);
    CHECK(dst!=MAP_FAILED && cmd!=MAP_FAILED);*dst=0;
    cmd[0]=0x10000002;cmd[1]=0x40011000;cmd[2]=0;cmd[3]=0x44495245;cmd[4]=0x05000000;cmd[5]=0;
    struct object objects[]={{data.handle,0,0,4096,0x40011000,20,0,0},{batch.handle,0,0,4096,0x10000,16,0,0}};
    struct exec request={(uintptr_t)objects,2,0,24,0,0,0,0,1u<<11,0,0};
    CHECK(!ioctl(fd,IOC(0x69,sizeof(request)),&request));
    struct wait_gem wait={data.handle,0,5000000000ll};CHECK(!ioctl(fd,IOC(0x6c,sizeof(wait)),&wait));
    CHECK(*dst==0x44495245);
    CHECK(!munmap((void*)cmd,4096) && !munmap((void*)dst,4096) && !close(fd));
    puts("INTEL_RENDER_DIRECT_PASS");return 0;
}
