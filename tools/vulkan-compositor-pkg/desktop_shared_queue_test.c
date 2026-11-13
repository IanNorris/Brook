/* Test-kernel-only: distinct clients using the same PRIME buffer must retain
 * order even when the earlier request has an injected pending dependency. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#define IOC(n,s) ((3u<<30)|((unsigned)(s)<<16)|('d'<<8)|(n))
struct create {uint64_t size;uint32_t handle,pad;};
struct map_offset {uint32_t handle,pad;uint64_t offset,flags,extensions;};
struct context {uint32_t id,pad;};
struct prime {uint32_t handle,flags;int32_t fd;};
struct sync_object {uint32_t handle,flags;};
struct sync_handle {uint32_t handle,flags;int32_t fd;uint32_t pad;};
struct object {uint32_t handle,reloc_count;uint64_t relocs,alignment,offset,flags,reserved1,reserved2;};
struct fence {uint32_t handle,flags;};
struct exec {uint64_t objects;uint32_t count,start,length,dr1,dr4,clip_count;uint64_t clips,flags,context,fences;};
static int submit(int drm,uint32_t data,uint32_t dependency,uint32_t signal,uint32_t value){
    struct context context={0};struct create batch={4096,0,0};
    if(ioctl(drm,IOC(0x6d,sizeof(context)),&context) || !context.id || ioctl(drm,IOC(0x5b,sizeof(batch)),&batch))return -1;
    struct map_offset map={batch.handle,0,0,2,0};if(ioctl(drm,IOC(0x64,sizeof(map)),&map))return -1;
    uint32_t *cmd=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED,drm,(off_t)map.offset);if(cmd==MAP_FAILED)return -1;
    cmd[0]=0x10000002;cmd[1]=0x40011000;cmd[2]=0;cmd[3]=value;cmd[4]=0x05000000;cmd[5]=0;
    struct object objects[]={{data,0,0,4096,0x40011000,20,0,0},{batch.handle,0,0,4096,0x10000,16,0,0}};
    struct fence f={dependency?dependency:signal,dependency?1u:2u};
    struct exec request={(uintptr_t)objects,2,0,24,0,0,1,(uintptr_t)&f,(1u<<11)|(1u<<19),context.id,0};
    return ioctl(drm,IOC(0x69,sizeof(request)),&request);
}
static int poll_retry(struct pollfd *p,int milliseconds){
    for(unsigned i=0;i<8;++i){int result=poll(p,1,milliseconds);if(result>=0 || errno!=EINTR)return result;}
    return -1;
}
#define CHECK(c) do{if(!(c)){fprintf(stderr,"SHARED_QUEUE_FAIL line=%d errno=%d\n",__LINE__,errno);return 1;}}while(0)
int main(void){
    int drm=open("/dev/dri/renderD129",O_RDWR|O_CLOEXEC);CHECK(drm>=0);
    struct create data={4096,0,0};CHECK(!ioctl(drm,IOC(0x5b,sizeof(data)),&data));
    struct map_offset map={data.handle,0,0,2,0};CHECK(!ioctl(drm,IOC(0x64,sizeof(map)),&map));
    uint32_t *output=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED,drm,(off_t)map.offset);CHECK(output!=MAP_FAILED);
    struct prime shared={data.handle,O_CLOEXEC|O_RDWR,-1};CHECK(!ioctl(drm,IOC(0x2d,sizeof(shared)),&shared) && shared.fd>=0);
    int ready[2],release[2];CHECK(!pipe(ready) && !pipe(release));pid_t child=fork();CHECK(child>=0);
    if(!child){
        close(ready[0]);close(release[1]);int own=open("/dev/dri/renderD129",O_RDWR|O_CLOEXEC);if(own<0)_exit(10);
        struct prime imported={0,0,shared.fd};if(ioctl(own,IOC(0x2e,sizeof(imported)),&imported) || !imported.handle)_exit(11);
        struct sync_object pending={0,0};if(ioctl(own,IOC(0xbf,sizeof(pending)),&pending))_exit(12);
        pending.flags=0x42524b54u;if(ioctl(own,IOC(0xee,sizeof(pending)),&pending))_exit(13);
        if(submit(own,imported.handle,pending.handle,0,0x11112222u))_exit(14);
        if(write(ready[1],"R",1)!=1){pending.flags=0x42524b55u;(void)ioctl(own,IOC(0xee,sizeof(pending)),&pending);_exit(15);}
        char value=0;ssize_t n=read(release[0],&value,1);
        pending.flags=0x42524b55u;if(ioctl(own,IOC(0xee,sizeof(pending)),&pending))_exit(16);
        close(own);_exit(n==1 && value=='G'?0:17);
    }
    close(ready[1]);close(release[0]);char byte=0;CHECK(read(ready[0],&byte,1)==1 && byte=='R');
    struct sync_object done={0,0};CHECK(!ioctl(drm,IOC(0xbf,sizeof(done)),&done));
    CHECK(!submit(drm,data.handle,0,done.handle,0x33334444u));
    struct sync_handle exported={done.handle,1,-1,0};CHECK(!ioctl(drm,IOC(0xc1,sizeof(exported)),&exported) && exported.fd>=0);
    struct pollfd p={.fd=exported.fd,.events=POLLIN};CHECK(poll_retry(&p,500)==0);
    CHECK(write(release[1],"G",1)==1);close(release[1]);close(ready[0]);
    p.revents=0;CHECK(poll_retry(&p,5000)==1 && (p.revents&POLLIN));
    CHECK(output[0]==0x33334444u);int status=0;CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
    close(exported.fd);close(shared.fd);close(drm);puts("SHARED_QUEUE_PHYSICAL_PASS");return 0;
}
