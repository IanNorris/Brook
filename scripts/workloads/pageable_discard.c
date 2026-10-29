#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
static long ctl(long op,long value){return syscall(0xB40,op,value,0L,0L,0L,0L);}
static unsigned char pattern(unsigned i,unsigned j){return (unsigned char)(i*31+j*73+(j>>8));}
static void counts(long held,long stored,long zero,long recovery){
    long h=ctl(13,0),s=ctl(2,0),s2=ctl(13,2),z=ctl(13,1),r=ctl(13,3);
    if(h!=held || s!=stored || s2!=stored || z!=zero || r!=recovery){
        fprintf(stderr,"discard counts actual=%ld/%ld/%ld/%ld/%ld expected=%ld/%ld/%ld/%ld\n",h,s,s2,z,r,held,stored,zero,recovery);assert(0);
    }
}
static void zero(volatile unsigned char* p,unsigned pages){for(unsigned j=0;j<pages*4096;++j)assert(p[j]==0);}
int main(void){
    counts(0,0,0,0);long base=ctl(11,16*4096);assert(base>0);unsigned char* p=(void*)(uintptr_t)base;
    for(unsigned i=0;i<8;++i)for(unsigned j=0;j<4096;++j)p[i*4096+j]=pattern(i,j);
    uint32_t random=0x45b1a9u;
    for(unsigned j=0;j<4096;++j){random^=random<<13;random^=random>>17;random^=random<<5;p[4096+j]=(unsigned char)random;}
    for(unsigned i=0;i<8;i+=2)assert(ctl(1,base+i*4096)==4096);
    assert(ctl(4,6)==0);errno=0;assert(ctl(1,base+4096)==-1 && errno==EIO);
    counts(4,4,8,1);assert(mprotect(p,4*4096,PROT_NONE)==0);
    long dr=ctl(14,13),rr=ctl(14,15),dw=ctl(14,12),rw=ctl(14,14);
    assert(ctl(4,2)==0 && madvise(p,5*4096,MADV_DONTNEED)==0);
    counts(2,1,13,0);
    assert(ctl(14,13)==dr && ctl(14,15)==rr && ctl(14,12)==dw && ctl(14,14)==rw);
    assert(ctl(4,1)==0 && madvise(p,5*4096,MADV_DONTNEED)==0);
    counts(2,1,13,0);assert(ctl(14,12)==dw && ctl(14,14)==rw);assert(ctl(4,0)==0);
    // Discarded bytes fault back as zero, including a reserve-backed refault.
    assert(ctl(8,1)==0);zero(p+4*4096,1);assert(ctl(7,0)==7);counts(3,1,12,0);
    assert(ctl(1,base+4*4096)==4096 && ctl(7,0)==8);
    assert(madvise(p+4*4096,4096,MADV_DONTNEED)==0);counts(2,1,13,0);
    // One request spans both pager reservations and ordinary anonymous pages.
    assert(mmap(p+12*4096,4*4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0)==p+12*4096);
    for(unsigned j=12*4096;j<16*4096;++j)p[j]=0x7a;
    counts(2,1,9,0);assert(madvise(p+8*4096,8*4096,MADV_DONTNEED)==0);counts(2,1,9,0);
    zero(p+8*4096,8);counts(6,1,5,0);
    for(unsigned i=5;i<8;++i)for(unsigned j=0;j<4096;++j)assert(((volatile unsigned char*)p)[i*4096+j]==pattern(i,j));
    counts(7,0,5,0);
    assert(mprotect(p,4*4096,PROT_READ|PROT_WRITE)==0);zero(p,5);counts(12,0,0,0);
    // Validate complete coverage before changing any page in a failed request.
    assert(munmap(p+9*4096,4096)==0 && ctl(1,base+6*4096)==4096);counts(10,1,0,0);
    for(unsigned j=0;j<4096;++j){p[8*4096+j]=0xc3;p[10*4096+j]=0xd7;}
    errno=0;assert(madvise(p+8*4096,3*4096,MADV_DONTNEED)==-1 && errno==ENOMEM);counts(10,1,0,0);
    for(unsigned j=0;j<4096;++j)assert(p[8*4096+j]==0xc3 && p[10*4096+j]==0xd7);
    for(unsigned j=0;j<4096;++j)assert(((volatile unsigned char*)p)[6*4096+j]==pattern(6,j));
    assert(munmap(p,16*4096)==0);counts(0,0,0,0);assert(ctl(14,1)==0 && ctl(14,2)==0 && ctl(7,0)==8);
    puts("PAGEABLE_DISCARD_PASS resident_stored_Recovery_Zero hidden mixed_anonymous no_backing_IO refault_zero hole_atomic cleanup");
}
