// Empty pathname contracts: distinguish a missing path from descriptor lookup.
#define _GNU_SOURCE
#include <sys/stat.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>

struct stamp { int64_t seconds; uint32_t nanos; int32_t reserved; };
struct xstat {
    uint32_t mask, blocksize; uint64_t attributes;
    uint32_t links, uid, gid; uint16_t mode, reserved;
    uint64_t inode, size, blocks, attributes_mask;
    struct stamp atime, btime, ctime, mtime;
    uint32_t rdev_major, rdev_minor, dev_major, dev_minor;
    uint64_t spare[14];
};
_Static_assert(sizeof(struct xstat)==256,"statx ABI");
static int failures;
static void check(const char *name,long result,int expected) {
    int actual=result<0?errno:0;
    printf("%s %s errno=%d expected=%d\n",actual==expected?"PASS":"FAIL",name,actual,expected);
    failures+=actual!=expected;
}
int main(int argc,char **argv) {
    char folder[512];snprintf(folder,sizeof(folder),"%s/atstatcheck-XXXXXX",argc>1?argv[1]:"/tmp");
    int old=open(".",O_RDONLY|O_DIRECTORY);if(old<0||!mkdtemp(folder)||chdir(folder))return 2;
    int fd=open("owned",O_CREAT|O_RDWR|O_EXCL,0600);if(fd<0)return 2;
    if(write(fd,"owned stat fixture",18)!=18)return 2;
    struct stat out,reference;struct xstat sx;
#define FS(d,p,f,e,n) do{errno=0;check(n,syscall(SYS_newfstatat,(d),(p),&out,(f)),(e));}while(0)
#define SX(d,p,f,e,n) do{errno=0;check(n,syscall(SYS_statx,(d),(p),(f),0x7ff,&sx),(e));}while(0)
    FS(AT_FDCWD,"",0,ENOENT,"fstatat empty cwd without flag");
    FS(fd,"",0,ENOENT,"fstatat empty file without flag");
    FS(-1,"",0,ENOENT,"fstatat empty invalid fd without flag");
    FS(fd,"",AT_EMPTY_PATH,0,"fstatat file empty lookup");
    if(fstat(fd,&reference)||out.st_ino!=reference.st_ino||out.st_size!=reference.st_size)++failures;
    FS(AT_FDCWD,"",AT_EMPTY_PATH,0,"fstatat cwd empty lookup");
    if(stat(".",&reference)||out.st_ino!=reference.st_ino||!S_ISDIR(out.st_mode))++failures;
    FS(-1,"",AT_EMPTY_PATH,EBADF,"fstatat invalid descriptor");
    FS(fd,NULL,AT_EMPTY_PATH,0,"fstatat NULL descriptor lookup");
    // Linux 6.12's NULL fast path accepts real descriptors; AT_FDCWD still
    // reaches pathname copying and returns EFAULT (unlike an empty string).
    FS(AT_FDCWD,NULL,AT_EMPTY_PATH,EFAULT,"fstatat NULL cwd lookup");
    FS(fd,NULL,0,EFAULT,"fstatat NULL without flag");
    FS(fd,NULL,AT_EMPTY_PATH|AT_SYMLINK_NOFOLLOW,EFAULT,"fstatat NULL nofollow is not a pathname");
    FS(fd,NULL,AT_EMPTY_PATH|AT_NO_AUTOMOUNT,EFAULT,"fstatat NULL automount hint misses fast path");
    FS(AT_FDCWD,"owned",0x40000000,EINVAL,"fstatat invalid flags");
    FS(AT_FDCWD,"owned",AT_NO_AUTOMOUNT,0,"fstatat ignored automount hint");
    SX(AT_FDCWD,"",0,ENOENT,"statx empty without flag");
    SX(fd,"",AT_EMPTY_PATH,0,"statx file empty lookup");
    if(fstat(fd,&reference)||sx.inode!=reference.st_ino||sx.size!=(uint64_t)reference.st_size)++failures;
    SX(AT_FDCWD,"",AT_EMPTY_PATH,0,"statx cwd empty lookup");
    if(stat(".",&reference)||sx.inode!=reference.st_ino||!S_ISDIR(sx.mode))++failures;
    SX(-1,"",AT_EMPTY_PATH,EBADF,"statx invalid descriptor");
    SX(fd,NULL,AT_EMPTY_PATH,0,"statx NULL descriptor lookup");
    SX(AT_FDCWD,NULL,AT_EMPTY_PATH,EFAULT,"statx NULL cwd lookup");
    SX(fd,NULL,0,EFAULT,"statx NULL without flag");
    SX(fd,NULL,AT_EMPTY_PATH|AT_SYMLINK_NOFOLLOW,EFAULT,"statx NULL nofollow misses fast path");
    SX(fd,NULL,AT_EMPTY_PATH|AT_NO_AUTOMOUNT,0,"statx NULL automount hint fast path");
    SX(fd,NULL,AT_EMPTY_PATH|0x2000,0,"statx NULL force sync hint");
    SX(fd,NULL,AT_EMPTY_PATH|0x4000,0,"statx NULL dont sync hint");
    SX(fd,NULL,AT_EMPTY_PATH|0x6000,EINVAL,"statx conflicting sync flags");
    SX(fd,NULL,AT_EMPTY_PATH|0x40000000,EINVAL,"statx invalid flags");
    int cwd=open(".",O_RDONLY|O_DIRECTORY);if(cwd<0||fchmod(cwd,0600))return 2;
    FS(AT_FDCWD,"",AT_EMPTY_PATH,0,"fstatat cwd metadata needs no search permission");
    SX(AT_FDCWD,"",AT_EMPTY_PATH,0,"statx cwd metadata needs no search permission");
    if(fchmod(cwd,0700))return 2;
    close(cwd);
    close(fd);unlink("owned");fchdir(old);close(old);rmdir(folder);
    printf("ATSTATCHECK %s failures=%d\n",failures?"FAIL":"PASS",failures);return failures?1:0;
}
