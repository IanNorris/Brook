/* First-time staging only: never changes the EFI loader or boot configuration. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include "../src/shared/brook_system_esp.h"
#include "../src/shared/update/payload.h"
#include "../src/shared/update/executable.h"

#if defined(BROOK_RECOVERY_STAGE_REPLACEMENT)
#define STAGE_DIRECTORY "RECOVNEW"
#elif defined(BROOK_RECOVERY_STAGE_ALTERNATE)
#define STAGE_DIRECTORY "RECOV2"
#else
#define STAGE_DIRECTORY "RECOVERY"
#endif

static int exact_read(int fd, void *buffer, size_t size) {
    uint8_t *p=buffer;
    while(size) { ssize_t n=read(fd,p,size); if(n<0 && errno==EINTR)continue; if(n<=0)return -1; p+=n; size-=(size_t)n; }
    return 0;
}
static int exact_write(int fd, const void *buffer, size_t size) {
    const uint8_t *p=buffer;
    while(size) { ssize_t n=write(fd,p,size); if(n<0 && errno==EINTR)continue; if(n<=0)return -1; p+=n; size-=(size_t)n; }
    return 0;
}
static int load(const char *path, size_t min, size_t max, uint8_t **out, size_t *size) {
    int fd=open(path,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);struct stat st;
    if(fd<0)return -1;
    if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_size<(off_t)min||(uint64_t)st.st_size>max){close(fd);return -1;}
    uint8_t *p=malloc((size_t)st.st_size);if(!p){close(fd);return -1;}
    int err=exact_read(fd,p,(size_t)st.st_size);if(close(fd))err=-1;
    if(err){free(p);return -1;}*out=p;*size=(size_t)st.st_size;return 0;
}
static int create_checked(int dir,const char *name,const uint8_t *data,size_t size) {
    int fd=openat(dir,name,O_RDWR|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0644);if(fd<0)return -1;
    int err=exact_write(fd,data,size);if(!err && fsync(fd))err=-1;
    struct stat st;uint8_t buf[65536];size_t offset=0;
    if(!err)err=fstat(fd,&st)||!S_ISREG(st.st_mode)||(uint64_t)st.st_size!=size||lseek(fd,0,SEEK_SET)!=0;
    while(!err && offset<size){size_t n=size-offset>sizeof(buf)?sizeof(buf):size-offset;
        if(exact_read(fd,buf,n)||memcmp(buf,data+offset,n))err=1;
        offset+=n;}
    if(close(fd))err=1;
    return err?-1:0;
}
static int marker(const char *path) {
    int fd=open(path,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);if(fd<0)return -1;
    struct stat st;int err=fstat(fd,&st)||!S_ISREG(st.st_mode);if(close(fd))err=1;
    return err?-1:0;
}
static int initialize_power_record(void) {
    static const uint8_t empty[1024];
    int root=-1,dir=-1,lock=-1,result=1,mutated=0;
    struct stat file;
    root=open("/media/system",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(root<0||ioctl(root,BROOK_IOCTL_VERIFY_SYSTEM_ESP_ROOT,0)||
       marker("/etc/hardware-maintenance"))goto done;
    lock=open("/tmp/hardware-update.lock",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if(lock<0)goto done;
    dir=openat(root,"RECOVERY",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(dir<0||ioctl(dir,BROOK_IOCTL_VERIFY_SYSTEM_ESP_MEMBER,0)||
       fstatat(dir,"POWER.BIN",&file,AT_SYMLINK_NOFOLLOW)==0||errno!=ENOENT)goto done;
    mutated=1;
    if(create_checked(dir,"POWER.BIN",empty,sizeof(empty))||fsync(dir)||fsync(root))goto done;
    result=0;
done:
    if(dir>=0)close(dir);
    if(root>=0)close(root);
    if(lock>=0){close(lock);if((!result||!mutated) && unlink("/tmp/hardware-update.lock"))result=1;}
    if(result)fprintf(stderr,"AC_SHUTDOWN_RECORD_INIT_FAILED mutated=%d\n",mutated);
    else puts("AC_SHUTDOWN_RECORD_INIT_OK");
    return result;
}
int main(int argc,char **argv) {
    if(geteuid()==0 && argc==2 && !strcmp(argv[1],"--init-ac-shutdown-record"))
        return initialize_power_record();
    if(argc!=4||geteuid()!=0){fprintf(stderr,"usage: recovery-stage kernel initrd selector | --init-ac-shutdown-record\n");return 2;}
    uint8_t *kernel=NULL,*initrd=NULL,*record=NULL,hash[32];size_t kb=0,ib=0,rb=0;
    int root=-1,dir=-1,lock=-1,result=1,mutated=0;struct stat st,child;struct statvfs space;
    if(load(argv[1],64,BROOK_UPDATE_KERNEL_MAX,&kernel,&kb)||!BrookUpdateExecutable(kernel,kb)||
       load(argv[2],512,BROOK_UPDATE_INITRD_MAX,&initrd,&ib)||load(argv[3],128,128,&record,&rb)||
       !BrookUpdateRecordValid(record)||BrookUpdateGet32(record+16)!=0||
       BrookUpdateGet64(record+24)!=kb||BrookUpdateGet64(record+32)!=ib)goto done;
    BrookUpdateHash(kernel,kb,hash);if(!BrookUpdateEqual(hash,record+40,32))goto done;
    BrookUpdateHash(initrd,ib,hash);if(!BrookUpdateEqual(hash,record+72,32))goto done;
    root=open("/media/system",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(root<0||ioctl(root,BROOK_IOCTL_VERIFY_SYSTEM_ESP_ROOT,0)||fstat(root,&st)||
       marker("/etc/hardware-maintenance")||marker("/etc/ahci-boot-volume")||marker("/etc/ahci-write-target")||
       fstatvfs(root,&space)||!space.f_frsize||space.f_bavail>UINT64_MAX/space.f_frsize||
       (uint64_t)space.f_bavail*space.f_frsize<(uint64_t)kb+ib+1048576)goto done;
    lock=open("/tmp/hardware-update.lock",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);if(lock<0)goto done;
    /* An existing directory is never modified: inspect interrupted attempts. */
    if(fstatat(root,STAGE_DIRECTORY,&child,AT_SYMLINK_NOFOLLOW)==0 || errno!=ENOENT)goto done;
    /* FAT mkdir can change disk state even when its final sync fails. */
    mutated=1;
    if(mkdirat(root,STAGE_DIRECTORY,0755))goto done;
    dir=openat(root,STAGE_DIRECTORY,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(dir<0||fstat(dir,&child)||child.st_dev!=st.st_dev||
       ioctl(dir,BROOK_IOCTL_VERIFY_SYSTEM_ESP_MEMBER,0))goto done;
    if(create_checked(dir,"KERNEL.ELF",kernel,kb)||create_checked(dir,"INITRD.IMG",initrd,ib)||fsync(dir))goto done;
    /* Publish the selector last; the existing bootloader remains unchanged. */
    if(create_checked(dir,"SELECT.BIN",record,rb)||fsync(dir)||fsync(root))goto done;
    /* Detect a concurrent rename or overmount before reporting publication. */
    int check=openat(root,STAGE_DIRECTORY,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    struct stat final;
    if(check<0)goto done;
    int changed=ioctl(check,BROOK_IOCTL_VERIFY_SYSTEM_ESP_MEMBER,0)||fstat(check,&final)||
        final.st_dev!=child.st_dev||final.st_ino!=child.st_ino;
    if(close(check))changed=1;
    if(changed)goto done;
    result=0;
done:
    if(dir>=0)close(dir);
    if(root>=0)close(root);
    if(lock>=0){close(lock);if((!result||!mutated) && unlink("/tmp/hardware-update.lock"))result=1;}
    free(kernel);free(initrd);free(record);
    if(result)fprintf(stderr,"SSD_RECOVERY_STAGE_FAILED mutated=%d; inspect before retry\n",mutated);
    else puts("SSD_RECOVERY_STAGE_OK bootloader unchanged");
    return result;
}
