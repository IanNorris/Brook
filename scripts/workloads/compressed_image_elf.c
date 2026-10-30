#define _GNU_SOURCE
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <unistd.h>
static unsigned char pattern(unsigned i){return (unsigned char)(i*73+(i>>8));}
int main(int argc,char** argv) {
    if(argc==2 && !strcmp(argv[1],"--child"))return 77;
    const char* base=argc>1?argv[1]:"/data/packed";
    char data[512],hard[512],linkpath[512],private[512],privateexe[512],newpath[512];
    snprintf(data,sizeof(data),"%s/data.bin",base);snprintf(hard,sizeof(hard),"%s/hard",base);
    snprintf(linkpath,sizeof(linkpath),"%s/link",base);snprintf(private,sizeof(private),"%s/private/denied",base);
    snprintf(privateexe,sizeof(privateexe),"%s/private/hello",base);snprintf(newpath,sizeof(newpath),"%s/new",base);
    int fd=open(data,O_RDONLY);assert(fd>=0);struct stat s,a;assert(fstat(fd,&s)==0 && s.st_size==131089);
    assert(stat(hard,&a)==0 && a.st_ino==s.st_ino && a.st_dev==s.st_dev && a.st_nlink==2);
    unsigned char bytes[8192];assert(pread(fd,bytes,sizeof(bytes),65530)==sizeof(bytes));
    for(unsigned i=0;i<sizeof(bytes);++i)assert(bytes[i]==pattern(65530+i));
    for(unsigned off=0;off<131089;off+=sizeof(bytes)) {
        unsigned n=131089-off;if(n>sizeof(bytes))n=sizeof(bytes);
        assert(pread(fd,bytes,n,off)==n);for(unsigned i=0;i<n;++i)assert(bytes[i]==pattern(off+i));
    }
    char target[64];assert(lstat(linkpath,&a)==0 && S_ISLNK(a.st_mode));
    assert(readlink(linkpath,target,sizeof(target))==8 && !memcmp(target,"data.bin",8));
    int via=open(linkpath,O_RDONLY);assert(via>=0 && fstat(via,&a)==0 && a.st_ino==s.st_ino);assert(close(via)==0);
    DIR* directory=opendir(base);assert(directory);unsigned found=0;struct dirent* entry;
    while((entry=readdir(directory)))if(!strcmp(entry->d_name,"data.bin"))++found;
    assert(found==1 && closedir(directory)==0);
    int dir=open(base,O_RDONLY|O_DIRECTORY);assert(dir>=0);via=openat(dir,"data.bin",O_RDONLY);assert(via>=0 && close(via)==0);
    assert(open(newpath,O_CREAT|O_WRONLY,0600)==-1 && errno==EROFS);
    assert(open(data,O_RDWR)==-1 && errno==EROFS);
    assert(chmod(data,0777)==-1 && errno==EROFS && truncate(data,0)==-1 && errno==EROFS);
    struct statvfs fs;assert(fstatvfs(fd,&fs)==0 && (fs.f_flag&ST_RDONLY));
    unsigned char* mapped=mmap(NULL,s.st_size,PROT_READ|PROT_WRITE,MAP_PRIVATE,fd,0);assert(mapped!=MAP_FAILED);
    for(unsigned i=0;i<131089;i+=4096)assert(mapped[i]==pattern(i));
    mapped[0]^=0x55;assert(pread(fd,bytes,1,0)==1 && bytes[0]==pattern(0));assert(munmap(mapped,s.st_size)==0);
    assert(mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0)==MAP_FAILED && (errno==EACCES || errno==EROFS));
    pid_t child=fork();assert(child>=0);
    if(!child) {
        assert(setuid(65534)==0); // distinct from host-built image owner as well as root
        assert(open(private,O_RDONLY)==-1 && errno==EACCES);
        assert(openat(dir,"private/denied",O_RDONLY)==-1 && errno==EACCES);
        execl(privateexe,"hello","--child",(char*)0);assert(errno==EACCES);_exit(0);
    }
    int status;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
    if(argc>2 && !strcmp(argv[2],"--detach")) {
        // Optional physical-media acceptance: source FAT must stay retained
        // after both source and compressed namespaces have been detached.
        if(argc>3) {
            char source[512];snprintf(source,sizeof(source),"%s/APPS.BRK",argv[3]);
            assert(open(source,O_RDWR)==-1 && errno==EROFS);
            assert(umount2(argv[3],MNT_DETACH)==0);
        }
        assert(umount2(base,MNT_DETACH)==0);
        assert(pread(fd,bytes,17,65536)==17);for(unsigned i=0;i<17;++i)assert(bytes[i]==pattern(65536+i));
        via=openat(dir,"data.bin",O_RDONLY);assert(via>=0 && close(via)==0);
    }
    assert(close(dir)==0 && close(fd)==0);
    puts("COMPRESSED_IMAGE_ELF_PASS executable cross_block hardlink symlink openat readonly private_COW shared_write_denied permissions cleanup");
}
