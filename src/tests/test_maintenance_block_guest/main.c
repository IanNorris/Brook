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

#ifndef BROOK_TEST_BLOCK_PATH
#define BROOK_TEST_BLOCK_PATH "/dev/brook-test-disk"
#endif
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"FAIL line %d: %s errno=%d\n",__LINE__,#x,errno);return 1; } } while(0)
int main(void) {
    const char *path=BROOK_TEST_BLOCK_PATH;
    struct stat st;
    CHECK(stat(path,&st)==0 && S_ISBLK(st.st_mode));
    int fd=open(path,O_RDWR|O_EXCL);
    CHECK(fd>=0 && fstat(fd,&st)==0 && S_ISBLK(st.st_mode));
    int other=open(path,O_RDWR);
    CHECK(other>=0 && lseek(other,0,SEEK_CUR)==0);
    CHECK(lseek(other,4096,SEEK_SET)==4096 && lseek(fd,0,SEEK_CUR)==0);
    pid_t competing=fork();CHECK(competing>=0);
    if(competing==0) {int denied=open(path,O_RDWR);_exit(denied<0 && errno==EBUSY ? 0 : 4);}
    int competingStatus=0;CHECK(waitpid(competing,&competingStatus,0)==competing && WIFEXITED(competingStatus) && WEXITSTATUS(competingStatus)==0);
    CHECK(close(other)==0);
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
    int gate[2];CHECK(pipe(gate)==0);
    pid_t inheritor=fork();CHECK(inheritor>=0);
    if(inheritor==0) {
        close(gate[1]);char signal=0;
        if(read(gate[0],&signal,1)!=1 || pread(fd,got,1,8192)!=1)_exit(5);
        close(copy);close(fd);_exit(0);
    }
    CHECK(close(gate[0])==0);
    CHECK(close(copy)==0 && close(fd)==0);
    competing=fork();CHECK(competing>=0);
    if(competing==0) {int denied=open(path,O_RDWR);_exit(denied<0 && errno==EBUSY ? 0 : 4);}
    CHECK(waitpid(competing,&competingStatus,0)==competing && WIFEXITED(competingStatus) && WEXITSTATUS(competingStatus)==0);
    CHECK(write(gate[1],"x",1)==1 && close(gate[1])==0);
    int inheritedStatus=0;CHECK(waitpid(inheritor,&inheritedStatus,0)==inheritor && WIFEXITED(inheritedStatus) && WEXITSTATUS(inheritedStatus)==0);
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
