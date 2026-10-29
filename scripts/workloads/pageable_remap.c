#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
static long ctl(long op,long value){return syscall(0xB40,op,value,0L,0L,0L,0L);}
static unsigned char pattern(unsigned i,unsigned j){return (unsigned char)(i*31+j*73+(j>>8));}
static void counts(long held,long stored,long zero,long recovery){
    long h=ctl(13,0),s=ctl(13,2),z=ctl(13,1),r=ctl(13,3);
    if(h!=held || s!=stored || z!=zero || r!=recovery){fprintf(stderr,"remap counts actual=%ld/%ld/%ld/%ld expected=%ld/%ld/%ld/%ld\n",h,s,z,r,held,stored,zero,recovery);assert(0);}
}
static void reap(pid_t p){int s;assert(waitpid(p,&s,0)==p && WIFEXITED(s) && WEXITSTATUS(s)==0);}
static void drain(void){struct timespec delay={0,10000000};for(unsigned n=0;n<500 && ctl(2,0);++n)assert(nanosleep(&delay,0)==0);assert(ctl(2,0)==0 && ctl(14,1)==0);counts(0,0,0,0);}
static void shrink(void){
    unsigned char* p=(void*)(uintptr_t)ctl(11,8*4096);assert((intptr_t)p>0);
    for(unsigned j=0;j<4096;++j)p[j]=0x6b;
    uint32_t random=0x45b1a9u;for(unsigned j=0;j<4096;++j){random^=random<<13;random^=random>>17;random^=random<<5;p[6*4096+j]=(unsigned char)random;}
    assert(ctl(1,(long)(uintptr_t)p)==4096 && ctl(4,6)==0 && ctl(1,(long)(uintptr_t)(p+6*4096))==-1 && errno==EIO);counts(1,1,6,1);
    long dr=ctl(14,13),dw=ctl(14,12),rr=ctl(14,15),rw=ctl(14,14);
    assert(mremap(p,8*4096,4*4096,0)==p);counts(0,1,3,0);
    assert(mremap(p,4*4096,4*4096,MREMAP_MAYMOVE)==p);
    errno=0;assert(mremap(p,4*4096,32*1024*1024,MREMAP_MAYMOVE)==MAP_FAILED && errno==ENOMEM);counts(0,1,3,0);
    assert(ctl(14,13)==dr && ctl(14,12)==dw && ctl(14,15)==rr && ctl(14,14)==rw);
    for(unsigned j=0;j<4096;++j)assert(((volatile unsigned char*)p)[j]==0x6b);
    assert(munmap(p,4*4096)==0);drain();
}
static void mixed_move(void){
    int done[2];assert(pipe(done)==0);
    unsigned char* p=(void*)(uintptr_t)ctl(11,16*4096),*q=(void*)(uintptr_t)ctl(11,16*4096);assert((intptr_t)p>0 && (intptr_t)q>0);
    for(unsigned i=0;i<8;++i)for(unsigned j=0;j<4096;++j)p[i*4096+j]=pattern(i,j);
    uint32_t random=0x45b1a9u;for(unsigned j=0;j<4096;++j){random^=random<<13;random^=random>>17;random^=random<<5;p[4096+j]=(unsigned char)random;}
    for(unsigned i=0;i<8;i+=2)assert(ctl(1,(long)(uintptr_t)(p+i*4096))==4096);
    assert(ctl(4,6)==0 && ctl(1,(long)(uintptr_t)(p+4096))==-1 && errno==EIO);
    q[14*4096]=0xee;assert(mprotect(p,16*4096,PROT_NONE)==0);counts(5,4,23,1);
    long dr=ctl(14,13),dw=ctl(14,12),rr=ctl(14,15),rw=ctl(14,14);
    assert(mremap(p,16*4096,6*4096,MREMAP_MAYMOVE|MREMAP_FIXED,q)==q);counts(4,3,9,1);
    assert(ctl(14,13)==dr && ctl(14,12)==dw && ctl(14,15)==rr && ctl(14,14)==rw);
    assert(mprotect(q,6*4096,PROT_READ|PROT_WRITE)==0);
    for(unsigned i=0;i<6;++i){random=0x45b1a9u;for(unsigned j=0;j<4096;++j){unsigned char e=pattern(i,j);if(i==1){random^=random<<13;random^=random>>17;random^=random<<5;e=(unsigned char)random;}assert(((volatile unsigned char*)q)[i*4096+j]==e);}}
    assert(q[14*4096]==0xee);counts(7,0,9,0);
    // Move one sparse fragment far from its siblings, then fork that layout.
    enum { Big=16*1024*1024,At=8*1024*1024 };
    unsigned char* big=(void*)(uintptr_t)ctl(11,Big);assert((intptr_t)big>0);
    for(unsigned j=0;j<4096;++j)big[At+j]=0x77;
    assert(mprotect(big+At,4096,PROT_READ)==0 && ctl(1,(long)(uintptr_t)(big+At))==4096);
    unsigned char* low=mmap(0,4096,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(low!=MAP_FAILED);
    assert(mremap(big+At,4096,4096,MREMAP_MAYMOVE|MREMAP_FIXED,low)==low);
    long child=syscall(SYS_fork);assert(child>=0);
    if(!child){
        assert(mprotect(low,4096,PROT_READ|PROT_WRITE)==0);for(unsigned j=0;j<4096;++j)assert(((volatile unsigned char*)low)[j]==0x77);low[0]=0xa4;
        assert(munmap(low,4096)==0 && munmap(big,Big)==0 && munmap(q,16*4096)==0);
        char c='x';assert(write(done[1],&c,1)==1);_exit(0);
    }
    char c;assert(read(done[0],&c,1)==1 && c=='x');for(unsigned j=0;j<4096;++j)assert(((volatile unsigned char*)low)[j]==0x77);
    assert(munmap(low,4096)==0 && munmap(big,Big)==0 && munmap(q,16*4096)==0);reap(child);
    assert(close(done[0])==0 && close(done[1])==0);drain();
}
static void same_backing(void){
    unsigned char* p=(void*)(uintptr_t)ctl(0,8*4096);assert((intptr_t)p>0);
    for(unsigned i=0;i<8;++i)for(unsigned j=0;j<4096;++j)p[i*4096+j]=pattern(i,j);
    assert(mprotect(p,2*4096,PROT_READ)==0 && ctl(1,(long)(uintptr_t)p)==4096 && ctl(1,(long)(uintptr_t)(p+4096))==4096);
    assert(ctl(1,(long)(uintptr_t)(p+4*4096))==4096 && ctl(1,(long)(uintptr_t)(p+5*4096))==4096);
    assert(mremap(p,2*4096,2*4096,MREMAP_MAYMOVE|MREMAP_FIXED,p+4*4096)==p+4*4096);counts(4,2,0,0);
    long child=syscall(SYS_fork);assert(child>=0);if(!child){*(volatile unsigned char*)p=1;_exit(99);}
    int status;assert(waitpid(child,&status,0)==child && WIFSIGNALED(status) && WTERMSIG(status)==SIGSEGV);
    assert(mprotect(p+4*4096,2*4096,PROT_READ|PROT_WRITE)==0);
    for(unsigned i=2;i<8;++i){unsigned old=i==4?0:i==5?1:i;for(unsigned j=0;j<4096;++j)assert(((volatile unsigned char*)p)[i*4096+j]==pattern(old,j));}
    assert(munmap(p,8*4096)==0);drain();
}
static void ordinary_overwrite(void){
    unsigned char* p=(void*)(uintptr_t)ctl(0,8*4096);assert((intptr_t)p>0);
    for(unsigned j=0;j<8*4096;++j)p[j]=0x22;
    uint32_t random=0x45b1a9u;for(unsigned j=0;j<4096;++j){random^=random<<13;random^=random>>17;random^=random<<5;p[2*4096+j]=(unsigned char)random;}
    assert(ctl(1,(long)(uintptr_t)(p+4096))==4096 && ctl(1,(long)(uintptr_t)(p+3*4096))==4096);
    assert(ctl(4,6)==0 && ctl(1,(long)(uintptr_t)(p+2*4096))==-1 && errno==EIO);counts(6,2,0,1);
    unsigned char* ordinary=mmap(0,4*4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(ordinary!=MAP_FAILED);
    for(unsigned j=0;j<4*4096;++j)ordinary[j]=0xcc;
    assert(mremap(ordinary,4*4096,4*4096,MREMAP_MAYMOVE|MREMAP_FIXED,p)==p);counts(4,0,0,0);
    for(unsigned j=0;j<8*4096;++j)assert(((volatile unsigned char*)p)[j]==(j<4*4096?0xcc:0x22));
    assert(madvise(p,8*4096,MADV_DONTNEED)==0 && munmap(p,8*4096)==0);drain();
}
int main(void){counts(0,0,0,0);shrink();mixed_move();same_backing();ordinary_overwrite();puts("PAGEABLE_REMAP_PASS in_place_shrink shrink_fixed_move hidden_Stored_Recovery_Zero sparse_fragment_fork same_backing_stored_overwrite ordinary_overwrite source_hole counters cleanup");}
