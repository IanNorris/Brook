#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
static long ctl(long op,long value){return syscall(0xB40,op,value,0L,0L,0L,0L);}
static unsigned char pattern(unsigned i,unsigned j){return (unsigned char)(i*31+j*73+(j>>8));}
static void counts(long held,long stored,long zero,long recovery){
    long h=ctl(13,0),s=ctl(13,2),z=ctl(13,1),r=ctl(13,3);
    if(h!=held || s!=stored || z!=zero || r!=recovery){fprintf(stderr,"fork counts actual=%ld/%ld/%ld/%ld expected=%ld/%ld/%ld/%ld\n",h,s,z,r,held,stored,zero,recovery);assert(0);}
}
static unsigned char* seed(void){
    long base=ctl(11,16*4096);assert(base>0);unsigned char* p=(void*)(uintptr_t)base;
    for(unsigned i=0;i<12;++i)for(unsigned j=0;j<4096;++j)p[i*4096+j]=pattern(i,j);
    uint32_t random=0x45b1a9u;
    for(unsigned j=0;j<4096;++j){random^=random<<13;random^=random>>17;random^=random<<5;p[4096+j]=(unsigned char)random;}
    for(unsigned i=0;i<12;i+=2)assert(ctl(1,base+i*4096)==4096);
    assert(ctl(4,6)==0);errno=0;assert(ctl(1,base+4096)==-1 && errno==EIO);
    assert(mprotect(p,4*4096,PROT_NONE)==0 && mprotect(p+4*4096,4096,PROT_READ)==0);
    assert(munmap(p+9*4096,4096)==0);counts(5,6,4,1);return p;
}
static void permit(unsigned char* p){assert(mprotect(p,9*4096,PROT_READ|PROT_WRITE)==0 && mprotect(p+10*4096,6*4096,PROT_READ|PROT_WRITE)==0);}
static void original(volatile unsigned char* p){
    for(unsigned i=0;i<16;++i)if(i!=9){
        uint32_t random=0x45b1a9u;
        for(unsigned j=0;j<4096;++j){
            unsigned char expected=i>=12?0:pattern(i,j);
            if(i==1){random^=random<<13;random^=random>>17;random^=random<<5;expected=(unsigned char)random;}
            assert(p[i*4096+j]==expected);
        }
    }
}
static void bytes(volatile unsigned char* p,unsigned char tag,int write){
    for(unsigned i=0;i<16;++i)if(i!=9)for(unsigned j=0;j<4096;++j){unsigned char value=(unsigned char)(tag+i+j*3);if(write)p[i*4096+j]=value;else assert(p[i*4096+j]==value);}
}
static void churn(unsigned char* p,unsigned char tag){
    for(unsigned round=0;round<5;++round)for(unsigned i=0;i<12;i+=2){
        assert(ctl(1,(long)(uintptr_t)(p+i*4096))==4096);
        for(unsigned j=0;j<4096;++j)assert(((volatile unsigned char*)p)[i*4096+j]==(unsigned char)(tag+i+j*3));
    }
}
static void send_byte(int fd){char c='x';assert(write(fd,&c,1)==1);}
static void receive(int fd){char c=0;assert(read(fd,&c,1)==1 && c=='x');}
static void reap(pid_t pid){int status;assert(waitpid(pid,&status,0)==pid && WIFEXITED(status) && WEXITSTATUS(status)==0);}
static void drain(void){
    struct timespec delay={0,10000000};
    for(unsigned n=0;n<500 && ctl(2,0);++n)assert(nanosleep(&delay,0)==0);
    assert(ctl(2,0)==0 && ctl(14,1)==0 && ctl(14,2)==0);counts(0,0,0,0);
}
static void failures(void){
    unsigned char* p=seed();long total=ctl(2,0);assert(total==6);
    for(long stage=1;stage<=5;++stage){
        assert(ctl(20,stage)==0);errno=0;long pid=syscall(SYS_fork);
        if(pid==0)_exit(99);
        assert(pid==-1 && errno==ENOMEM);counts(5,6,4,1);assert(ctl(2,0)==total);
    }
    permit(p);original(p);p[3*4096]=pattern(3,0);
    assert(munmap(p,16*4096)==0);drain();
}
static void concurrent(void){
    int to_child[2],to_parent[2];assert(pipe(to_child)==0 && pipe(to_parent)==0);
    unsigned char* p=seed();long dw=ctl(14,12),dr=ctl(14,13),rw=ctl(14,14),rr=ctl(14,15);
    long pid=syscall(SYS_fork);assert(pid>=0);
    counts(5,6,4,1);assert(ctl(14,12)==dw && ctl(14,13)==dr && ctl(14,14)==rw && ctl(14,15)==rr);
    if(!pid){
        send_byte(to_parent[1]);receive(to_child[0]);permit(p);original(p);bytes(p,0xb1,1);churn(p,0xb1);
        send_byte(to_parent[1]);receive(to_child[0]);bytes(p,0xb1,0);
        long probe=syscall(SYS_fork);assert(probe>=0);
        if(!probe){*(volatile unsigned char*)(p+9*4096)=1;_exit(99);}
        int status;assert(waitpid(probe,&status,0)==probe && WIFSIGNALED(status) && WTERMSIG(status)==SIGSEGV);
        assert(munmap(p,16*4096)==0);send_byte(to_parent[1]);_exit(0);
    }
    receive(to_parent[0]);send_byte(to_child[1]);permit(p);original(p);bytes(p,0xa1,1);churn(p,0xa1);
    receive(to_parent[0]);bytes(p,0xa1,0);send_byte(to_child[1]);receive(to_parent[0]);bytes(p,0xa1,0);
    assert(munmap(p,16*4096)==0);reap(pid);
    assert(close(to_child[0])==0 && close(to_child[1])==0 && close(to_parent[0])==0 && close(to_parent[1])==0);drain();
}
static void lifetime(int parent_first,const char* self){
    int go[2],done[2];assert(pipe(go)==0 && pipe(done)==0);
    unsigned char* p=seed();long pid=syscall(SYS_fork);assert(pid>=0);
    if(!pid){
        counts(5,6,4,1);receive(go[0]);
        if(parent_first){permit(p);original(p);}
        assert(munmap(p,16*4096)==0);send_byte(done[1]);
        if(!parent_first){char* args[]={(char*)self,"--exec-child",0};execv(self,args);_exit(98);}
        _exit(0);
    }
    if(parent_first)assert(munmap(p,16*4096)==0);
    send_byte(go[1]);receive(done[0]);
    if(!parent_first){permit(p);original(p);p[3*4096]=pattern(3,0);assert(munmap(p,16*4096)==0);}
    reap(pid);assert(close(go[0])==0 && close(go[1])==0 && close(done[0])==0 && close(done[1])==0);drain();
}
static void* thread(void* value){return value;}
static void sparse(void){
    enum { Bytes=16*1024*1024 };
    unsigned char* p=(void*)(uintptr_t)ctl(11,Bytes);assert((intptr_t)p>0);
    p[0]=0x5a;long child=syscall(SYS_fork);assert(child>=0);
    if(!child){
        // The parent has no leaf in these distant 2 MiB page-table spans.
        for(unsigned at=2*1024*1024;at<Bytes;at+=2*1024*1024){
            assert(ctl(8,1)==0 && ((volatile unsigned char*)p)[at]==0);p[at]=0xa5;
            assert(ctl(1,(long)(uintptr_t)(p+at))==4096);
        }
        assert(munmap(p,Bytes)==0);_exit(0);
    }
    assert(munmap(p,Bytes)==0);reap(child);drain();
}
int main(int argc,char** argv){
    if(argc==2 && !strcmp(argv[1],"--exec-child")){
        pthread_t t;void* value=0;assert(pthread_create(&t,0,thread,(void*)123)==0 && pthread_join(t,&value)==0 && value==(void*)123);
        puts("PAGEABLE_FORK_EXEC_RESET_PASS");return 0;
    }
    assert(argc==1);counts(0,0,0,0);failures();concurrent();lifetime(1,argv[0]);lifetime(0,argv[0]);sparse();
    pthread_t t;assert(pthread_create(&t,0,thread,0)!=0);drain();
    puts("PAGEABLE_FORK_PASS independent_entries stored_tokens Recovery_copy resident_COW concurrent_churn fragments hole_SIGSEGV parent_child_exit failed_prepare_COW_cleanup exec_reset sparse_reserve_refault CLONE_VM_exclusion");
}
