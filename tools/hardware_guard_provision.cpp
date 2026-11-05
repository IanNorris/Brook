/* First-time guard provisioning on the boot-admitted SSD system volume.
 * Never replace an existing guard, loader, recovery payload or selector. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../src/shared/brook_system_esp.h"
#include "../src/shared/update/recovery_guard.h"

static int read_file(const char *path, char *out, size_t capacity) {
    int fd=open(path,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);struct stat st;
    if(fd<0)return -1;
    if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_size<=0||(uint64_t)st.st_size>=capacity) {close(fd);return -1;}
    size_t done=0;
    while(done<(size_t)st.st_size) {
        ssize_t n=read(fd,out+done,(size_t)st.st_size-done);
        if(n<0&&errno==EINTR)continue;
        if(n<=0){close(fd);return -1;}done+=(size_t)n;
    }
    if(close(fd))return -1;
    out[done]='\0';return 0;
}
static int split(char *buffer,char **lines,unsigned count) {
    char *p=buffer;
    for(unsigned i=0;i<count;++i) {
        lines[i]=p;char *end=strchr(p,'\n');
        if(!end || end==p)return -1;
        *end='\0';p=end+1;
    }
    return *p ? -1 : 0;
}
static int hex32(const char *s) {
    if(strlen(s)!=32)return 0;
    for(unsigned i=0;i<32;++i)if(!((s[i]>='0'&&s[i]<='9')||(s[i]>='a'&&s[i]<='f')))return 0;
    return 1;
}
static int range(const char *s) {
    int comma=0,digits=0;
    for(;*s;++s) {
        if(*s==',' && !comma && digits){comma=1;digits=0;continue;}
        if(*s<'0'||*s>'9')return 0;
        ++digits;
    }
    return comma&&digits;
}
static int load_identity(char out[97]) {
    char esp[160],data[192];char *e[4],*d[5];
    if(read_file("/etc/ahci-boot-volume",esp,sizeof(esp)) ||
       read_file("/etc/ahci-data-volume",data,sizeof(data)) || split(esp,e,4) || split(data,d,5) ||
       strcmp(e[0],"BRKESP1") || strcmp(d[0],"BRKDATA1") ||
       !hex32(e[1]) || !hex32(e[2]) || !hex32(d[1]) || !hex32(d[2]) || !hex32(d[4]) ||
       strcmp(e[1],d[1]) || !range(e[3]) || !range(d[3]))return -1;
    memcpy(out,d[1],32);memcpy(out+32,d[2],32);memcpy(out+64,d[4],32);out[96]='\0';return 0;
}
static int create_guard(int directory,const char *name,const uint8_t record[128],dev_t device) {
    int fd=openat(directory,name,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0644);
    if(fd<0)return -1;
    size_t done=0;int error=0;
    while(done<128) {
        ssize_t n=write(fd,record+done,128-done);
        if(n<0&&errno==EINTR)continue;
        if(n<=0){error=1;break;}done+=(size_t)n;
    }
    if(!error && fsync(fd))error=1;
    if(close(fd))error=1;
    if(error)return -1;
    fd=openat(directory,name,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);struct stat st;uint8_t check[128];done=0;
    if(fd<0)return -1;
    if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_dev!=device||st.st_size!=128)error=1;
    while(!error && done<128) {
        ssize_t n=read(fd,check+done,128-done);
        if(n<0&&errno==EINTR)continue;
        if(n<=0){error=1;break;}done+=(size_t)n;
    }
    if(!error && memcmp(record,check,128))error=1;
    if(close(fd))error=1;
    return error?-1:0;
}
int main(int argc,char **argv) {
    if(argc!=2||geteuid()!=0||strlen(argv[1])!=96) {
        fprintf(stderr,"usage: guard-provision expected-identity\n");return 2;
    }
    char identity[97];uint8_t record[128]={0};
    if(load_identity(identity)||strcmp(identity,argv[1])) {fprintf(stderr,"GUARD_PROVISION_IDENTITY_MISMATCH\n");return 1;}
    memcpy(record,"BRKRCV01",8);BrookUpdatePut64(record+8,1);BrookUpdatePut32(record+16,BROOK_RECOVERY_GUARD_CLEAN);
    for(unsigned i=0;i<48;++i) {
        unsigned value=0;
        for(unsigned j=0;j<2;++j) {char c=identity[i*2+j];value=value*16+(unsigned)(c<='9'?c-'0':c-'a'+10);}
        record[24+i]=(uint8_t)value;
    }
    BrookUpdatePut32(record+124,BrookUpdateCrc(record,124));
    if(!BrookRecoveryGuardAllowsNormal(record,128,record,128,identity))return 1;
    int root=-1,dir=-1,lock=-1,result=1,mutated=0;struct stat rst,dst,check;
    root=open("/media/system",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(root<0||ioctl(root,BROOK_IOCTL_VERIFY_SYSTEM_ESP_ROOT,0)||fstat(root,&rst))goto done;
    lock=open("/tmp/hardware-update.lock",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if(lock<0)goto done;
    dir=openat(root,"RECOVERY",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(dir<0||ioctl(dir,BROOK_IOCTL_VERIFY_SYSTEM_ESP_MEMBER,0)||fstat(dir,&dst)||dst.st_dev!=rst.st_dev)goto done;
    if(fstatat(dir,"GUARD0.BIN",&check,AT_SYMLINK_NOFOLLOW)==0 || errno!=ENOENT)goto done;
    if(fstatat(dir,"GUARD1.BIN",&check,AT_SYMLINK_NOFOLLOW)==0 || errno!=ENOENT)goto done;
    mutated=1;
    if(create_guard(dir,"GUARD0.BIN",record,rst.st_dev)||fsync(dir)||fsync(root))goto done;
    if(create_guard(dir,"GUARD1.BIN",record,rst.st_dev)||fsync(dir)||fsync(root))goto done;
    {
        int fresh=openat(root,"RECOVERY",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        if(fresh<0)goto done;
        int error=ioctl(fresh,BROOK_IOCTL_VERIFY_SYSTEM_ESP_MEMBER,0)||fstat(fresh,&check)||
                  check.st_dev!=dst.st_dev||check.st_ino!=dst.st_ino;
        if(close(fresh))error=1;
        if(error)goto done;
    }
    result=0;
done:
    if(dir>=0)close(dir);
    if(root>=0)close(root);
    if(lock>=0) {close(lock);if((!result||!mutated)&&unlink("/tmp/hardware-update.lock"))result=1;}
    if(result)fprintf(stderr,"GUARD_PROVISION_FAILED mutated=%d; inspect before retry\n",mutated);
    else puts("GUARD_PROVISION_OK loader unchanged");
    return result;
}
