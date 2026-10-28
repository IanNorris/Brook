#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
static long ctl(long op,long value){return syscall(0xB40,op,value,0L,0L,0L,0L);}
static unsigned char value(unsigned page,unsigned byte){return page*31+byte*73+(byte>>8);}
static void verify(volatile unsigned char* p,unsigned page){for(unsigned j=0;j<4096;++j)assert(p[j]==value(page,j));}
int main(void){
    enum { Pages=12,Reserve=8 };assert(ctl(2,0)==0 && ctl(7,0)==Reserve);
    char name[]="/tmp/page-handoff-XXXXXX";int fd=mkstemp(name);assert(fd>=0 && unlink(name)==0);
    long base=ctl(0,Pages*4096);assert(base>0);unsigned char* p=(void*)(uintptr_t)base;
    for(unsigned i=0;i<Pages;++i)for(unsigned j=0;j<4096;++j)p[i*4096+j]=value(i,j);
    for(unsigned i=0;i<Pages;++i)assert(ctl(1,base+i*4096)==4096);
    for(unsigned i=0;i<Reserve;++i){assert(ctl(8,1)==0);verify(p+i*4096,i);}
    assert(ctl(7,0)==0 && ctl(9,1)==0);
    // No emergency frames remain. A retired same-AS victim supplies the fault.
    assert(ctl(8,1)==0);verify(p+Reserve*4096,Reserve);
    assert(ctl(7,0)==0 && ctl(2,0)==Pages-Reserve);
    // Storage of the next victim succeeds, but publication of the target fails.
    // The handed-off frame is transaction-owned and must be retired once;
    // both stored byte copies remain authoritative and the lease is released.
    assert(ctl(8,1)==0 && ctl(4,7)==0);
    assert(write(fd,p+(Pages-1)*4096,4096)==-1 && errno==EFAULT);
    assert(ctl(2,0)==Pages-Reserve+1 && ctl(7,0)==0 && ctl(6,0)==0);
    assert(ctl(8,0)==0 && ctl(9,0)==0);
    for(unsigned i=0;i<Pages;++i)verify(p+i*4096,i);
    for(unsigned i=0;i<Pages;++i)assert(ctl(1,base+i*4096)==4096);
    assert(ctl(7,0)==Reserve && munmap(p,Pages*4096)==0 && ctl(2,0)==0 && close(fd)==0);
    puts("PAGEABLE_HANDOFF_PASS empty_reserve victim_reuse publication_failure retained_bytes cleanup");
}
