/* Test-kernel-only: an independent client must run while a prior client's
 * submission waits on a synthetic dependency. Neither uses handoff output. */
#define _GNU_SOURCE
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
struct context {uint32_t id,pad;};
struct sync_object {uint32_t handle,flags;};
struct object {uint32_t handle,reloc_count;uint64_t relocs,alignment,offset,flags,reserved1,reserved2;};
struct fence {uint32_t handle,flags;};
struct exec {uint64_t objects;uint32_t count,start,length,dr1,dr4,clip_count;uint64_t clips,flags,context,fences;};
struct wait_gem {uint32_t handle,flags;int64_t timeout;};
struct Client {int drm;uint32_t handle,context;uint32_t *output;};
static int submit(struct Client *c,uint32_t value,uint32_t dependency){
    if(c->drm<0)c->drm=open("/dev/dri/renderD129",O_RDWR|O_CLOEXEC);
    if(c->drm<0)return -1;
    struct context context={0};struct create data={4096,0,0},batch={4096,0,0};
    if(ioctl(c->drm,IOC(0x6d,sizeof(context)),&context) || !context.id ||
       ioctl(c->drm,IOC(0x5b,sizeof(data)),&data) || ioctl(c->drm,IOC(0x5b,sizeof(batch)),&batch))return -1;
    struct map_offset a={data.handle,0,0,2,0},b={batch.handle,0,0,2,0};
    if(ioctl(c->drm,IOC(0x64,sizeof(a)),&a) || ioctl(c->drm,IOC(0x64,sizeof(b)),&b))return -1;
    c->output=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED,c->drm,(off_t)a.offset);
    uint32_t *cmd=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED,c->drm,(off_t)b.offset);
    if(c->output==MAP_FAILED || cmd==MAP_FAILED)return -1;
    cmd[0]=0x10000002;cmd[1]=0x40011000;cmd[2]=0;cmd[3]=value;cmd[4]=0x05000000;cmd[5]=0;
    struct object objects[]={{data.handle,0,0,4096,0x40011000,20,0,0},{batch.handle,0,0,4096,0x10000,16,0,0}};
    struct fence wait={dependency,1};struct exec request={(uintptr_t)objects,2,0,24,0,0,dependency?1u:0u,
        dependency?(uintptr_t)&wait:0,(1u<<11)|(dependency?(1u<<19):0),context.id,0};
    c->handle=data.handle;c->context=context.id;return ioctl(c->drm,IOC(0x69,sizeof(request)),&request);
}
static int wait_buffer(struct Client *c,int64_t ns){struct wait_gem w={c->handle,0,ns};return ioctl(c->drm,IOC(0x6c,sizeof(w)),&w);}
#define CHECK(c) do{if(!(c)){fprintf(stderr,"INDEPENDENT_QUEUE_FAIL line=%d errno=%d\n",__LINE__,errno);return 1;}}while(0)
int main(void){
    int ready[2],release[2];CHECK(!pipe(ready) && !pipe(release));pid_t child=fork();CHECK(child>=0);
    if(!child){
        close(ready[0]);close(release[1]);struct Client a={.drm=-1};
        int seed=open("/dev/dri/renderD129",O_RDWR|O_CLOEXEC);if(seed<0)_exit(10);
        /* The dependency and submission share this open file. */
        struct sync_object pending={0,0};if(ioctl(seed,IOC(0xbf,sizeof(pending)),&pending))_exit(11);
        pending.flags=0x42524b54u;if(ioctl(seed,IOC(0xee,sizeof(pending)),&pending))_exit(12);
        a.drm=seed;if(submit(&a,0x13579bdfu,pending.handle))_exit(13);
        errno=0;if(!wait_buffer(&a,0) || errno!=ETIME){pending.flags=0x42524b55u;(void)ioctl(seed,IOC(0xee,sizeof(pending)),&pending);_exit(14);}
        if(write(ready[1],"R",1)!=1){pending.flags=0x42524b55u;(void)ioctl(seed,IOC(0xee,sizeof(pending)),&pending);_exit(15);}
        char byte=0;const ssize_t received=read(release[0],&byte,1);
        pending.flags=0x42524b55u;if(ioctl(seed,IOC(0xee,sizeof(pending)),&pending))_exit(16);
        if(received!=1 || byte!='G' || wait_buffer(&a,5000000000ll) || a.output[0]!=0x13579bdfu)_exit(17);
        close(seed);_exit(0);
    }
    close(ready[1]);close(release[0]);char byte=0;
    CHECK(read(ready[0],&byte,1)==1 && byte=='R');
    struct Client b={.drm=-1};CHECK(!submit(&b,0x2468ace0u,0));
    CHECK(!wait_buffer(&b,5000000000ll) && b.output[0]==0x2468ace0u);
    CHECK(write(release[1],"G",1)==1);close(release[1]);close(ready[0]);
    int status=0,done=0;for(unsigned i=0;i<1000;++i){pid_t r=waitpid(child,&status,WNOHANG);CHECK(r>=0);if(r==child){done=1;break;}usleep(10000);}
    if(!done){(void)kill(child,SIGKILL);fprintf(stderr,"INDEPENDENT_QUEUE_TIMEOUT\n");return 1;}
    CHECK(WIFEXITED(status) && !WEXITSTATUS(status));close(b.drm);puts("INDEPENDENT_QUEUE_PHYSICAL_PASS");return 0;
}
