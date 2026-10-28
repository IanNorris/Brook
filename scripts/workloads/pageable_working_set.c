#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
static long ctl(long op,long value){return syscall(0xB40,op,value,0L,0L,0L,0L);}
static uint64_t ns(void){struct timespec t;assert(syscall(SYS_clock_gettime,CLOCK_MONOTONIC,&t)==0);return (uint64_t)t.tv_sec*1000000000+t.tv_nsec;}
static int order(const void* a,const void* b){uint64_t x=*(const uint64_t*)a,y=*(const uint64_t*)b;return (x>y)-(x<y);}
static unsigned char pattern(unsigned i,unsigned j){return (unsigned char)(i*31+j*73+(j>>8));}
static uint64_t samples[1024];
int main(int argc,char** argv) {
    enum { Pages=1024, Target=64, Bytes=Pages*4096 };
    int eager=argc==2 && !strcmp(argv[1],"--eager");
    int fd=open("/tmp/pageable-working-set-data",O_CREAT|O_TRUNC|O_RDWR,0600);assert(fd>=0);
    unsigned char seed[4096];memset(seed,79,sizeof(seed));assert(write(fd,seed,sizeof(seed))==sizeof(seed));
    assert(lseek(fd,0,SEEK_SET)==0);
    long base=ctl(eager?0:11,Bytes);assert(base>0);unsigned char* p=(void*)(uintptr_t)base;
    assert(ctl(13,0)==(eager?Pages:0) && ctl(13,1)==(eager?0:Pages) && ctl(2,0)==0);
    if(!eager) {
        // Eight sparse faults allocate only eight pages, without backing writes.
        for(unsigned i=0;i<8;++i)for(unsigned j=0;j<4096;++j)assert(((volatile unsigned char*)p)[i*64*4096+j]==0);
        assert(ctl(13,0)==8 && ctl(13,1)==Pages-8 && ctl(2,0)==0);
        // A failed zero publication stays Zero and returns its reserve frame.
        long reserve=ctl(7,0);
        assert(ctl(8,1)==0 && ctl(4,7)==0);assert(syscall(SYS_getpid)==-1 && errno==EIO);
        assert(ctl(13,0)==8 && ctl(13,1)==Pages-8 && ctl(7,0)==reserve);
        // Move one older sparse page out of the eight-page recent window.
        assert(((volatile unsigned char*)p)[4096]==0);
        assert(ctl(9,1)==0 && ctl(12,8)==0);
        const long failures[]={6,1,4};
        for(unsigned k=0;k<3;++k) {
            assert(ctl(4,failures[k])==0);
            assert(((volatile unsigned char*)p)[(k+2)*4096]==0);
            assert(ctl(13,0)==10+k && ctl(13,1)==Pages-10-k && ctl(2,0)==0);
            assert(ctl(13,3)==(k==0?1:0));
            for(unsigned i=0;i<8;++i)for(unsigned j=0;j<4096;++j)
                assert(((volatile unsigned char*)p)[i*64*4096+j]==0);
            assert(ctl(13,3)==0);
        }
        assert(ctl(12,Target)==0);
    }
    uint64_t start=ns();
    for(unsigned i=0;i<Pages;++i)for(unsigned j=0;j<4096;++j)p[i*4096+j]=pattern(i,j);
    uint64_t fill=ns()-start;
    long held=ctl(13,0),stored=ctl(2,0);assert(ctl(13,1)==0);
    assert(held==(eager?Pages:Target) && stored==(eager?0:Pages-Target));
    start=ns();
    for(unsigned i=0;i<Pages;++i) {
        uint64_t before=ns();
        for(unsigned j=0;j<4096;++j)assert(((volatile unsigned char*)p)[i*4096+j]==pattern(i,j));
        samples[i]=ns()-before;
    }
    uint64_t scan=ns()-start;
    assert(ctl(13,0)==held && ctl(2,0)==stored && ctl(6,0)==0);
    if(!eager) {
        // A lease covering all pages can exceed the soft target; syscall
        // preparation must not evict the earlier part of its own buffer.
        assert(read(fd,p,Bytes)==4096);
        assert(ctl(13,0)==Pages && ctl(2,0)==0 && ctl(6,0)==0);
        for(unsigned j=0;j<4096;++j)assert(p[j]==79);
        // Scalar time output is scoped too, preserving all other cold pages.
        assert(ctl(12,0)==0);
        assert(ctl(1,base)==4096 && ctl(1,base+4096)==4096);
        assert(ctl(2,0)==2);
        assert(syscall(SYS_clock_gettime,CLOCK_MONOTONIC,p)==0);
        assert(ctl(2,0)==1 && ctl(6,0)==0);
        (void)ns();assert(ctl(2,0)==1);
    }
    qsort(samples,Pages,sizeof(samples[0]),order);
    assert(ctl(12,0)==0 && ctl(9,0)==0);
    assert(munmap(p,Bytes)==0 && ctl(2,0)==0);
    assert(close(fd)==0 && unlink("/tmp/pageable-working-set-data")==0);
    printf("PAGEABLE_WORKING_SET_PASS mode=%s bytes=%u held_pages=%ld stored_pages=%ld fill_ns=%llu scan_ns=%llu page_verify_p50_ns=%llu p95_ns=%llu max_ns=%llu\n",eager?"eager":"soft",Bytes,held,stored,(unsigned long long)fill,(unsigned long long)scan,(unsigned long long)samples[Pages/2],(unsigned long long)samples[Pages*95/100],(unsigned long long)samples[Pages-1]);
}
