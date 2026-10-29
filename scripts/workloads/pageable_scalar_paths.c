#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <unistd.h>
static long ctl(long op,long value){return syscall(0xB40,op,value,0L,0L,0L,0L);}
static void cold(unsigned char* p){for(unsigned i=0;i<16;++i){
    (void)((volatile unsigned char*)p)[i*4096];
    assert(ctl(1,(long)(uintptr_t)(p+i*4096))==4096);
}}
int main(void){
    char expected[256];ssize_t size=readlink("/proc/self/exe",expected,sizeof(expected));assert(size>0 && size<200);
    int rootfd=open("/",O_RDONLY|O_DIRECTORY);assert(rootfd>=0);
    long address=ctl(0,16*4096);assert(address>0);unsigned char* p=(void*)(uintptr_t)address;
    char* path=(void*)(p+4096-8);char* output=(void*)(p+3*4096-4);
    char* nullPath=(void*)(p+7*4096-8);strcpy(nullPath,"/dev/null");
    strcpy(path,"/proc/self/exe");memset(output,0,256);cold(p);
    assert(ctl(2,0)==16 && syscall(SYS_brk,0L)>0 && ctl(2,0)==16);
    assert(readlink(path,output,256)==size && !memcmp(output,expected,size));
    assert(ctl(2,0)==12 && ctl(6,0)==0);
    cold(p);assert(readlinkat(-100,path,output,256)==size && !memcmp(output,expected,size));
    assert(ctl(2,0)==12 && ctl(6,0)==0);
    assert(mprotect(p+8192,8192,PROT_READ)==0);cold(p);
    errno=0;assert(readlink(path,output,256)==-1 && errno==EFAULT);
    assert(ctl(2,0)==14 && ctl(6,0)==0);
    assert(mprotect(p+8192,8192,PROT_READ|PROT_WRITE)==0);
    cold(p);
    int fd=syscall(SYS_open,nullPath,O_RDONLY,0);assert(fd>=0 && ctl(2,0)==14);
    assert(close(fd)==0 && ctl(2,0)==14 && ctl(6,0)==0);
    cold(p);fd=openat(AT_FDCWD,nullPath,O_RDONLY);assert(fd>=0 && ctl(2,0)==14);
    assert(close(fd)==0 && ctl(2,0)==14 && ctl(6,0)==0);
    cold(p);errno=0;assert(syscall(SYS_open,1L,O_RDONLY,0)==-1 && errno==EFAULT);
    assert(ctl(2,0)==16 && ctl(6,0)==0);
    fd=openat(rootfd,nullPath+1,O_RDONLY);assert(fd>=0 && ctl(2,0)==14);
    assert(close(fd)==0 && ctl(2,0)==14 && ctl(6,0)==0);
    assert(((volatile unsigned char*)p)[8*4096]==0 && ctl(2,0)==13);
    struct stat* metadata=(void*)(p+9*4096-32);
    cold(p);assert(syscall(SYS_fstat,rootfd,metadata)==0 && S_ISDIR(metadata->st_mode));
    assert(ctl(2,0)==14 && ctl(6,0)==0);
    assert(mprotect(p+8*4096,8192,PROT_READ)==0);cold(p);
    errno=0;assert(syscall(SYS_fstat,rootfd,metadata)==-1 && errno==EFAULT);
    assert(ctl(2,0)==16 && ctl(6,0)==0);
    errno=0;assert(syscall(SYS_fstat,-1,metadata)==-1 && errno==EBADF && ctl(2,0)==16);
    assert(munmap(p,16*4096)==0 && ctl(2,0)==0 && ctl(6,0)==0);
    unsigned char statxBytes[256];
    assert(syscall(332,rootfd,"",0x1000,0x7ff,statxBytes)==0);
    uint16_t mode;memcpy(&mode,statxBytes+28,sizeof(mode));assert(S_ISDIR(mode));
    assert(close(rootfd)==0);
    puts("PAGEABLE_SCALAR_PATHS_PASS brk_close_no_restore open_openat_readlink_readlinkat_targeted fstat_staged crossing_pages readonly lease_cleanup");
}
