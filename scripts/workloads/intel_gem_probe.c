// Opt-in, non-rendering Intel render-node smoke test. Exercises the real
// userspace GEM ioctl, mmap, dup and close paths; no GPU commands are issued.
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define IOC(nr, size) ((3u<<30)|((unsigned)(size)<<16)|('d'<<8)|(nr))
struct version {int32_t major,minor,patch,pad;uint64_t name_len,name,date_len,date,desc_len,desc;};
struct getparam {int32_t param,pad;uint64_t value;};
struct create {uint64_t size;uint32_t handle,pad;};
struct wait_gem {uint32_t handle,flags;int64_t timeout;};
struct close_gem {uint32_t handle,pad;};
struct mmap_offset {uint32_t handle,pad;uint64_t offset,flags,extensions;};
struct context {uint32_t id,pad;};
struct context_param {uint32_t id,size;uint64_t param,value;};
struct busy {uint32_t handle,busy;};
struct advice {uint32_t handle,advice,retained;};
struct domain {uint32_t handle,read,write;};
#define CHECK(c) do {if(!(c)){fprintf(stderr,"INTEL_GEM_PROBE_FAIL line=%d errno=%d\n",__LINE__,errno);return 1;}} while(0)
int main(void){
    DIR* dir=opendir("/dev/dri");CHECK(dir);int found=0;struct dirent* entry;
    while((entry=readdir(dir)))if(!strcmp(entry->d_name,"renderD129"))found=1;
    CHECK(found);
    struct stat relative;CHECK(!fstatat(dirfd(dir),"renderD129",&relative,0) && S_ISCHR(relative.st_mode));
    CHECK(!fstatat(dirfd(dir),"renderD129",&relative,AT_SYMLINK_NOFOLLOW) && S_ISCHR(relative.st_mode));
    int relative_fd=openat(dirfd(dir),"renderD129",O_RDWR);CHECK(relative_fd>=0);CHECK(!close(relative_fd) && !closedir(dir));
    int fd=open("/dev/dri/renderD129",O_RDWR|O_CLOEXEC);CHECK(fd>=0);
    struct stat st;CHECK(!fstat(fd,&st) && S_ISCHR(st.st_mode) && st.st_rdev==0xe281);
    char name[16]={0};struct version v={0};v.name_len=sizeof(name);v.name=(uint64_t)(uintptr_t)name;
    CHECK(!ioctl(fd,IOC(0,sizeof(v)),&v) && v.major==1 && v.name_len==4 && !memcmp(name,"i915",4));
    int value=-1;struct getparam gp={4,0,(uint64_t)(uintptr_t)&value};
    CHECK(!ioctl(fd,IOC(0x46,sizeof(gp)),&gp) && value==0x1616);
    gp.param=32;CHECK(!ioctl(fd,IOC(0x46,sizeof(gp)),&gp) && value==9);
    gp.param=9;value=-1;CHECK(!ioctl(fd,IOC(0x46,sizeof(gp)),&gp) && value==0);
    gp.param=37;value=-1;CHECK(!ioctl(fd,IOC(0x46,sizeof(gp)),&gp) && value==0);
    gp.param=19;value=-1;CHECK(!ioctl(fd,IOC(0x46,sizeof(gp)),&gp) && value==1);
    gp.param=999;errno=0;CHECK(ioctl(fd,IOC(0x46,sizeof(gp)),&gp)==-1 && errno==EINVAL);
    gp.param=4;gp.value=0;errno=0;CHECK(ioctl(fd,IOC(0x46,sizeof(gp)),&gp)==-1 && errno==EFAULT);
    int other=open("/dev/dri/renderD129",O_RDWR);CHECK(other>=0);
    struct context ctx={0};CHECK(!ioctl(fd,IOC(0x6d,sizeof(ctx)),&ctx) && ctx.id);
    struct context_param cp={ctx.id,0,8,0};CHECK(!ioctl(fd,IOC(0x75,sizeof(cp)),&cp));
    cp.value=1;CHECK(!ioctl(fd,IOC(0x74,sizeof(cp)),&cp) && !cp.value);
    errno=0;CHECK(ioctl(other,IOC(0x74,sizeof(cp)),&cp)==-1 && errno==ENOENT);
    CHECK(!ioctl(fd,IOC(0x6e,sizeof(ctx)),&ctx));
    errno=0;CHECK(ioctl(fd,IOC(0x6e,sizeof(ctx)),&ctx)==-1 && errno==ENOENT);
    cp=(struct context_param){0,0,3,0};CHECK(!ioctl(fd,IOC(0x74,sizeof(cp)),&cp) && cp.value==(1ull<<48));
    struct create c={8192,0,0};CHECK(!ioctl(fd,IOC(0x5b,sizeof(c)),&c) && c.handle && c.size==8192);
    struct domain dom={c.handle,1,0};CHECK(!ioctl(fd,IOC(0x5f,sizeof(dom)),&dom));
    dom.read=2;errno=0;CHECK(ioctl(fd,IOC(0x5f,sizeof(dom)),&dom)==-1 && errno==EINVAL);
    struct busy busy_req={c.handle,1};CHECK(!ioctl(fd,IOC(0x57,sizeof(busy_req)),&busy_req) && !busy_req.busy);
    struct advice advice_req={c.handle,1,0};CHECK(!ioctl(fd,IOC(0x66,sizeof(advice_req)),&advice_req) && advice_req.retained);
    struct wait_gem wait_req={c.handle,0,0};CHECK(!ioctl(fd,IOC(0x6c,sizeof(wait_req)),&wait_req));
    wait_req.flags=1;errno=0;CHECK(ioctl(fd,IOC(0x6c,sizeof(wait_req)),&wait_req)==-1 && errno==EINVAL);
    wait_req.flags=0;wait_req.handle=c.handle+1;errno=0;CHECK(ioctl(fd,IOC(0x6c,sizeof(wait_req)),&wait_req)==-1 && errno==ENOENT);
    struct mmap_offset m={c.handle,0,0,2,0};
    CHECK(ioctl(other,IOC(0x64,sizeof(m)),&m)<0);
    CHECK(!ioctl(fd,IOC(0x64,sizeof(m)),&m) && m.offset);
    uint8_t* p=mmap(NULL,8192,PROT_READ|PROT_WRITE,MAP_SHARED,fd,(off_t)m.offset);CHECK(p!=MAP_FAILED);
    CHECK(!p[0] && !p[4096]);p[0]=0x45;p[4096]=0x91;
    int alias=dup(fd);CHECK(alias>=0);CHECK(!close(fd));
    struct close_gem close_req={c.handle,0};CHECK(!ioctl(alias,IOC(9,sizeof(close_req)),&close_req));
    CHECK(mmap(NULL,4096,PROT_READ,MAP_SHARED,alias,(off_t)m.offset)==MAP_FAILED);
    CHECK(!close(alias) && p[0]==0x45 && p[4096]==0x91);
    CHECK(!munmap(p,8192) && !close(other));
    puts("INTEL_GEM_PROBE_PASS");return 0;
}
