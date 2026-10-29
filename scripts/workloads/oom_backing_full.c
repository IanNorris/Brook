#define _GNU_SOURCE
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
/* Disposable 4 MiB page-store test only, run as guest root. Hold all logical
 * slots occupied while a separate ordinary-user process exhausts physical RAM.
 * clock_gettime is an audited scalar syscall: sleeping would restore these
 * opted-in mappings through the conservative raw-user-access barrier. */
static long ctl(long op,long value){return syscall(0xB40,op,value,0L,0L,0L,0L);}
static uint64_t ms(void){struct timespec t;assert(clock_gettime(CLOCK_MONOTONIC,&t)==0);return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
static void pattern(unsigned page,unsigned char *bytes){
    uint32_t state=page+0x6c8e9cf5u;
    for(unsigned j=0;j<2048;++j){state^=state<<13;state^=state>>17;state^=state<<5;bytes[j]=state;}
    for(unsigned j=2048;j<4096;++j)bytes[j]=bytes[j-2048];
}
int main(void){
    enum {Bytes=4*1024*1024,Pages=Bytes/4096};
    setvbuf(stdout,NULL,_IONBF,0);assert(ctl(2,0)==0);
    long address=ctl(0,Bytes);assert(address>0);unsigned char *p=(void*)(uintptr_t)address;
    unsigned char expected[4096];
    for(unsigned i=0;i<Pages;++i){
        pattern(i,expected);
        for(unsigned j=0;j<4096;++j)p[i*4096+j]=expected[j];
        assert(ctl(1,(long)(p+i*4096))==4096);
    }
    assert(ctl(2,0)==Pages);
    long ram=ctl(14,3),disk=ctl(14,4);assert(ram>0 && disk>0 && ram+disk==Pages);
    char text[160];int count=snprintf(text,sizeof(text),"OOM_BACKING_FULL_READY slots=%u ram_slots=%ld disk_slots=%ld\n",Pages,ram,disk);
    /* stdio may use writev, which intentionally triggers the experimental
     * pager's raw-uaccess restore barrier. Scalar write preserves the slots. */
    assert(count>0 && count<(int)sizeof(text) && syscall(SYS_write,1,text,count)==count);
    uint64_t deadline=ms()+60000;
    while(ms()<deadline)__asm__ volatile("pause");
    assert(ctl(2,0)==Pages);
    for(unsigned i=0;i<Pages;++i){pattern(i,expected);for(unsigned j=0;j<4096;++j)assert(((volatile unsigned char*)p)[i*4096+j]==expected[j]);}
    assert(munmap(p,Bytes)==0 && ctl(2,0)==0);
    puts("OOM_BACKING_FULL_INTEGRITY_PASS");return 0;
}
