#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
static long ctl(long op,long value){return syscall(0xB40,op,value,0L,0L,0L,0L);}
static unsigned char pattern(unsigned page,unsigned at){return (unsigned char)(page*31+at*73+(at>>8));}
static void verify(unsigned char* p,unsigned i){for(unsigned j=0;j<4096;++j)assert(p[j]==pattern(i,j));}
int main(void) {
    enum { Pages=12, Reserve=8 };
    assert(ctl(2,0)==0 && ctl(7,0)==Reserve);
    long base=ctl(0,Pages*4096);assert(base>0);unsigned char* p=(unsigned char*)(uintptr_t)base;
    for(unsigned i=0;i<Pages;++i)for(unsigned j=0;j<4096;++j)p[i*4096+j]=pattern(i,j);
    for(unsigned i=0;i<Pages;++i)assert(ctl(1,base+i*4096)==4096);
    assert(ctl(2,0)==Pages && ctl(7,0)==Reserve);
    // Ordinary allocation fails, checkout occurs, then read/checksum/short-read
    // or publication fails. Transaction unwinds before checkout returns its ref.
    const long failures[]={2,3,5,7};
    for(unsigned i=0;i<4;++i) {
        assert(ctl(8,1)==0 && ctl(4,failures[i])==0);
        assert(syscall(SYS_getpid)==-1 && errno==EIO);
        assert(ctl(2,0)==Pages && ctl(7,0)==Reserve);
    }
    for(unsigned i=0;i<Reserve;++i) {
        assert(ctl(8,1)==0);verify(p+i*4096,i);
        assert(ctl(7,0)==Reserve-i-1);
    }
    assert(ctl(8,1)==0);assert(syscall(SYS_getpid)==-1 && errno==ENOMEM);
    assert(ctl(2,0)==Pages-Reserve && ctl(7,0)==0);
    // Progress after exhaustion: a successfully stored resident victim provides
    // a reusable detached frame; no free-and-reallocate shortcut is involved.
    assert(ctl(1,base)==4096 && ctl(7,0)==1);
    assert(ctl(8,1)==0);verify(p+Reserve*4096,Reserve);assert(ctl(7,0)==0);
    for(unsigned i=1;i<=Reserve;++i)assert(ctl(1,base+i*4096)==4096);
    assert(ctl(7,0)==Reserve && ctl(2,0)==Pages);
    for(unsigned i=0;i<Pages;++i)verify(p+i*4096,i);
    for(unsigned round=0;round<8;++round) {
        for(unsigned i=0;i<Pages;++i)assert(ctl(1,base+i*4096)==4096);
        for(unsigned i=0;i<Reserve;++i){assert(ctl(8,1)==0);verify(p+i*4096,i);}
        for(unsigned i=Reserve;i<Pages;++i)verify(p+i*4096,i);
        assert(ctl(7,0)==0 && ctl(2,0)==0);
        for(unsigned i=0;i<Reserve;++i)assert(ctl(1,base+i*4096)==4096);
        assert(ctl(7,0)==Reserve);
        for(unsigned i=0;i<Reserve;++i)verify(p+i*4096,i);
    }
    assert(munmap(p,Pages*4096)==0);assert(ctl(2,0)==0 && ctl(7,0)==Reserve);
    puts("PAGEABLE_RESERVE_PASS forced_oom read/checksum/short/publication_failure exhaustion/refill cycles=8 clean_slots");
}
