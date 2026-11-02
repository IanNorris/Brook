// Run only against the disposable QEMU disk exposed by the test kernel.
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(x) do { if(!(x)) { fprintf(stderr,"FAIL line %d: %s errno=%d\n",__LINE__,#x,errno);return 1; } } while(0)
int main(void) {
    const char *path="/dev/brook-test-disk";
    struct stat st;
    CHECK(stat(path,&st)==0 && S_ISBLK(st.st_mode));
    int fd=open(path,O_RDWR|O_EXCL);
    CHECK(fd>=0 && fstat(fd,&st)==0 && S_ISBLK(st.st_mode));
    int other=open(path,O_RDWR|O_EXCL);
    CHECK(other<0 && errno==EBUSY);
    uint64_t bytes=0;int sector=0;
    CHECK(ioctl(fd,BLKGETSIZE64,&bytes)==0 && bytes==64u*1024u*1024u);
    CHECK(ioctl(fd,BLKSSZGET,&sector)==0 && sector==512);
    CHECK(ioctl(fd,BLKPBSZGET,&sector)==0 && sector==512);
    CHECK(ioctl(fd,BLKRRPART,0)<0 && errno==ENOTTY);
    int copy=dup(fd);CHECK(copy>=0);
    CHECK(lseek(fd,8192,SEEK_SET)==8192 && lseek(copy,0,SEEK_CUR)==8192);
    const char data[]="Brook partition tool I/O probe";
    CHECK(write(copy,data,sizeof(data))==(ssize_t)sizeof(data));
    CHECK(lseek(fd,0,SEEK_CUR)==8192+(off_t)sizeof(data));
    char got[sizeof(data)]={0};
    CHECK(pread(fd,got,sizeof(got),8192)==sizeof(got) && !memcmp(got,data,sizeof(got)));
    CHECK(lseek(fd,-1,SEEK_SET)<0 && errno==EINVAL);
    CHECK(pread(fd,got,1,(off_t)bytes)==0);
    CHECK(fsync(fd)==0);
    CHECK(close(copy)==0 && close(fd)==0);
    fd=open(path,O_RDONLY);CHECK(fd>=0);
    CHECK(write(fd,data,sizeof(data))<0 && errno==EBADF);
    CHECK(close(fd)==0);
    pid_t child=fork();CHECK(child>=0);
    if(child==0) {
        if(setuid(1000))_exit(2);
        int denied=open(path,O_RDONLY);
        _exit(denied<0 && errno==EPERM ? 0 : 3);
    }
    int status=0;CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
    puts("MAINTENANCE_BLOCK_GUEST_OK");return 0;
}
