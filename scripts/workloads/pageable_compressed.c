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
static void verify(volatile unsigned char* p,unsigned i){for(unsigned j=0;j<4096;++j)assert(p[j]==pattern(i,j));}
int main(void) {
    enum { Pages=12, Reserve=8 };
    assert(ctl(2,0)==0 && ctl(14,0)>0 && ctl(14,1)==0 && ctl(14,2)==0 && ctl(7,0)==Reserve);
    long base=ctl(0,Pages*4096);assert(base>0);unsigned char* p=(void*)(uintptr_t)base;
    for(unsigned i=0;i<Pages;++i)for(unsigned j=0;j<4096;++j)p[i*4096+j]=pattern(i,j);
    for(unsigned i=0;i<Pages;++i)assert(ctl(1,base+i*4096)==4096);
    assert(ctl(2,0)==Pages && ctl(14,3)==Pages && ctl(14,4)==0 && ctl(14,1)<Pages*4096);
    long allocated=ctl(14,1),payload=ctl(14,2),spills=ctl(14,8);
    // Failed and short spills retain the compressed source and same stored slot.
    const long fail[]={1,4};
    for(unsigned i=0;i<2;++i) {
        assert(ctl(4,fail[i])==0);assert(ctl(15,base)==-1 && errno==EIO);
        assert(ctl(14,3)==Pages && ctl(14,4)==0 && ctl(14,1)==allocated && ctl(14,2)==payload);
    }
    assert(ctl(15,base)==0 && ctl(14,3)==Pages-1 && ctl(14,4)==1 && ctl(14,8)==spills+1);
    // Raw read failure on the explicitly spilled first page returns its reserve;
    // both compressed peers and the original disk token remain owned.
    const long reads[]={2,3,5,7};
    for(unsigned i=0;i<4;++i) {
        assert(ctl(8,1)==0 && ctl(4,reads[i])==0);
        assert(syscall(SYS_getpid)==-1 && errno==EIO);
        assert(ctl(2,0)==Pages && ctl(7,0)==Reserve && ctl(14,3)==Pages-1);
    }
    for(unsigned i=0;i<Reserve;++i){assert(ctl(8,1)==0);verify(p+i*4096,i);}
    for(unsigned i=Reserve;i<Pages;++i)verify(p+i*4096,i);
    assert(ctl(2,0)==0 && ctl(14,1)==0 && ctl(14,2)==0 && ctl(14,3)==0 && ctl(14,4)==0);
    // Fault-driven eviction/refill uses the same compressed token ownership.
    assert(ctl(9,1)==0);
    for(unsigned i=0;i<Pages;++i)assert(ctl(1,base+i*4096)==4096);
    for(unsigned round=0;round<3;++round)for(unsigned i=0;i<Pages;++i) {
        assert(ctl(8,1)==0);verify(p+i*4096,i);assert(ctl(8,0)==0);
    }
    assert(ctl(9,0)==0);
    for(unsigned i=0;i<Pages;++i) {
        errno=0;long result=ctl(1,base+i*4096);assert(result==4096 || (result==-1 && errno==EBUSY));
    }
    assert(ctl(7,0)==Reserve && munmap(p,Pages*4096)==0);
    assert(ctl(2,0)==0 && ctl(14,1)==0 && ctl(14,2)==0 && ctl(14,3)==0 && ctl(14,4)==0);
    puts("PAGEABLE_COMPRESSED_PASS RAM_disk failed_short_spill forced_oom_reclaim read_errors byte_integrity clean_pool");
}
