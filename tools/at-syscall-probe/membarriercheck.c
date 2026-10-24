#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

enum { QUERY=0, PRIVATE=8, REGISTER=16, ROUNDS=10000 };
static int checks;
static void check(int ok,const char* name) {
    ++checks;
    if(!ok) { fprintf(stderr,"FAIL %s errno=%d\n",name,errno);exit(1); }
}
static long mb(int cmd,unsigned flags) { return syscall(324,cmd,flags,0); }
static int denied(void) { errno=0;return mb(PRIVATE,0)==-1 && errno==EPERM; }
static pthread_barrier_t begin,end;
static atomic_int x,y,r0,r1,failed;
static int cpu0=-1,cpu1=-1;
static void pin(int cpu) {
    if(cpu<0)return;
    cpu_set_t set;CPU_ZERO(&set);CPU_SET(cpu,&set);
    if(pthread_setaffinity_np(pthread_self(),sizeof(set),&set))atomic_store(&failed,1);
}
static void* worker(void* arg) {
    int side=(int)(long)arg;pin(side ? cpu1:cpu0);
    for(int i=0;i<ROUNDS;++i) {
        pthread_barrier_wait(&begin);
        if(side) {
            // Fast side deliberately has no hardware barrier. The syscall on
            // the other CPU must provide the matching remote barrier.
            atomic_store_explicit(&y,1,memory_order_relaxed);
            __asm__ volatile("" ::: "memory");
            atomic_store_explicit(&r1,atomic_load_explicit(&x,memory_order_relaxed),memory_order_relaxed);
        } else {
            atomic_store_explicit(&x,1,memory_order_relaxed);
            if(mb(PRIVATE,0))atomic_store(&failed,1);
            atomic_store_explicit(&r0,atomic_load_explicit(&y,memory_order_relaxed),memory_order_relaxed);
        }
        pthread_barrier_wait(&end);
    }
    return NULL;
}
static void* concurrent(void* arg) {
    (void)arg;
    for(int i=0;i<500;++i) {
        // Alternate affinity to force issuers onto both CPUs between calls.
        if(!(i%8))pin((i/8)&1 ? cpu1:cpu0);
        if(mb(PRIVATE,0))atomic_store(&failed,1);
    }
    return NULL;
}
static void* mapping_churn(void* arg) {
    (void)arg;
    for(int i=0;i<500;++i) {
        void* p=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        if(p==MAP_FAILED){atomic_store(&failed,1);break;}
        *(volatile char*)p=1;
        if(mprotect(p,4096,PROT_READ) || munmap(p,4096))atomic_store(&failed,1);
    }
    return NULL;
}
int main(int argc,char** argv) {
    if(argc==2 && !strcmp(argv[1],"--after-exec"))return denied()?0:1;
    long supported=mb(QUERY,0);
    check(supported>=0 && (supported&(PRIVATE|REGISTER))==(PRIVATE|REGISTER),"query");
    check(denied(),"unregistered denied");
    errno=0;check(mb(QUERY,1)==-1 && errno==EINVAL,"invalid flags");
    errno=0;check(mb(PRIVATE|REGISTER,0)==-1 && errno==EINVAL,"combined command rejected");
    check(mb(REGISTER,0)==0 && mb(REGISTER,0)==0,"idempotent registration");
    check(mb(PRIVATE,0)==0,"registered barrier");
    pid_t child=fork();check(child>=0,"fork");
    if(!child) {
        if(mb(PRIVATE,0))_exit(1);
        execl(argv[0],argv[0],"--after-exec",(char*)0);_exit(2);
    }
    int status=0;check(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status),"fork inherits and exec clears registration");
    cpu_set_t allowed;CPU_ZERO(&allowed);
    check(sched_getaffinity(0,sizeof(allowed),&allowed)==0,"available CPUs");
    for(int i=0;i<CPU_SETSIZE;++i)if(CPU_ISSET(i,&allowed)) {
        if(cpu0<0)cpu0=i;else{cpu1=i;break;}
    }
    check(cpu1>=0,"two distinct CPUs required");
    check(!pthread_barrier_init(&begin,NULL,3) && !pthread_barrier_init(&end,NULL,3),"litmus barriers");
    pthread_t a,b;
    check(!pthread_create(&a,NULL,worker,(void*)0) && !pthread_create(&b,NULL,worker,(void*)1),"litmus workers");
    unsigned forbidden=0;
    for(int i=0;i<ROUNDS;++i) {
        atomic_store_explicit(&x,0,memory_order_relaxed);atomic_store_explicit(&y,0,memory_order_relaxed);
        pthread_barrier_wait(&begin);pthread_barrier_wait(&end);
        if(!atomic_load(&r0) && !atomic_load(&r1))++forbidden;
    }
    pthread_join(a,NULL);pthread_join(b,NULL);
    check(!forbidden && !atomic_load(&failed),"remote StoreLoad ordering");
    pthread_t c,d,e;
    check(!pthread_create(&c,NULL,concurrent,NULL) && !pthread_create(&d,NULL,concurrent,NULL) && !pthread_create(&e,NULL,mapping_churn,NULL),"concurrent rendezvous and mapping workers");
    pthread_join(c,NULL);pthread_join(d,NULL);pthread_join(e,NULL);
    check(!atomic_load(&failed),"concurrent rendezvous and mapping completion");
    printf("{\"membarrier_pass\":true,\"checks\":%d,\"rounds\":%d,\"forbidden\":%u,\"cpus\":[%d,%d]}\n",checks,ROUNDS,forbidden,cpu0,cpu1);
    return 0;
}
