/* First-time allocation only; never enables panic writes or replaces a file. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <cstdio>
#include <sys/ioctl.h>
#include <sys/statvfs.h>
#include "hardware_provision_identity.h"
#include "../src/shared/brook_system_esp.h"
#include "../src/kernel/src/panic_journal.h"

static int create_journal(int dir,dev_t device) {
    constexpr size_t bytes=brook::PanicJournalSectors*brook::PanicJournalSector;
    uint8_t buffer[4096]={};
    int fd=openat(dir,"PANIC.BIN",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if(fd<0)return -1;
    size_t done=0;int error=0;
    while(done<bytes) {
        const size_t chunk=bytes-done<sizeof(buffer)?bytes-done:sizeof(buffer);
        ssize_t n=write(fd,buffer,chunk);
        if(n<0&&errno==EINTR)continue;
        if(n<=0){error=1;break;}done+=size_t(n);
    }
    if(!error&&fsync(fd))error=1;
    if(close(fd))error=1;
    if(error)return -1;
    fd=openat(dir,"PANIC.BIN",O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
    struct stat st{};done=0;
    if(fd<0)return -1;
    if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_dev!=device||st.st_size!=off_t(bytes))error=1;
    while(!error&&done<bytes) {
        const size_t chunk=bytes-done<sizeof(buffer)?bytes-done:sizeof(buffer);
        ssize_t n=read(fd,buffer,chunk);
        if(n<0&&errno==EINTR)continue;
        if(n<=0){error=1;break;}
        for(ssize_t i=0;i<n;++i)if(buffer[i])error=1;
        done+=size_t(n);
    }
    if(close(fd))error=1;
    return error?-1:0;
}
int main(int argc,char** argv) {
    if(argc!=2||geteuid()!=0||strlen(argv[1])!=96) {
        std::fprintf(stderr,"usage: panic-journal-provision expected-identity\n");return 2;
    }
    char identity[97];
    if(load_identity(identity)||strcmp(identity,argv[1])) {
        std::fprintf(stderr,"PANIC_JOURNAL_IDENTITY_MISMATCH\n");return 1;
    }
    int root=-1,lock=-1,result=1,mutated=0;struct stat rst{},check{};struct statvfs space{};
    root=open("/media/system",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(root<0||ioctl(root,BROOK_IOCTL_VERIFY_SYSTEM_ESP_ROOT,0)||fstat(root,&rst)||
       fstatvfs(root,&space)||!space.f_frsize||space.f_bavail>UINT64_MAX/space.f_frsize||
       uint64_t(space.f_bavail)*space.f_frsize<brook::PanicJournalSectors*brook::PanicJournalSector+1048576)goto done;
    lock=open("/tmp/hardware-update.lock",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if(lock<0)goto done;
    if(fstatat(root,"PANIC.BIN",&check,AT_SYMLINK_NOFOLLOW)==0||errno!=ENOENT)goto done;
    mutated=1;
    if(create_journal(root,rst.st_dev)||fsync(root))goto done;
    {
        int fresh=open("/media/system",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        if(fresh<0)goto done;
        int error=ioctl(fresh,BROOK_IOCTL_VERIFY_SYSTEM_ESP_ROOT,0)||fstat(fresh,&check)||
                  check.st_dev!=rst.st_dev||check.st_ino!=rst.st_ino;
        if(close(fresh))error=1;
        if(error)goto done;
    }
    result=0;
done:
    if(root>=0)close(root);
    if(lock>=0) {close(lock);if((!result||!mutated)&&unlink("/tmp/hardware-update.lock"))result=1;}
    if(result)std::fprintf(stderr,"PANIC_JOURNAL_PROVISION_FAILED mutated=%d; inspect before retry\n",mutated);
    else std::puts("PANIC_JOURNAL_PROVISION_OK writer remains disabled");
    return result;
}
