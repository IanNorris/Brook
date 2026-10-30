#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
static const char* path="/data/home/.brook-storage-acceptance";
static void check(int ok,const char* what){if(!ok){perror(what);exit(1);}}
int main(int argc,char** argv){
    check(argc==3 && strlen(argv[2])==32,"arguments");
    int create=!strcmp(argv[1],"create");check(create||!strcmp(argv[1],"verify"),"operation");
    check(getuid()==1000,"ordinary desktop UID");
    struct stat st;check(!stat("/data/home",&st)&&st.st_uid==1000,"persistent home owner");
    unsigned char expected[8199];for(unsigned i=0;i<sizeof(expected);++i)expected[i]=(i*73+19)%251;
    memcpy(expected,argv[2],32);expected[4093]=0xe3;expected[4096]=0x37;
    int fd=open(path,O_RDWR|(create?(O_CREAT|O_EXCL):0),0600);check(fd>=0,"open persistence file");
    if(create){
        unsigned char initial[8199];memcpy(initial,expected,sizeof(initial));initial[4093]^=0xff;initial[4096]^=0xff;
        check(write(fd,initial,sizeof(initial))==(ssize_t)sizeof(initial),"write page/tail data");
        check(!fsync(fd),"initial fsync");
        unsigned char* shared=mmap(NULL,8192,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
        check(shared!=MAP_FAILED,"shared mmap");shared[4093]=0xe3;shared[4096]=0x37;
        check(!msync(shared,8192,MS_SYNC),"shared msync");check(!munmap(shared,8192),"shared munmap");
        check(!fsync(fd),"post-mmap fsync");
    }
    unsigned char data[8199];check(pread(fd,data,sizeof(data),0)==(ssize_t)sizeof(data),"reread persistence file");
    check(!memcmp(data,expected,sizeof(data)),"persistent bytes");
    unsigned char* private=mmap(NULL,8192,PROT_READ|PROT_WRITE,MAP_PRIVATE,fd,0);
    check(private!=MAP_FAILED,"private mmap");
    pid_t pid=fork();check(pid>=0,"fork COW child");
    if(!pid){private[0]^=0xff;private[4096]^=0x7f;_exit(private[0]!=expected[0] && private[4096]!=expected[4096]?0:2);}
    int status;check(waitpid(pid,&status,0)==pid && WIFEXITED(status)&&!WEXITSTATUS(status),"COW child exit");
    check(!memcmp(private,expected,8192),"parent mapping survived child COW");
    private[0]^=0xff;check(!munmap(private,8192),"private munmap");
    check(pread(fd,data,sizeof(data),0)==(ssize_t)sizeof(data)&&!memcmp(data,expected,sizeof(data)),"private mapping did not alter file");
    check(!close(fd),"close persistence file");
    errno=0;fd=open("/media/usb/STORE.IMG",O_RDONLY);
    check(fd<0 && errno==EBUSY,"exclusive backing alias refused");
    puts(create?"WRITABLE_DESKTOP_CREATE_PASS":"WRITABLE_DESKTOP_REBOOT_PASS");return 0;
}
