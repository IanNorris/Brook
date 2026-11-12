// Opt-in fixed diagnostic. This is not a general-purpose i915 3D test.
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#define IOC(nr, size) ((3u<<30)|((unsigned)(size)<<16)|('d'<<8)|(nr))
#define CHECK(c) do {if(!(c)){fprintf(stderr,"INTEL_EXEC_STORE_FAIL line=%d errno=%d\n",__LINE__,errno);return 1;}} while(0)
struct create {uint64_t size;uint32_t handle,pad;};
struct context {uint32_t id,pad;};
struct map_offset {uint32_t handle,pad;uint64_t offset,flags,extensions;};
struct object {uint32_t handle,reloc_count;uint64_t relocs,alignment,offset,flags,reserved1,reserved2;};
struct exec {uint64_t objects;uint32_t count,start,length,dr1,dr4,clip_count;uint64_t clips,flags,context,fences;};
struct wait_gem {uint32_t handle,flags;int64_t timeout;};
struct domain {uint32_t handle,read,write;};
struct sync_object {uint32_t handle,flags;};
struct exec_fence {uint32_t handle,flags;};
struct sync_wait {uint64_t handles;int64_t timeout;uint32_t count,flags,first,pad;uint64_t deadline;};
int main(void){
    int fd=open("/dev/dri/renderD129",O_RDWR|O_CLOEXEC);CHECK(fd>=0);
    struct context ctx={0};CHECK(!ioctl(fd,IOC(0x6d,sizeof(ctx)),&ctx) && ctx.id);
    struct create data={4096,0,0},batch={4096,0,0};
    CHECK(!ioctl(fd,IOC(0x5b,sizeof(data)),&data) && !ioctl(fd,IOC(0x5b,sizeof(batch)),&batch));
    struct map_offset dm={data.handle,0,0,2,0},bm={batch.handle,0,0,2,0};
    CHECK(!ioctl(fd,IOC(0x64,sizeof(dm)),&dm) && !ioctl(fd,IOC(0x64,sizeof(bm)),&bm));
    volatile uint32_t* dst=mmap(0,4096,PROT_READ|PROT_WRITE,MAP_SHARED,fd,dm.offset);
    volatile uint32_t* cmd=mmap(0,4096,PROT_READ|PROT_WRITE,MAP_SHARED,fd,bm.offset);
    CHECK(dst!=MAP_FAILED && cmd!=MAP_FAILED);*dst=0;
    cmd[0]=0x10000002;cmd[1]=0x40011000;cmd[2]=0;cmd[3]=0x53544f52;cmd[4]=0x05000000;cmd[5]=0;
    struct object objects[2]={{data.handle,0,0,4096,0x40011000,20,0,0},{batch.handle,0,0,4096,0x10000,16,0,0}};
    struct exec request={(uintptr_t)objects,2,0,24,0,0,0,0,1u<<11,ctx.id,0};
    cmd[1]=0x10000;errno=0;CHECK(ioctl(fd,IOC(0x69,sizeof(request)),&request)==-1 && errno==EINVAL);
    cmd[1]=0x40011000;CHECK(!ioctl(fd,IOC(0x69,sizeof(request)),&request));
    // The GPU must see the checked shadow rather than these later writes.
    cmd[0]=0;cmd[1]=0;cmd[3]=0;
    struct domain domain={data.handle,1,0};CHECK(!ioctl(fd,IOC(0x5f,sizeof(domain)),&domain));
    struct wait_gem wait={data.handle,0,5000000000ll};CHECK(!ioctl(fd,IOC(0x6c,sizeof(wait)),&wait));
    CHECK(*dst==0x53544f52);
    // Mesa also uses handle LUT, batch-first and async object hints. The
    // single worker serializes the hint while preserving its stronger order.
    cmd[0]=0x10000002;cmd[1]=0x40011000;cmd[2]=0;cmd[3]=0x53544f53;cmd[4]=0x05000000;cmd[5]=0;
    struct object first=objects[0];objects[0]=objects[1];objects[1]=first;
    objects[0].flags|=64;objects[1].flags|=64;
    struct sync_object input={0,1},output={0,0};
    CHECK(!ioctl(fd,IOC(0xbf,sizeof(input)),&input) && !ioctl(fd,IOC(0xbf,sizeof(output)),&output));
    struct exec_fence fences[]={{input.handle,1},{output.handle,2}};
    request.clip_count=2;request.clips=(uintptr_t)fences;
    request.flags=(1u<<11)|(1u<<12)|(1u<<18)|(1u<<19);wait.timeout=5000000000ll;
    CHECK(!ioctl(fd,IOC(0x69,sizeof(request)),&request));
    struct timespec now;CHECK(!clock_gettime(CLOCK_MONOTONIC,&now));
    uint32_t output_handle=output.handle;
    struct sync_wait sw={(uintptr_t)&output_handle,(int64_t)now.tv_sec*1000000000ll+now.tv_nsec+5000000000ll,1,0,0,0,0};
    CHECK(!ioctl(fd,IOC(0xc3,sizeof(sw)),&sw));
    CHECK(!ioctl(fd,IOC(0x6c,sizeof(wait)),&wait) && *dst==0x53544f53);
    input.flags=0;CHECK(!ioctl(fd,IOC(0xc0,sizeof(input)),&input) && !ioctl(fd,IOC(0xc0,sizeof(output)),&output));
    CHECK(!ioctl(fd,IOC(0x6e,sizeof(ctx)),&ctx));
    CHECK(!munmap((void*)cmd,4096) && !munmap((void*)dst,4096) && !close(fd));
    puts("INTEL_EXEC_STORE_PASS");return 0;
}
