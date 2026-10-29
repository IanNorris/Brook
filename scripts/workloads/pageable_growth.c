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
    if(h!=held || s!=stored || z!=zero || r!=recovery){fprintf(stderr,"growth counts actual=%ld/%ld/%ld/%ld expected=%ld/%ld/%ld/%ld\n",h,s,z,r,held,stored,zero,recovery);assert(0);}
}
static void reap(pid_t p){int s;assert(waitpid(p,&s,0)==p && WIFEXITED(s) && WEXITSTATUS(s)==0);}
static void send_byte(int fd){char c='x';assert(write(fd,&c,1)==1);}
static void receive(int fd){char c;assert(read(fd,&c,1)==1 && c=='x');}
static void drain(void){struct timespec delay={0,10000000};for(unsigned n=0;n<500 && ctl(2,0);++n)assert(nanosleep(&delay,0)==0);assert(ctl(2,0)==0 && ctl(14,1)==0 && ctl(7,0)==8);counts(0,0,0,0);}
static void random_page(unsigned char* p){uint32_t r=0x45b1a9u;for(unsigned j=0;j<4096;++j){r^=r<<13;r^=r>>17;r^=r<<5;p[j]=(unsigned char)r;}}
static void check_random(volatile unsigned char* p){uint32_t r=0x45b1a9u;for(unsigned j=0;j<4096;++j){r^=r<<13;r^=r>>17;r^=r<<5;assert(p[j]==(unsigned char)r);}}
static void failures(void){
    unsigned char* p=(void*)(uintptr_t)ctl(11,4*4096);assert((intptr_t)p>0);
    for(unsigned j=0;j<4096;++j)p[j]=0x3c;
    random_page(p+4096);
    assert(ctl(1,(long)(uintptr_t)p)==4096 && ctl(4,6)==0 && ctl(1,(long)(uintptr_t)(p+4096))==-1 && errno==EIO);counts(1,1,2,1);
    long dw=ctl(14,12),dr=ctl(14,13),rw=ctl(14,14),rr=ctl(14,15);
    for(long stage=1;stage<=2;++stage){assert(ctl(21,stage)==0);errno=0;assert(mremap(p,4*4096,8*4096,0)==MAP_FAILED && errno==ENOMEM);counts(1,1,2,1);}
    assert(mremap(p,4*4096,8*4096,0)==p);counts(1,1,6,1);
    assert(ctl(14,12)==dw && ctl(14,13)==dr && ctl(14,14)==rw && ctl(14,15)==rr);
    for(unsigned j=0;j<4096;++j)assert(((volatile unsigned char*)p)[j]==0x3c);
    check_random(p+4096);
    for(unsigned j=2*4096;j<8*4096;++j)assert(((volatile unsigned char*)p)[j]==0);
    counts(8,0,0,0);assert(munmap(p,8*4096)==0);drain();
}
static void hidden_cow(void){
    enum { Old=4*4096,New=4*1024*1024 };
    int go[2],done[2];assert(pipe(go)==0 && pipe(done)==0);
    unsigned char* p=(void*)(uintptr_t)ctl(0,Old);assert((intptr_t)p>0);
    for(unsigned j=0;j<Old;++j)p[j]=0x46;
    assert(mprotect(p,Old,PROT_NONE)==0);
    long child=syscall(SYS_fork);assert(child>=0);
    if(!child){receive(go[0]);assert(mprotect(p,Old,PROT_READ|PROT_WRITE)==0);for(unsigned j=0;j<Old;++j)assert(((volatile unsigned char*)p)[j]==0x46);assert(munmap(p,Old)==0);send_byte(done[1]);_exit(0);}
    assert(mremap(p,Old,New,0)==p);counts(4,0,New/4096-4,0);
    long probe=syscall(SYS_fork);assert(probe>=0);if(!probe){*(volatile unsigned char*)(p+New-4096)=1;_exit(99);}
    int status;assert(waitpid(probe,&status,0)==probe && WIFSIGNALED(status) && WTERMSIG(status)==SIGSEGV);counts(4,0,New/4096-4,0);
    assert(mprotect(p,New,PROT_READ|PROT_WRITE)==0);for(unsigned j=0;j<Old;++j)p[j]=0x99;
    assert(ctl(8,1)==0 && ((volatile unsigned char*)p)[2*1024*1024]==0 && ctl(7,0)==7);
    assert(ctl(1,(long)(uintptr_t)(p+2*1024*1024))==4096 && ctl(7,0)==8);
    send_byte(go[1]);receive(done[0]);for(unsigned j=0;j<Old;++j)assert(((volatile unsigned char*)p)[j]==0x99);
    assert(munmap(p,New)==0);reap(child);assert(close(go[0])==0 && close(go[1])==0 && close(done[0])==0 && close(done[1])==0);drain();
}
static void fragment(int fixed){
    int go[2],done[2];assert(pipe(go)==0 && pipe(done)==0);
    unsigned char* p=(void*)(uintptr_t)ctl(0,8*4096);assert((intptr_t)p>0);
    for(unsigned i=0;i<8;++i)for(unsigned j=0;j<4096;++j)p[i*4096+j]=pattern(i,j);
    random_page(p+3*4096);
    assert(ctl(1,(long)(uintptr_t)(p+2*4096))==4096 && ctl(1,(long)(uintptr_t)(p+4*4096))==4096);
    assert(ctl(4,6)==0 && ctl(1,(long)(uintptr_t)(p+3*4096))==-1 && errno==EIO);
    assert(mprotect(p+2*4096,2*4096,PROT_READ)==0);counts(6,2,0,1);
    long child=syscall(SYS_fork);assert(child>=0);
    if(!child){
        receive(go[0]);assert(mprotect(p,8*4096,PROT_READ|PROT_WRITE)==0);
        for(unsigned i=0;i<8;++i)if(i==3)check_random(p+i*4096);else for(unsigned j=0;j<4096;++j)assert(((volatile unsigned char*)p)[i*4096+j]==pattern(i,j));
        assert(munmap(p,8*4096)==0);send_byte(done[1]);_exit(0);
    }
    long dw=ctl(14,12),dr=ctl(14,13),rw=ctl(14,14),rr=ctl(14,15);
    if(!fixed){errno=0;assert(mremap(p+2*4096,2*4096,4*4096,0)==MAP_FAILED && errno==ENOMEM);counts(6,2,0,1);}
    unsigned char* next=fixed?mremap(p+2*4096,2*4096,4*4096,MREMAP_MAYMOVE|MREMAP_FIXED,p+4*4096):mremap(p+2*4096,2*4096,4*4096,MREMAP_MAYMOVE);
    assert(next!=MAP_FAILED && next!=(p+2*4096) && (!fixed || next==p+4*4096));counts(fixed?3:6,fixed?1:2,2,1);
    assert(ctl(14,12)==dw && ctl(14,13)==dr && ctl(14,14)==rw && ctl(14,15)==rr);
    for(unsigned j=0;j<4096;++j)assert(((volatile unsigned char*)next)[j]==pattern(2,j));
    check_random(next+4096);
    assert(mprotect(next,4*4096,PROT_READ|PROT_WRITE)==0);for(unsigned j=0;j<2*4096;++j)next[j]=0xa1;
    for(unsigned j=2*4096;j<4*4096;++j)assert(((volatile unsigned char*)next)[j]==0);
    for(unsigned i=0;i<8;++i)if(i<2 || (!fixed && i>=4))for(unsigned j=0;j<4096;++j)assert(((volatile unsigned char*)p)[i*4096+j]==pattern(i,j));
    if(!fixed){
        // Parent-first teardown must leave the child's original token and
        // independent Recovery copy alive until its later verification.
        assert(munmap(next,4*4096)==0 && munmap(p,8*4096)==0);
        send_byte(go[1]);receive(done[0]);
    }else{
        send_byte(go[1]);receive(done[0]);
        for(unsigned j=0;j<2*4096;++j)assert(((volatile unsigned char*)next)[j]==0xa1);
        assert(munmap(p,8*4096)==0);
    }
    reap(child);
    assert(close(go[0])==0 && close(go[1])==0 && close(done[0])==0 && close(done[1])==0);drain();
}
int main(void){counts(0,0,0,0);failures();hidden_cow();fragment(0);fragment(1);puts("PAGEABLE_GROWTH_PASS preparation_rollback in_place hidden_COW permissions zero_tail reserve_fault auto_move fragment_offset same_backing_destination Stored_Recovery_transfer no_backing_IO fork_isolation cleanup");}
