#define _GNU_SOURCE
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
static long ctl(long op,long value){return syscall(0xB40,op,value,0L,0L,0L,0L);}
static uint64_t ns(void){struct timespec t;assert(syscall(SYS_clock_gettime,CLOCK_MONOTONIC,&t)==0);return (uint64_t)t.tv_sec*1000000000+t.tv_nsec;}
int main(void){
    enum { A=1024,B=256,Stored=64,Queries=100 };
    long first=ctl(0,A*4096),second=ctl(0,B*4096);assert(first>0 && second>0);
    for(unsigned i=0;i<Stored;++i)assert(ctl(1,first+i*16*4096)==4096);
    for(unsigned i=0;i<A;i+=2)assert(mprotect((void*)(uintptr_t)(first+i*4096),4096,PROT_READ)==0);
    for(unsigned i=0;i<B;i+=2)assert(mprotect((void*)(uintptr_t)(second+i*4096),4096,PROT_READ)==0);
    uint64_t begin=ns();for(unsigned i=0;i<Queries;++i)assert(ctl(13,0)==A+B-Stored);uint64_t elapsed=ns()-begin;
    assert(ctl(2,0)==Stored && ctl(13,2)==Stored && ctl(13,1)==0);
    long child=syscall(SYS_fork);assert(child>=0);
    if(!child){
        assert(ctl(13,0)==A+B-Stored && ctl(13,2)==Stored);
        assert(madvise((void*)(uintptr_t)second,B*4096,MADV_DONTNEED)==0);
        assert(ctl(13,0)==A-Stored && ctl(13,1)==B);
        assert(madvise((void*)(uintptr_t)first,A*4096,MADV_DONTNEED)==0);
        assert(ctl(13,0)==0 && ctl(13,1)==A+B && ctl(13,2)==0);
        assert(munmap((void*)(uintptr_t)first,A*4096)==0 && munmap((void*)(uintptr_t)second,B*4096)==0);_exit(0);
    }
    assert(madvise((void*)(uintptr_t)first,A*4096,MADV_DONTNEED)==0);
    for(unsigned i=0;i<Queries;++i)assert(ctl(13,0)==B);
    assert(ctl(13,2)==0 && ctl(13,1)==A);
    assert(munmap((void*)(uintptr_t)second,B*4096)==0 && munmap((void*)(uintptr_t)first,A*4096)==0);
    int status;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
    assert(ctl(13,0)==0 && ctl(13,1)==0 && ctl(2,0)==0 && ctl(14,1)==0 && ctl(7,0)==8);
    printf("PAGEABLE_FRAGMENTS_PASS two_regions pages=%u repeated_queries=%u first_query_batch_ns=%llu fork independent_discard reset_marks exact_counts cleanup\n",A+B,Queries,(unsigned long long)elapsed);
}
