#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
enum {Bytes=8*1024*1024+123,Rounds=600};
static uint64_t ns(void){struct timespec t;assert(clock_gettime(CLOCK_MONOTONIC,&t)==0);return (uint64_t)t.tv_sec*1000000000+t.tv_nsec;}
static uint32_t next(uint32_t *state){return *state=*state*1664525u+1013904223u;}
static void put(int fd,const unsigned char* p,size_t n,off_t at){while(n){ssize_t r=pwrite(fd,p,n,at);if(r<0&&errno==EINTR)continue;assert(r>0);p+=r;n-=r;at+=r;}}
int main(int argc,char **argv){
    assert(argc==2);setvbuf(stdout,NULL,_IONBF,0);
    unsigned char *expected=malloc(Bytes),*patch=malloc(262181);assert(expected&&patch);
    for(unsigned i=0;i<Bytes;++i)expected[i]=(unsigned char)(i*13+(i>>9)*17+0x53);
    int fd=open(argv[1],O_RDWR|O_CREAT|O_EXCL,0600);assert(fd>=0);
    uint64_t start=ns();put(fd,expected,Bytes,0);assert(fsync(fd)==0);
    static const unsigned lengths[]={1,127,128,511,512,513,4095,4096,4097,65535,65549,262181};
    uint32_t state=0x42726f6f;unsigned char sample[4096];
    for(unsigned round=0;round<Rounds;++round){
        unsigned length=lengths[next(&state)%12],offset=next(&state)%(Bytes-length+1);
        for(unsigned j=0;j<length;++j)patch[j]=(unsigned char)(round*29+(j>>8)+j*7);
        put(fd,patch,length,offset);memcpy(expected+offset,patch,length);
        if(!(round%17))assert(fsync(fd)==0);
        unsigned at=next(&state)%(Bytes-sizeof(sample));assert(pread(fd,sample,sizeof(sample),at)==sizeof(sample));
        assert(!memcmp(sample,expected+at,sizeof(sample)));
    }
    assert(fsync(fd)==0&&close(fd)==0);fd=open(argv[1],O_RDONLY);assert(fd>=0);
    for(unsigned at=0;at<Bytes;){unsigned count=Bytes-at;if(count>sizeof(sample))count=sizeof(sample);assert(pread(fd,sample,count,at)==count);assert(!memcmp(sample,expected+at,count));at+=count;}
    assert(close(fd)==0);uint64_t digest=14695981039346656037ULL;
    for(unsigned i=0;i<Bytes;++i){digest^=expected[i];digest*=1099511628211ULL;}
    printf("PARTIAL_WRITE_NATIVE_PASS bytes=%u operations=%u fnv64=%016llx elapsed_ns=%llu\n",Bytes,Rounds,(unsigned long long)digest,(unsigned long long)(ns()-start));
    free(patch);free(expected);return 0;
}
