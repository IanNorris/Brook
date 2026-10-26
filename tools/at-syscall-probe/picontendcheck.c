// Native Linux reference and disposable Brook guest; no game dependencies.
#define _GNU_SOURCE
#include <sys/syscall.h>
#include <linux/futex.h>
#include <sys/mman.h>
#include <unistd.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>
#include <stdatomic.h>
#include <sys/wait.h>
#include <signal.h>

#define CK(x) do { if (!(x)) { fprintf(stderr,"PICONTEND_FAIL line=%d %s errno=%d\n",__LINE__,#x,errno); _exit(1); } } while (0)
static long fx(uint32_t* word,int op,const struct timespec* deadline) {
    errno=0;return syscall(SYS_futex,word,op,0,deadline,NULL,0);
}
static uint32_t tid(void) {return (uint32_t)syscall(SYS_gettid);}
static struct timespec after(int clock,unsigned ms) {
    struct timespec t;CK(clock_gettime(clock,&t)==0);
    t.tv_nsec+=(long)ms*1000000;t.tv_sec+=t.tv_nsec/1000000000;t.tv_nsec%=1000000000;return t;
}
static void wait_for_waiter(uint32_t* word) {
    for(unsigned i=0;i<10000;++i) {
        if(__atomic_load_n(word,__ATOMIC_ACQUIRE)&FUTEX_WAITERS)return;
        struct timespec pause={0,1000000};nanosleep(&pause,NULL);
    }
    CK(!"waiter did not enter kernel");
}
struct Worker {uint32_t* word;unsigned rounds;int flags;};
static atomic_uint inside,completed;
static void* worker(void* raw) {
    struct Worker* arg=raw;
    for(unsigned i=0;i<arg->rounds;++i) {
        CK(fx(arg->word,FUTEX_LOCK_PI|arg->flags,NULL)==0);
        CK((__atomic_load_n(arg->word,__ATOMIC_ACQUIRE)&FUTEX_TID_MASK)==tid());
        CK(atomic_fetch_add(&inside,1)==0);
        atomic_fetch_add(&completed,1);
        CK(atomic_fetch_sub(&inside,1)==1);
        CK(fx(arg->word,FUTEX_UNLOCK_PI|arg->flags,NULL)==0);
    }
    return NULL;
}
static void stress(int flags,int alias) {
    int fd=-1;uint32_t local=0;uint32_t* a=&local;uint32_t* b=a;
    if(alias) {
        fd=syscall(SYS_memfd_create,"pi-contend",0);CK(fd>=0 && ftruncate(fd,4096)==0);
        a=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
        b=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
        CK(a!=MAP_FAILED && b!=MAP_FAILED && a!=b);
    }
    __atomic_store_n(a,tid(),__ATOMIC_RELEASE);
    enum {N=8,ROUNDS=200};pthread_t threads[N];struct Worker args[N];
    atomic_store(&completed,0);
    for(unsigned i=0;i<N;++i) {
        args[i]=(struct Worker){i%2?a:b,ROUNDS,flags};
        CK(pthread_create(&threads[i],NULL,worker,&args[i])==0);
    }
    wait_for_waiter(a);
    errno=0;CK(syscall(SYS_futex,a,FUTEX_WAKE|flags,1,NULL,NULL,0)==-1 && errno==EINVAL);
    CK(fx(a,FUTEX_UNLOCK_PI|flags,NULL)==0);
    for(unsigned i=0;i<N;++i)CK(pthread_join(threads[i],NULL)==0);
    CK(atomic_load(&completed)==N*ROUNDS && atomic_load(&inside)==0 && *a==0);
    if(alias) {CK(munmap(a,4096)==0 && munmap(b,4096)==0 && close(fd)==0);}
    printf("PI_STRESS_PASS flags=%d alias=%d acquisitions=%d\n",flags,alias,N*ROUNDS);fflush(stdout);
}
struct Timeout {uint32_t* word;int op,clock;};
static void* timeout_worker(void* raw) {
    struct Timeout* arg=raw;struct timespec end=after(arg->clock,100);
    CK(fx(arg->word,arg->op,&end)==-1 && errno==ETIMEDOUT);return NULL;
}
static void timeouts(void) {
    const struct {int op,clock;} cases[]={
        {FUTEX_LOCK_PI|FUTEX_PRIVATE_FLAG,CLOCK_REALTIME},
        {FUTEX_LOCK_PI2|FUTEX_PRIVATE_FLAG,CLOCK_MONOTONIC},
        {FUTEX_LOCK_PI2|FUTEX_PRIVATE_FLAG|FUTEX_CLOCK_REALTIME,CLOCK_REALTIME}};
    for(unsigned i=0;i<sizeof(cases)/sizeof(*cases);++i) {
        uint32_t word=tid();pthread_t thread;
        struct Timeout arg={&word,cases[i].op,cases[i].clock};
        CK(pthread_create(&thread,NULL,timeout_worker,&arg)==0);
        CK(pthread_join(thread,NULL)==0);
        CK((word&FUTEX_TID_MASK)==tid());
        CK(fx(&word,FUTEX_UNLOCK_PI|FUTEX_PRIVATE_FLAG,NULL)==0 && word==0);
    }
    puts("PI_TIMEOUT_PASS");fflush(stdout);
}
static pthread_mutex_t robust,robust_second;
static atomic_uint owner_ready,owner_exit;
static void* robust_owner(void* unused) { (void)unused;CK(pthread_mutex_lock(&robust)==0);return NULL; }
static void* contended_robust_owner(void* unused) {
    (void)unused;CK(pthread_mutex_lock(&robust_second)==0);
    CK(pthread_mutex_lock(&robust)==0);atomic_store(&owner_ready,1);
    while(!atomic_load(&owner_exit)) {struct timespec pause={0,1000000};nanosleep(&pause,NULL);}
    return NULL;
}
static void* robust_successor(void* unused) {
    (void)unused;CK(pthread_mutex_lock(&robust)==EOWNERDEAD);
    CK(pthread_mutex_lock(&robust_second)==EOWNERDEAD);
    CK(pthread_mutex_consistent(&robust_second)==0 && pthread_mutex_unlock(&robust_second)==0);
    CK(pthread_mutex_consistent(&robust)==0 && pthread_mutex_unlock(&robust)==0);return NULL;
}
static void* robust_late_contender(void* unused) {
    (void)unused;CK(pthread_mutex_lock(&robust)==0);
    CK((((uint32_t*)&robust)[0]&FUTEX_TID_MASK)==tid());
    CK(pthread_mutex_unlock(&robust)==0);return NULL;
}
static void pthread_controls(void) {
    pthread_mutexattr_t attr;pthread_mutex_t recursive;
    CK(pthread_mutexattr_init(&attr)==0);
    CK(pthread_mutexattr_setprotocol(&attr,PTHREAD_PRIO_INHERIT)==0);
    CK(pthread_mutexattr_settype(&attr,PTHREAD_MUTEX_RECURSIVE)==0);
    CK(pthread_mutex_init(&recursive,&attr)==0);
    CK(pthread_mutex_lock(&recursive)==0 && pthread_mutex_lock(&recursive)==0);
    CK(pthread_mutex_unlock(&recursive)==0 && pthread_mutex_unlock(&recursive)==0);
    CK(pthread_mutex_destroy(&recursive)==0);
    CK(pthread_mutexattr_settype(&attr,PTHREAD_MUTEX_NORMAL)==0);
    CK(pthread_mutexattr_setrobust(&attr,PTHREAD_MUTEX_ROBUST)==0);
    CK(pthread_mutex_init(&robust,&attr)==0);
    pthread_t thread;CK(pthread_create(&thread,NULL,robust_owner,NULL)==0);
    CK(pthread_join(thread,NULL)==0);
    CK(pthread_mutex_lock(&robust)==EOWNERDEAD);
    CK(pthread_mutex_consistent(&robust)==0 && pthread_mutex_unlock(&robust)==0);
    CK(pthread_mutex_destroy(&robust)==0);
    for(unsigned repeat=0;repeat<16;++repeat) {
        atomic_store(&owner_ready,0);atomic_store(&owner_exit,0);
        CK(pthread_mutex_init(&robust,&attr)==0 && pthread_mutex_init(&robust_second,&attr)==0);
        CK(pthread_create(&thread,NULL,contended_robust_owner,NULL)==0);
        while(!atomic_load(&owner_ready)) {struct timespec pause={0,1000000};nanosleep(&pause,NULL);}
        pthread_t successor;CK(pthread_create(&successor,NULL,robust_successor,NULL)==0);
        // This probe deliberately targets glibc's x86-64 pthread mutex layout.
        wait_for_waiter((uint32_t*)&robust);
        atomic_store(&owner_exit,1);
        pthread_t late;CK(pthread_create(&late,NULL,robust_late_contender,NULL)==0);
        CK(pthread_join(thread,NULL)==0 && pthread_join(successor,NULL)==0 && pthread_join(late,NULL)==0);
        CK(pthread_mutex_destroy(&robust)==0 && pthread_mutex_destroy(&robust_second)==0);
    }
    CK(pthread_mutexattr_destroy(&attr)==0);
    puts("PI_PTHREAD_RECURSIVE_AND_ROBUST_PASS");fflush(stdout);
}
static void killed_raw_owner(void) {
    int fd=syscall(SYS_memfd_create,"pi-killed-owner",0);CK(fd>=0 && ftruncate(fd,4096)==0);
    uint32_t* word=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);CK(word!=MAP_FAILED);
    int ready[2],hold[2];CK(pipe(ready)==0 && pipe(hold)==0);
    pid_t child=fork();CK(child>=0);
    if(!child) {
        CK(syscall(SYS_set_robust_list,NULL,24)==0); // Deliberately no robust list.
        close(ready[0]);close(hold[1]);
        CK(fx(word,FUTEX_LOCK_PI,NULL)==0);
        CK(write(ready[1],"R",1)==1);char byte;
        (void)read(hold[0],&byte,1);_exit(99);
    }
    close(ready[1]);close(hold[0]);char byte;CK(read(ready[0],&byte,1)==1);
    struct Worker arg={word,1,0};pthread_t successor;atomic_store(&completed,0);
    CK(pthread_create(&successor,NULL,worker,&arg)==0);
    wait_for_waiter(word);CK(kill(child,SIGKILL)==0);
    int status;CK(waitpid(child,&status,0)==child && WIFSIGNALED(status) && WTERMSIG(status)==SIGKILL);
    CK(pthread_join(successor,NULL)==0 && atomic_load(&completed)==1 && *word==0);
    CK(close(ready[0])==0 && close(hold[1])==0 && munmap(word,4096)==0 && close(fd)==0);
    puts("PI_KILLED_RAW_OWNER_PASS");fflush(stdout);
}
static volatile sig_atomic_t handled;
static atomic_uint signal_acquired;
static pthread_mutex_t signal_mutex;
static void handler(int signal) {(void)signal;++handled;}
static void* signal_waiter(void* unused) {
    (void)unused;CK(pthread_mutex_lock(&signal_mutex)==0);
    CK((((uint32_t*)&signal_mutex)[0]&FUTEX_TID_MASK)==tid());
    atomic_store(&signal_acquired,1);CK(pthread_mutex_unlock(&signal_mutex)==0);return NULL;
}
static void signal_restart(void) {
    pthread_mutexattr_t attr;CK(pthread_mutexattr_init(&attr)==0);
    CK(pthread_mutexattr_setprotocol(&attr,PTHREAD_PRIO_INHERIT)==0);
    CK(pthread_mutex_init(&signal_mutex,&attr)==0);
    struct sigaction action={0};action.sa_handler=handler;sigemptyset(&action.sa_mask);
    // No SA_RESTART: PI lock interruption must still restart unconditionally.
    CK(sigaction(SIGUSR1,&action,NULL)==0);
    CK(pthread_mutex_lock(&signal_mutex)==0);
    pthread_t waiter;CK(pthread_create(&waiter,NULL,signal_waiter,NULL)==0);
    wait_for_waiter((uint32_t*)&signal_mutex);
    CK(pthread_kill(waiter,SIGUSR1)==0);
    for(unsigned i=0;i<10000 && !handled;++i) {struct timespec pause={0,1000000};nanosleep(&pause,NULL);}
    CK(handled>0);
    struct timespec pause={0,20000000};nanosleep(&pause,NULL);
    CK(!atomic_load(&signal_acquired));
    CK(pthread_mutex_unlock(&signal_mutex)==0);
    CK(pthread_join(waiter,NULL)==0 && atomic_load(&signal_acquired));
    CK(pthread_mutex_destroy(&signal_mutex)==0 && pthread_mutexattr_destroy(&attr)==0);
    puts("PI_SIGNAL_RESTART_PASS");fflush(stdout);
}
int main(void) {
    alarm(60);
    pthread_controls();
    timeouts();
    stress(FUTEX_PRIVATE_FLAG,0);
    stress(0,1);
    killed_raw_owner();
    signal_restart();
    puts("PICONTEND_PASS");
    return 0;
}
