#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
static long ctl(long op,long value){return syscall(0xB40,op,value,0L,0L,0L,0L);}
static sigjmp_buf resume;
static volatile sig_atomic_t expect_fault;
static void fault(int signal){assert(signal==SIGSEGV && expect_fault);expect_fault=0;siglongjmp(resume,1);}
static unsigned char pattern(unsigned page,unsigned at){return (unsigned char)(page*31+at*73+(at>>8));}
static void verify(volatile unsigned char* p,unsigned page){for(unsigned j=0;j<4096;++j)assert(p[j]==pattern(page,j));}
static void counts(long held,long stored,long zero,long recovery){
    static unsigned step;
    long h=ctl(13,0),s=ctl(2,0),s2=ctl(13,2),z=ctl(13,1),r=ctl(13,3),e=ctl(13,4);++step;
    if(h!=held || s!=stored || s2!=stored || z!=zero || r!=recovery || e) {
        fprintf(stderr,"counts step=%u actual=%ld/%ld/%ld/%ld/%ld/%ld expected=%ld/%ld/%ld/%ld\n",step,h,s,s2,z,r,e,held,stored,zero,recovery);
        assert(!"pager counts mismatch");
    }
}
static void denied(volatile unsigned char* p,int write){
    if(!sigsetjmp(resume,1)){expect_fault=1;if(write)*p=99;else (void)*p;assert(!"access unexpectedly permitted");}
    assert(!expect_fault);
}
int main(void){
    struct sigaction action={0};action.sa_handler=fault;sigemptyset(&action.sa_mask);assert(sigaction(SIGSEGV,&action,0)==0);
    counts(0,0,0,0);
    long base=ctl(11,16*4096);assert(base>0);unsigned char* p=(void*)(uintptr_t)base;
    for(unsigned i=0;i<12;++i)for(unsigned j=0;j<4096;++j)p[i*4096+j]=pattern(i,j);
    // Force raw backing for the rollback-failure control even with RAM LZ4 on.
    uint32_t random=0x45b1a9u;
    for(unsigned j=0;j<4096;++j){random^=random<<13;random^=random>>17;random^=random<<5;p[4096+j]=(unsigned char)random;}
    for(unsigned i=0;i<12;i+=2)assert(ctl(1,base+i*4096)==4096);
    // A failed write plus failed PTE rollback leaves the only valid frame in
    // Recovery; partial removal must retire that reference as well as slots.
    assert(ctl(4,6)==0);errno=0;assert(ctl(1,base+4096)==-1 && errno==EIO);
    counts(6,6,4,1);
    assert(mprotect(p,4*4096,PROT_NONE)==0);counts(6,6,4,1);
    assert(munmap(p+4096,4096)==0);counts(5,6,4,0);
    assert(munmap(p+4*4096,3*4096)==0);counts(4,4,4,0);
    assert(mmap(p+8*4096,4*4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0)==p+8*4096);
    counts(2,2,4,0);
    for(unsigned j=0;j<4*4096;++j)assert(p[8*4096+j]==0);
    assert(mprotect(p,4096,PROT_READ)==0);verify(p,0);counts(3,1,4,0);
    assert(mprotect(p+2*4096,4096,PROT_READ)==0);verify(p+2*4096,2);counts(4,0,4,0);
    assert(mprotect(p+3*4096,4096,PROT_READ|PROT_WRITE)==0);verify(p+3*4096,3);verify(p+7*4096,7);
    // The unaudited-syscall barrier skips inaccessible fragments instead of
    // trying to read them. Restore the permission afterward without data loss.
    assert(ctl(1,base+2*4096)==4096);
    assert(mprotect(p+2*4096,4096,PROT_NONE)==0);
    assert(syscall(SYS_getpid)>0 && ctl(2,0)==1);
    denied(p,1);denied(p+4096,0);denied(p+2*4096,0);
    assert(mprotect(p+2*4096,4096,PROT_READ)==0);verify(p+2*4096,2);
    for(unsigned i=12;i<16;++i)for(unsigned j=0;j<4096;++j)assert(p[i*4096+j]==0);
    assert(munmap(p+12*4096,4*4096)==0);
    assert(munmap(p+2*4096,2*4096)==0);
    assert(munmap(p+7*4096,4096)==0);
    assert(munmap(p,4096)==0);
    assert(munmap(p+8*4096,4*4096)==0);
    counts(0,0,0,0);assert(ctl(14,1)==0 && ctl(14,2)==0 && ctl(7,0)==8);
    puts("PAGEABLE_LIFECYCLE_PASS split_unmap fixed_replacement protection hidden_barrier Recovery_retirement slots cleanup");
}
