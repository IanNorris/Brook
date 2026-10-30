#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>
int main(int argc,char** argv) {
    if(argc==2&&!strcmp(argv[1],"recovery")) {
        struct stat st;assert(stat("/nix/hello",&st)==-1&&errno==ENOENT);
        puts("APP_SNAPSHOT_RECOVERY_OK no_application_mount");return 0;
    }
    assert(argc==1);
    int fd=open("/media/usb/APPS.BRK",O_RDWR|O_NOFOLLOW);assert(fd>=0);
    struct stat st;assert(!fstat(fd,&st)&&S_ISREG(st.st_mode)&&st.st_size>0&&st.st_size<16*1024*1024);
    char zeros[4096]={0};
    for(off_t offset=0;offset<st.st_size;) {
        size_t n=st.st_size-offset>(off_t)sizeof(zeros)?sizeof(zeros):(size_t)(st.st_size-offset);
        ssize_t written=pwrite(fd,zeros,n,offset);assert(written>0);offset+=written;
    }
    assert(fsync(fd)==0&&close(fd)==0);
    assert(umount2("/media/usb",MNT_DETACH)==0);
    assert(open("/media/usb/APPS.BRK",O_RDONLY)==-1&&errno==ENOENT);
    puts("APP_SNAPSHOT_SOURCE_OVERWRITTEN_NAMESPACE_DETACHED");fflush(stdout);
    // Neither this ELF's load segments nor the data files have been accessed
    // before the USB source was destroyed and unmounted.
    execl("/nix/hello","hello","/nix",(char*)0);
    perror("APP_SNAPSHOT_EXEC");return 1;
}
