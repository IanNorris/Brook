// vcs_store_probe.c - VA-API phase 1c acceptance probe for Brook's Broadwell
// VCS0 (BSD) user submission. Hand-rolled i915 UAPI, no libdrm.
//
// Finds the i915 render node, reads GETPARAM HAS_BSD, then submits one
// softpinned MI_STORE_DATA_IMM batch with I915_EXEC_BSD | NO_RELOC and a
// syncobj signal fence, waits on the fence and on the target BO, and checks
// the stored value. Modes:
//   (default)          HAS_BSD must be 1; N jobs (--count N, default 1) must
//                      each complete with the right value. Exit 0 = PASS.
//   --expect-refused   HAS_BSD must be 0 and the BSD execbuffer must fail
//                      with EINVAL (gate off / no self-test). Exit 0 = PASS.
//   --high             softpin the batch and target above 4 GiB (48-bit).
//   --hang             DESTRUCTIVE for this boot: a self-looping batch. Expect
//                      the job's fence to fail (EIO on the BO) after the 2 s
//                      budget, VCS quarantined and HAS_BSD back to 0.
// Build: $MUSL_CC -static -O2 -Wall -Wextra -Werror vcs_store_probe.c -o vcs_store_probe
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define IOWR(nr,size) ((3ul<<30)|((unsigned long)(size)<<16)|('d'<<8)|(nr))

struct drm_version {int major,minor,patch,pad;uint64_t name_len,name,date_len,date,desc_len,desc;};
struct getparam {int32_t param,pad;uint64_t value;};
struct gem_create {uint64_t size;uint32_t handle,pad;};
struct gem_close {uint32_t handle,pad;};
struct gem_wait {uint32_t handle,flags;int64_t timeout;};
struct mmap_offset {uint32_t handle,pad;uint64_t offset,flags,extensions;};
struct exec_object {uint32_t handle,reloc_count;uint64_t relocs,alignment,offset,flags,rsvd1,rsvd2;};
struct execbuffer {uint64_t objects;uint32_t count,start,length,dr1,dr4,num_cliprects;uint64_t cliprects,flags,rsvd1,rsvd2;};
struct exec_fence {uint32_t handle,flags;};
struct syncobj {uint32_t handle,flags;};
struct syncobj_wait {uint64_t handles;int64_t timeout;uint32_t count,flags,first,pad;uint64_t deadline;};

#define DRM_VERSION        IOWR(0x00,sizeof(struct drm_version))
#define DRM_GEM_CLOSE      IOWR(0x09,sizeof(struct gem_close))
#define I915_GETPARAM      IOWR(0x46,sizeof(struct getparam))
#define I915_GEM_CREATE    IOWR(0x5b,sizeof(struct gem_create))
#define I915_MMAP_OFFSET   IOWR(0x64,sizeof(struct mmap_offset))
#define I915_EXECBUFFER2   IOWR(0x69,sizeof(struct execbuffer))
#define I915_GEM_WAIT      IOWR(0x6c,sizeof(struct gem_wait))
#define SYNCOBJ_CREATE     IOWR(0xbf,sizeof(struct syncobj))
#define SYNCOBJ_DESTROY    IOWR(0xc0,sizeof(struct syncobj))
#define SYNCOBJ_WAIT       IOWR(0xc3,sizeof(struct syncobj_wait))

#define EXEC_BSD        2u
#define EXEC_NO_RELOC   (1u<<11)
#define EXEC_FENCE_ARRAY (1u<<19)
#define OBJ_WRITE 4u
#define OBJ_48B   8u
#define OBJ_PINNED 16u

static int xioctl(int fd,unsigned long req,void* arg){int r;do r=ioctl(fd,req,arg);while(r<0 && errno==EINTR);return r;}
static uint64_t now_ns(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000ull+(uint64_t)t.tv_nsec;}

static int open_i915(char* path,size_t len){
    for(int n=128;n<136;++n){
        snprintf(path,len,"/dev/dri/renderD%d",n);
        int fd=open(path,O_RDWR|O_CLOEXEC);if(fd<0)continue;
        char name[16]={0};struct drm_version v={0};v.name_len=sizeof(name)-1;v.name=(uintptr_t)name;
        if(!xioctl(fd,DRM_VERSION,&v) && !strcmp(name,"i915"))return fd;
        close(fd);
    }
    return -1;
}
static int getparam(int fd,int param,int* value){struct getparam g={param,0,(uintptr_t)value};*value=-1;return xioctl(fd,I915_GETPARAM,&g);}
static uint32_t bo_create(int fd){struct gem_create c={4096,0,0};return xioctl(fd,I915_GEM_CREATE,&c)?0:c.handle;}
static uint32_t* bo_map(int fd,uint32_t h){
    struct mmap_offset m={h,0,0,2,0};if(xioctl(fd,I915_MMAP_OFFSET,&m))return NULL;
    void* p=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED,fd,(off_t)m.offset);return p==MAP_FAILED?NULL:p;
}

