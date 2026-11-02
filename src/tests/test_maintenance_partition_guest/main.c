#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(x) do { if(!(x)) { fprintf(stderr,"FAIL line %d: %s errno=%d\n",__LINE__,#x,errno);return 1; } } while(0)
int main(int argc,char** argv) {
    const char *path="/dev/brook-test-disk2";
    if(argc==2 && !strcmp(argv[1],"invalid")) {
        int fd=open(path,O_RDONLY);CHECK(fd<0 && errno==ENODEV);
        puts("PARTITION_BAD_GPT_REJECTED");return 0;
    }
    struct stat pathstat,fdstat;CHECK(stat(path,&pathstat)==0 && S_ISBLK(pathstat.st_mode));
    int fd=open(path,O_RDWR|O_EXCL);CHECK(fd>=0);
    CHECK(stat(path,&pathstat)==0 && S_ISBLK(pathstat.st_mode));
    CHECK(fstat(fd,&fdstat)==0 && S_ISBLK(fdstat.st_mode) && fdstat.st_rdev==pathstat.st_rdev);
    int whole=open("/dev/brook-test-disk",O_RDONLY);CHECK(whole<0 && errno==EBUSY);
    uint64_t size=0;CHECK(ioctl(fd,BLKGETSIZE64,&size)==0 && size==94208u*512u);
    char data[16];CHECK(pread(fd,data,sizeof(data),(off_t)size-3)==3);
    CHECK(pwrite(fd,data,1,(off_t)size)<0 && errno==ENOSPC);
    int copy=dup(fd);CHECK(copy>=0);
    CHECK(fsync(fd)==0 && close(fd)==0);
    whole=open("/dev/brook-test-disk",O_RDONLY);CHECK(whole<0 && errno==EBUSY);
    CHECK(close(copy)==0);
    whole=open("/dev/brook-test-disk",O_RDONLY);CHECK(whole>=0 && close(whole)==0);
    puts("PARTITION_RANGE_OK");return 0;
}
