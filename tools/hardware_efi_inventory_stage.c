/* Stage only a new, separately selectable EFI inventory loader. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../src/shared/brook_system_esp.h"
#include "../src/shared/update/payload.h"

#if defined(BROOK_MAINTENANCE_RAM_EXEC_STAGE)
#define STAGE_EFI_NAME "BRKMODE.ELF"
#elif defined(BROOK_EFI_GUARD_T6_STAGE)
#define STAGE_EFI_NAME "BROOKT6.EFI"
#elif defined(BROOK_EFI_GUARD_NEXT_STAGE)
#define STAGE_EFI_NAME "BROOKV5.EFI"
#elif defined(BROOK_EFI_GUARD_STAGE)
#define STAGE_EFI_NAME "BROOKG2.EFI"
#else
#define STAGE_EFI_NAME "BROOKINV.EFI"
#endif

static int exact_read(int fd, void *data, size_t size) {
    uint8_t *p=data;
    while(size) { ssize_t n=read(fd,p,size); if(n<0&&errno==EINTR)continue; if(n<=0)return -1; p+=n; size-=(size_t)n; }
    return 0;
}
static int exact_write(int fd, const void *data, size_t size) {
    const uint8_t *p=data;
    while(size) { ssize_t n=write(fd,p,size); if(n<0&&errno==EINTR)continue; if(n<=0)return -1; p+=n; size-=(size_t)n; }
    return 0;
}
static int decode_hash(const char *text,uint8_t hash[32]) {
    if(strlen(text)!=64)return -1;
    for(unsigned i=0;i<32;i++) {
        unsigned value=0;
        for(unsigned j=0;j<2;j++) { char c=text[2*i+j];
            if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))return -1;
            value=value*16+(unsigned)(c<='9'?c-'0':c-'a'+10);
        }
        hash[i]=(uint8_t)value;
    }
    return 0;
}
#if !defined(BROOK_MAINTENANCE_RAM_EXEC_STAGE)
static int valid_efi(const uint8_t *p,size_t size) {
    if(size<256 || p[0]!='M'||p[1]!='Z')return 0;
    uint32_t off=BrookUpdateGet32(p+0x3c);
    if(off>size-26 || memcmp(p+off,"PE\0\0",4) || p[off+4]!=0x64 || p[off+5]!=0x86)return 0;
    return p[off+24]==0x0b && p[off+25]==0x02;
}
#endif
static int create_checked(int dir,const uint8_t *data,size_t size,dev_t device) {
    int fd=openat(dir,STAGE_EFI_NAME,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0644);
    if(fd<0)return -1;
    int error=exact_write(fd,data,size) || fsync(fd);
    if(close(fd))error=1;
    if(error)return -1;
    fd=openat(dir,STAGE_EFI_NAME,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
    struct stat st;uint8_t buffer[65536];size_t at=0;
    if(fd<0)return -1;
    if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_dev!=device||(uint64_t)st.st_size!=size)error=1;
    while(!error&&at<size) { size_t n=size-at>sizeof(buffer)?sizeof(buffer):size-at;
        if(exact_read(fd,buffer,n)||memcmp(buffer,data+at,n))error=1;
        at+=n;
    }
    if(close(fd))error=1;
    return error?-1:0;
}
int main(int argc,char **argv) {
    int source=-1,root=-1,efi=-1,dir=-1,lock=-1,result=1,mutated=0;uint8_t *image=NULL;
    uint8_t expected[32],actual[32];struct stat st,rst,dst,check;
    if(argc!=3||geteuid()!=0||decode_hash(argv[2],expected)) {
        fprintf(stderr,"usage: efi-inventory-stage image sha256\n");return 2;
    }
    source=open(argv[1],O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
    if(source<0||fstat(source,&st)||!S_ISREG(st.st_mode)||st.st_size<256||st.st_size>8*1024*1024)goto done;
    image=malloc((size_t)st.st_size);if(!image||exact_read(source,image,(size_t)st.st_size))goto done;
    BrookUpdateHash(image,(size_t)st.st_size,actual);
    if(!BrookUpdateEqual(expected,actual,32))goto done;
#if defined(BROOK_MAINTENANCE_RAM_EXEC_STAGE)
    if (st.st_size < 64 || memcmp(image,"\177ELF\2\1\1",7) || image[18]!=62 || image[19]) goto done;
#else
    if(!valid_efi(image,(size_t)st.st_size))goto done;
#endif
    root=open("/media/system",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(root<0||ioctl(root,BROOK_IOCTL_VERIFY_SYSTEM_ESP_ROOT,0)||fstat(root,&rst))goto done;
    lock=open("/tmp/hardware-update.lock",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);if(lock<0)goto done;
    efi=openat(root,"EFI",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(efi<0||ioctl(efi,BROOK_IOCTL_VERIFY_SYSTEM_ESP_MEMBER,0))goto done;
    dir=openat(efi,"BOOT",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(dir<0||ioctl(dir,BROOK_IOCTL_VERIFY_SYSTEM_ESP_MEMBER,0)||fstat(dir,&dst)||dst.st_dev!=rst.st_dev)goto done;
    if(fstatat(dir,STAGE_EFI_NAME,&check,AT_SYMLINK_NOFOLLOW)==0||errno!=ENOENT)goto done;
    mutated=1;
    if(create_checked(dir,image,(size_t)st.st_size,rst.st_dev)||fsync(dir)||fsync(efi)||fsync(root))goto done;
    {
        int fresh=openat(efi,"BOOT",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        if(fresh<0)goto done;
        int error=ioctl(fresh,BROOK_IOCTL_VERIFY_SYSTEM_ESP_MEMBER,0)||fstat(fresh,&check)||
                  check.st_dev!=dst.st_dev||check.st_ino!=dst.st_ino;
        if(close(fresh))error=1;
        if(error)goto done;
    }
    result=0;
done:
    if(source>=0)close(source);
    if(dir>=0)close(dir);
    if(efi>=0)close(efi);
    if(root>=0)close(root);
    if(lock>=0) {close(lock);if((!result||!mutated)&&unlink("/tmp/hardware-update.lock"))result=1;}
    free(image);
    if(result)fprintf(stderr,"EFI_INVENTORY_STAGE_FAILED mutated=%d; inspect before retry\n",mutated);
    else puts("EFI_INVENTORY_STAGE_OK existing loaders unchanged");
    return result;
}