int main(int argc,char** argv){
    int expectRefused=0,high=0,hang=0;unsigned count=1;
    for(int i=1;i<argc;++i){
        if(!strcmp(argv[i],"--expect-refused"))expectRefused=1;
        else if(!strcmp(argv[i],"--high"))high=1;
        else if(!strcmp(argv[i],"--hang"))hang=1;
        else if(!strcmp(argv[i],"--count") && i+1<argc)count=(unsigned)strtoul(argv[++i],NULL,0);
        else {fprintf(stderr,"usage: %s [--expect-refused] [--high] [--hang] [--count N]\n",argv[0]);return 2;}
    }
    if(!count || count>100000)count=1;
    char path[32];int fd=open_i915(path,sizeof(path));
    if(fd<0){printf("VCSPROBE FAIL no i915 render node\n");return 1;}
    int hasBsd=-1;const int gp=getparam(fd,5,&hasBsd);
    printf("VCSPROBE node=%s has_bsd=%d getparam=%d errno=%d\n",path,hasBsd,gp,gp?errno:0);
    if(gp){printf("VCSPROBE FAIL getparam HAS_BSD\n");return 1;}
    if(!expectRefused && hasBsd!=1){printf("VCSPROBE SKIP has_bsd=%d (gate off or no self-test this boot)\n",hasBsd);return 3;}
    if(expectRefused && hasBsd!=0){printf("VCSPROBE FAIL expected has_bsd=0\n");return 1;}

    const uint32_t data=bo_create(fd),batch=bo_create(fd);
    uint32_t* d=data?bo_map(fd,data):NULL;uint32_t* b=batch?bo_map(fd,batch):NULL;
    if(!d || !b){printf("VCSPROBE FAIL bo create/map errno=%d\n",errno);return 1;}
    const uint64_t base=high?0x100000000ull:0;
    const uint64_t dataAddr=base+0x100000,batchAddr=base+0x200000;
    unsigned failures=0;uint64_t worstNs=0,totalNs=0;
    for(unsigned job=0;job<count;++job){
        const uint32_t value=0x56430000u^(job*0x9e3779b9u)^(uint32_t)now_ns();
        memset(d,0,4096);memset(b,0,4096);
        if(hang){ // MI_BATCH_BUFFER_START (PPGTT) to itself: never ends.
            b[0]=0x18800101;b[1]=(uint32_t)batchAddr;b[2]=(uint32_t)(batchAddr>>32);b[3]=0;
        }else{   // MI_STORE_DATA_IMM, 64-bit PPGTT address; MI_BATCH_BUFFER_END.
            b[0]=0;b[1]=(0x20u<<23)|2u;b[2]=(uint32_t)dataAddr;b[3]=(uint32_t)(dataAddr>>32);b[4]=value;b[5]=0x05000000u;
        }
        __sync_synchronize();
        struct syncobj so={0,0};
        if(xioctl(fd,SYNCOBJ_CREATE,&so)){printf("VCSPROBE FAIL syncobj create errno=%d\n",errno);return 1;}
        struct exec_object objs[2]={
            {data,0,0,4096,dataAddr,OBJ_PINNED|OBJ_WRITE|(high?OBJ_48B:0),0,0},
            {batch,0,0,4096,batchAddr,OBJ_PINNED|(high?OBJ_48B:0),0,0}};
        struct exec_fence fence={so.handle,2}; // I915_EXEC_FENCE_SIGNAL
        struct execbuffer eb={(uintptr_t)objs,2,0,hang?16:24,0,0,1,(uintptr_t)&fence,
            EXEC_BSD|EXEC_NO_RELOC|EXEC_FENCE_ARRAY,0,0};
        const uint64_t t0=now_ns();
        const int r=xioctl(fd,I915_EXECBUFFER2,&eb);const int err=r?errno:0;
        if(expectRefused){
            printf("VCSPROBE execbuffer result=%d errno=%d\n",r,err);
            const int ok=r && err==EINVAL;
            printf("VCSPROBE %s refused-as-expected=%d\n",ok?"PASS":"FAIL",ok);return ok?0:1;
        }
        if(r){printf("VCSPROBE FAIL job=%u execbuffer errno=%d\n",job,err);++failures;break;}
        uint32_t handle=so.handle;
        struct syncobj_wait w={(uintptr_t)&handle,(int64_t)(now_ns()+5000000000ull),1,1,0,0,0}; // WAIT_ALL
        const int wr=xioctl(fd,SYNCOBJ_WAIT,&w);const int werr=wr?errno:0;
        struct gem_wait gw={data,0,2000000000ll};
        const int gr=xioctl(fd,I915_GEM_WAIT,&gw);const int gerr=gr?errno:0;
        const uint64_t dt=now_ns()-t0;totalNs+=dt;if(dt>worstNs)worstNs=dt;
        __sync_synchronize();
        const uint32_t got=d[0];
        if(hang){
            printf("VCSPROBE hang job: fence_wait=%d/%d bo_wait=%d/%d value=%08x elapsed_ms=%llu\n",wr,werr,gr,gerr,got,
                (unsigned long long)(dt/1000000));
            int after=-1;getparam(fd,5,&after);
            const int ok=!wr && gr && gerr==EIO && after==0;
            printf("VCSPROBE %s hang-detected=%d has_bsd_after=%d\n",ok?"PASS":"FAIL",ok,after);
            return ok?0:1;
        }
        const int ok=!wr && !gr && got==value;
        if(!ok || job<3 || job+1==count)
            printf("VCSPROBE job=%u fence_wait=%d/%d bo_wait=%d/%d value=%08x expected=%08x us=%llu %s\n",job,wr,werr,gr,gerr,got,value,
                (unsigned long long)(dt/1000),ok?"ok":"MISMATCH");
        struct syncobj del={so.handle,0};xioctl(fd,SYNCOBJ_DESTROY,&del);
        if(!ok){++failures;break;}
    }
    int after=-1;getparam(fd,5,&after);
    printf("VCSPROBE %s jobs=%u failures=%u mean_us=%llu worst_us=%llu has_bsd_after=%d high=%d\n",failures?"FAIL":"PASS",count,failures,
        (unsigned long long)(totalNs/1000/count),(unsigned long long)(worstNs/1000),after,high);
    munmap(d,4096);munmap(b,4096);
    struct gem_close c1={data,0},c2={batch,0};xioctl(fd,DRM_GEM_CLOSE,&c1);xioctl(fd,DRM_GEM_CLOSE,&c2);
    close(fd);
    return failures?1:0;
}
