#define _GNU_SOURCE
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
static long ctl(long op,long value){return syscall(0xB40,op,value,0L,0L,0L,0L);}
static unsigned char pattern(unsigned page,unsigned at){return page*31+at*73+(at>>8);}
static void verify(volatile unsigned char* p,unsigned i){for(unsigned j=0;j<4096;++j)assert(p[j]==pattern(i,j));}
static unsigned char* prepare(void){
    long base=ctl(0,8*4096);assert(base>0);unsigned char* p=(void*)(uintptr_t)base;
    for(unsigned i=0;i<8;++i)for(unsigned j=0;j<4096;++j)p[i*4096+j]=pattern(i,j);
    for(unsigned i=0;i<8;++i)assert(ctl(1,base+i*4096)==4096);
    assert(ctl(2,0)==8 && ctl(7,0)==8);return p;
}
static void borrow(unsigned char* p,unsigned count){
    for(unsigned i=0;i<count;++i){assert(ctl(8,1)==0);verify(p+i*4096,i);}
    assert(ctl(8,0)==0);
}
static void await_refill(long baseline,unsigned count){
    for(unsigned n=0;n<500;++n){
        if(ctl(7,0)==8 && ctl(18,0)>=baseline+count)return;
        usleep(10000);
    }
    assert(!"reserve worker did not restore healthy reserve within five seconds");
}
int main(int argc,char** argv){
    assert(ctl(2,0)==0 && ctl(7,0)==8);
    if(argc==2 && !strcmp(argv[1],"--disabled")){
        assert(ctl(18,3)==0);unsigned char* p=prepare();borrow(p,2);usleep(250000);
        assert(ctl(7,0)==6 && ctl(18,0)==0);
        for(unsigned i=0;i<2;++i)assert(ctl(1,(long)(uintptr_t)(p+i*4096))==4096);
        assert(ctl(7,0)==8 && munmap(p,8*4096)==0 && ctl(2,0)==0);
        puts("PAGEABLE_REFILL_DISABLED_PASS deterministic_reserve explicit_replenish");return 0;
    }
    assert(argc==1 && ctl(18,3)==1);
    for(unsigned round=0;round<3;++round){
        unsigned char* p=prepare();long baseline=ctl(18,0);borrow(p,8);
        assert(ctl(2,0)==0);await_refill(baseline,8);
        // The original loans stay resident while distinct worker frames refill
        // the pool. A later unmap must not return the same ownership twice.
        for(unsigned i=0;i<8;++i)verify(p+i*4096,i);
        assert(munmap(p,8*4096)==0 && ctl(2,0)==0 && ctl(7,0)==8);
    }
    unsigned char* p;enum { Chunk=4*1024*1024,Maximum=64 };void* hogs[Maximum];unsigned count=0;
    while(ctl(3,0)>48*1024*1024/4096 && count<Maximum){
        void* h=mmap(NULL,Chunk,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(h!=MAP_FAILED);
        memset(h,0x5a,Chunk);hogs[count++]=h;
    }
    assert(count<Maximum && ctl(3,0)<64*1024*1024/4096);
    p=prepare(); // mmap is conservatively prefaulting: make cold pages after hog setup.
    long baseline=ctl(18,0),skips=ctl(18,1);borrow(p,8);usleep(250000);
    assert(ctl(7,0)==0 && ctl(18,0)==baseline && ctl(18,1)>skips);
    for(unsigned h=0;h<count;++h){
        for(unsigned at=0;at<Chunk;at+=4096)assert(((unsigned char*)hogs[h])[at]==0x5a);
        assert(munmap(hogs[h],Chunk)==0);
    }
    await_refill(baseline,8);
    for(unsigned i=0;i<8;++i)verify(p+i*4096,i);
    assert(munmap(p,8*4096)==0 && ctl(2,0)==0 && ctl(7,0)==8);
    printf("PAGEABLE_REFILL_PASS healthy_cycles=3 low_headroom_suppression recovery_after_unmap hog_mib=%u refilled=%ld low_skips=%ld slots=0 reserve=8\n",count*4,ctl(18,0),ctl(18,1));
}
