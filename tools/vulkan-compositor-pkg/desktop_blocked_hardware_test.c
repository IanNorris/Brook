/* Test-kernel-only fixture: a synthetic unsignaled dependency ensures the
 * protected-output request cannot reach the GPU. Run without another handoff
 * owner; the parent verifies owner-exit cleanup and output reuse. */
#define _GNU_SOURCE
#include "gpu_output_abi.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#define IOC(n,s) ((3u<<30)|((unsigned)(s)<<16)|('d'<<8)|(n))
struct create {uint64_t size;uint32_t handle,pad;};
struct map_offset {uint32_t handle,pad;uint64_t offset,flags,extensions;};
struct prime {uint32_t handle,flags;int32_t fd;};
struct sync_object {uint32_t handle,flags;};
struct object {uint32_t handle,reloc_count;uint64_t relocs,alignment,offset,flags,reserved1,reserved2;};
struct fence {uint32_t handle,flags;};
struct exec {uint64_t objects;uint32_t count,start,length,dr1,dr4,clip_count;uint64_t clips,flags,context,fences;};
#define CHILD(c,n) do{if(!(c)){dprintf(2,"BLOCKED_FIXTURE_CHILD_FAIL step=%d errno=%d\n",n,errno);_exit(n);}}while(0)
#define CHECK(c) do{if(!(c)){fprintf(stderr,"BLOCKED_FIXTURE_FAIL line=%d errno=%d\n",__LINE__,errno);return 1;}}while(0)
int main(void){
    int drm=open("/dev/dri/renderD129",O_RDWR|O_CLOEXEC);CHECK(drm>=0);
    int output=(int)syscall(BROOK_SYS_GPU_OUTPUT_ALLOC,4096,0,0,0,0,0);CHECK(output>=0);
    int output2=(int)syscall(BROOK_SYS_GPU_OUTPUT_ALLOC,4096,0,0,0,0,0);CHECK(output2>=0);
    pid_t child=fork();CHECK(child>=0);
    if(!child){
        struct prime imported={0,0,output};CHILD(!ioctl(drm,IOC(0x2e,sizeof(imported)),&imported) && imported.handle,10);
        struct prime imported2={0,0,output2};CHILD(!ioctl(drm,IOC(0x2e,sizeof(imported2)),&imported2) && imported2.handle,22);
        struct create batch={4096,0,0};CHILD(!ioctl(drm,IOC(0x5b,sizeof(batch)),&batch),11);
        struct map_offset map={batch.handle,0,0,2,0};CHILD(!ioctl(drm,IOC(0x64,sizeof(map)),&map),12);
        uint32_t *commands=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED,drm,(off_t)map.offset);CHILD(commands!=MAP_FAILED,13);
        commands[0]=0x05000000;commands[1]=0;
        struct sync_object wait={0,0},signal={0,0},signal2={0,0};
        CHILD(!ioctl(drm,IOC(0xbf,sizeof(wait)),&wait) && !ioctl(drm,IOC(0xbf,sizeof(signal)),&signal) &&
              !ioctl(drm,IOC(0xbf,sizeof(signal2)),&signal2),14);
        wait.flags=0x42524b54u;CHILD(!ioctl(drm,IOC(0xee,sizeof(wait)),&wait),15);
        CHILD(!syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_ATTACH,0,0,0,0,0),16);
        long ticket=syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_RESERVE,output,0,0,0,0);CHILD(ticket>0,17);
        struct object objects[]={{batch.handle,0,0,4096,0x10000,16,0,0},{imported.handle,0,0,4096,0x20000,20,0,0}};
        struct fence fences[]={{wait.handle,1},{signal.handle,2}};
        struct exec request={(uintptr_t)objects,2,0,8,0,0,2,(uintptr_t)fences,(1u<<11)|(1u<<18)|(1u<<19),0,0};
        CHILD(!ioctl(drm,IOC(0x69,sizeof(request)),&request),18);
        long ticket2=syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_RESERVE,output2,0,0,0,0);CHILD(ticket2>ticket,23);
        objects[1].handle=imported2.handle;fences[1].handle=signal2.handle;
        CHILD(!ioctl(drm,IOC(0x69,sizeof(request)),&request),24);
        CHILD(syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_OBSERVE,ticket,0,0,0,0)==0,19);
        CHILD(syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_OBSERVE,ticket2,0,0,0,0)==0,25);
        CHILD(!syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_RECOVERY_BEGIN,ticket,0,0,0,0),20);
        CHILD(!syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_RECOVERY_STATUS,0,0,0,0,0),21);
        dprintf(2,"BLOCKED_FIXTURE_CHILD_DRAINED\n");_exit(75);
    }
    int status=0,finished=0;
    for(unsigned i=0;i<1000;++i){pid_t result=waitpid(child,&status,WNOHANG);CHECK(result>=0);if(result==child){finished=1;break;}usleep(10000);}
    if(!finished){(void)kill(child,SIGKILL);fprintf(stderr,"BLOCKED_FIXTURE_EXIT_TIMEOUT\n");return 1;}
    CHECK(WIFEXITED(status) && WEXITSTATUS(status)==75);
    int attached=0;for(unsigned i=0;i<500;++i){
        if(!syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_ATTACH,0,0,0,0,0)){attached=1;break;}
        CHECK(errno==EBUSY);usleep(10000);
    }CHECK(attached);
    long next=syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_RESERVE,output,0,0,0,0);CHECK(next>0);
    long next2=syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_RESERVE,output2,0,0,0,0);CHECK(next2>next);
    CHECK(!syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_RELEASE,next,0,0,0,0));
    CHECK(!syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_RELEASE,next2,0,0,0,0));
    CHECK(!syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_DETACH,0,0,0,0,0));
    close(output2);close(output);close(drm);puts("BLOCKED_FIXTURE_TWO_OUTPUTS_OWNER_EXIT_PASS");return 0;
}
