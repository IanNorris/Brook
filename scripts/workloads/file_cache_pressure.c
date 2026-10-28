#define _GNU_SOURCE
#include <assert.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
static long ctl(long op,long value){return syscall(0xB40,op,value,0L,0L,0L,0L);}
int main(int argc,char** argv) {
    assert(argc==2);assert(ctl(17,1)==0);
    int fd=open(argv[1],O_RDONLY);assert(fd>=0);struct stat st;assert(fstat(fd,&st)==0 && st.st_size>=4096);
    volatile unsigned char* bytes=mmap(NULL,st.st_size,PROT_READ,MAP_PRIVATE,fd,0);assert((void*)bytes!=MAP_FAILED);
    unsigned sum=0;for(uint64_t at=0;at<(uint64_t)st.st_size;at+=4096)sum+=bytes[at];
    assert(munmap((void*)bytes,st.st_size)==0 && close(fd)==0);usleep(100000);
    long cached=ctl(16,0),oldReclaimed=ctl(16,2);assert(cached>0);
    enum { Chunk=4*1024*1024, Maximum=64 };void* held[Maximum]={0};unsigned count=0;
    while(ctl(3,0)>60*1024*1024/4096 && count<Maximum) {
        void* p=mmap(NULL,Chunk,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(p!=MAP_FAILED);held[count++]=p;
        for(unsigned at=0;at<Chunk;at+=4096)((volatile unsigned char*)p)[at]=0x5a;
    }
    assert(count<Maximum);usleep(100000);long before=ctl(3,0);assert(before<64*1024*1024/4096);
    assert(ctl(17,0)==0);long after=before,reclaimed=0,remaining=cached;
    for(unsigned n=0;n<500;++n) {
        usleep(10000);after=ctl(3,0);reclaimed=ctl(16,2)-oldReclaimed;remaining=ctl(16,0);
        if(reclaimed>0 && remaining<cached && after>=before+(reclaimed+1)/2)break;
    }
    assert(reclaimed>0 && remaining<cached && after>=before+(reclaimed+1)/2);
    for(unsigned i=0;i<count;++i)assert(munmap(held[i],Chunk)==0);
    printf("FILE_CACHE_PRESSURE_PASS cached_before=%ld cached_after=%ld reclaimed=%ld free_before=%ld free_after=%ld hog_mib=%u sample=%u\n",cached,remaining,reclaimed,before,after,count*4,sum);
}
