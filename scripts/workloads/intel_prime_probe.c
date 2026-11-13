// Exercise Intel PRIME aliases and SCM_RIGHTS on an explicitly enabled device.
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#define IOC(n,s) ((3u<<30)|((unsigned)(s)<<16)|('d'<<8)|(n))
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"INTEL_PRIME_FAIL line=%d errno=%d\n",__LINE__,errno);return 1; } } while (0)
struct create { uint64_t size;uint32_t handle,pad; };
struct prime { uint32_t handle,flags;int32_t fd; };
struct mmap_offset {uint32_t handle,pad;uint64_t offset,flags,extensions;};
struct gem_close {uint32_t handle,pad;};
int main(void) {
    int a=open("/dev/dri/renderD129",O_RDWR|O_CLOEXEC), b=open("/dev/dri/renderD129",O_RDWR|O_CLOEXEC);
    CHECK(a>=0 && b>=0);
    struct create create={4096,0,0};CHECK(!ioctl(a,IOC(0x5b,sizeof(create)),&create));
    struct mmap_offset map={create.handle,0,0,2,0};CHECK(!ioctl(a,IOC(0x64,sizeof(map)),&map));
    uint32_t *producer=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED,a,map.offset);CHECK(producer!=MAP_FAILED);
    *producer=0x1234abcd;
    struct prime out={create.handle,O_CLOEXEC|O_RDWR,-1};CHECK(!ioctl(a,IOC(0x2d,sizeof(out)),&out) && out.fd>=0);
    CHECK(fcntl(out.fd,F_GETFD)&FD_CLOEXEC);
    struct stat first,copy;CHECK(!fstat(out.fd,&first) && first.st_ino && first.st_size==4096);
    int duplicate=dup(out.fd);CHECK(duplicate>=0 && !fstat(duplicate,&copy));
    CHECK(copy.st_dev==first.st_dev && copy.st_ino==first.st_ino);close(duplicate);
    CHECK(lseek(out.fd,0,SEEK_END)==4096);
    CHECK(lseek(out.fd,0,SEEK_SET)==0);
    errno=0;CHECK(lseek(out.fd,1,SEEK_SET)==-1 && errno==EINVAL);
    errno=0;CHECK(lseek(out.fd,0,SEEK_CUR)==-1 && errno==EINVAL);
    int pair[2];CHECK(!socketpair(AF_UNIX,SOCK_STREAM,0,pair));
    char c='x',control[CMSG_SPACE(sizeof(int))]={0};struct iovec iov={&c,1};
    struct msghdr msg={0};msg.msg_iov=&iov;msg.msg_iovlen=1;msg.msg_control=control;msg.msg_controllen=sizeof(control);
    struct cmsghdr *header=CMSG_FIRSTHDR(&msg);header->cmsg_level=SOL_SOCKET;header->cmsg_type=SCM_RIGHTS;
    header->cmsg_len=CMSG_LEN(sizeof(int));memcpy(CMSG_DATA(header),&out.fd,sizeof(int));
    CHECK(sendmsg(pair[0],&msg,0)==1);close(out.fd);
    struct gem_close old={create.handle,0};CHECK(!ioctl(a,IOC(9,sizeof(old)),&old));close(a);
    memset(control,0,sizeof(control));msg.msg_controllen=sizeof(control);CHECK(recvmsg(pair[1],&msg,0)==1);
    header=CMSG_FIRSTHDR(&msg);CHECK(header && header->cmsg_level==SOL_SOCKET && header->cmsg_type==SCM_RIGHTS);
    int received=-1;memcpy(&received,CMSG_DATA(header),sizeof(received));CHECK(received>=0);
    CHECK(!fstat(received,&copy) && copy.st_dev==first.st_dev && copy.st_ino==first.st_ino);
    CHECK(lseek(received,0,SEEK_END)==4096);
    struct prime in={0,0,received};CHECK(!ioctl(b,IOC(0x2e,sizeof(in)),&in) && in.handle);
    struct prime again={0,0,received};CHECK(!ioctl(b,IOC(0x2e,sizeof(again)),&again) && again.handle==in.handle);
    close(received);map=(struct mmap_offset){in.handle,0,0,2,0};CHECK(!ioctl(b,IOC(0x64,sizeof(map)),&map));
    uint32_t *consumer=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED,b,map.offset);CHECK(consumer!=MAP_FAILED);
    CHECK(*consumer==0x1234abcd);*consumer=0x99887766;CHECK(*producer==0x99887766);
    struct gem_close imported={in.handle,0};CHECK(!ioctl(b,IOC(9,sizeof(imported)),&imported));close(b);
    CHECK(*consumer==0x99887766);munmap(consumer,4096);munmap(producer,4096);close(pair[0]);close(pair[1]);
    puts("INTEL_PRIME_SCM_PASS");return 0;
}
