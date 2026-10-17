#define _GNU_SOURCE
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

static int failures, checks;
static volatile sig_atomic_t caught;
#define CHECK(c) do { ++checks; if (!(c)) { ++failures; printf("FAIL %d: %s errno=%d\n",__LINE__,#c,errno); } } while(0)
static void handler(int s) { (void)s; ++caught; }
static uint64_t pending(void) {
    uint64_t bits=0;
    CHECK(syscall(SYS_rt_sigpending,&bits,8)==0);
    return bits;
}
static int consume(int signo) {
    uint64_t bit=1ULL<<(signo-1);
    struct timespec timeout={0};
    return syscall(SYS_rt_sigtimedwait,&bit,NULL,&timeout,8);
}
static pthread_barrier_t barrier;
static void *thread_pending(void *unused) {
    (void)unused;
    pthread_barrier_wait(&barrier);
    pthread_barrier_wait(&barrier);
    uint64_t bits=0;
    int ok=syscall(SYS_rt_sigpending,&bits,8)==0 && (bits&(1ULL<<(SIGUSR1-1)));
    ok=ok && consume(SIGUSR1)==SIGUSR1;
    return (void*)(uintptr_t)!ok;
}
int main(void) {
    struct sigaction action={0}; action.sa_handler=handler;
    sigemptyset(&action.sa_mask);
    CHECK(sigaction(SIGUSR1,&action,NULL)==0);
    sigset_t mask,old; sigemptyset(&mask);
    sigaddset(&mask,SIGUSR1);sigaddset(&mask,64);
    CHECK(pthread_sigmask(SIG_BLOCK,&mask,&old)==0);
    const uint64_t bit=1ULL<<(SIGUSR1-1);
    CHECK(!(pending()&bit));
    CHECK(syscall(SYS_tgkill,getpid(),syscall(SYS_gettid),SIGUSR1)==0);
    CHECK((pending()&bit) && (pending()&bit) && caught==0);
    sigset_t libc_pending;
    CHECK(sigpending(&libc_pending)==0 && sigismember(&libc_pending,SIGUSR1)==1);
    CHECK(syscall(SYS_rt_sigpending,NULL,0)==0);
    CHECK(syscall(SYS_rt_sigpending,(void*)1,0)==0);
    errno=0;CHECK(syscall(SYS_rt_sigpending,NULL,8)==-1 && errno==EFAULT);
    errno=0;CHECK(syscall(SYS_rt_sigpending,NULL,9)==-1 && errno==EINVAL);
    for(size_t n=1;n<=8;n++) {
        unsigned char bytes[10];memset(bytes,0xa5,sizeof(bytes));
        uint64_t expected=pending();
        CHECK(syscall(SYS_rt_sigpending,bytes+1,n)==0);
        CHECK(!memcmp(bytes+1,&expected,n));
        CHECK(bytes[0]==0xa5 && bytes[n+1]==0xa5);
    }
    size_t page=(size_t)sysconf(_SC_PAGESIZE);
    char *mapping=mmap(NULL,2*page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    CHECK(mapping!=MAP_FAILED);
    if(mapping!=MAP_FAILED) {
        CHECK(munmap(mapping+page,page)==0);
        CHECK(syscall(SYS_rt_sigpending,mapping+page-4,4)==0);
        errno=0;CHECK(syscall(SYS_rt_sigpending,mapping+page-4,8)==-1 && errno==EFAULT);
        CHECK(mprotect(mapping,page,PROT_READ)==0);
        errno=0;CHECK(syscall(SYS_rt_sigpending,mapping,8)==-1 && errno==EFAULT);
        CHECK(pending()&bit);
        CHECK(munmap(mapping,page)==0);
    }
    pid_t child=fork();CHECK(child>=0);
    if(!child) { uint64_t bits=0;_exit(syscall(SYS_rt_sigpending,&bits,8)==0 && !(bits&bit)?0:1); }
    if(child>0) {int status=0;CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);}
    CHECK(pthread_sigmask(SIG_SETMASK,&old,NULL)==0);
    CHECK(caught==1 && !(pending()&bit));
    CHECK(pthread_sigmask(SIG_BLOCK,&mask,NULL)==0);
    CHECK(syscall(SYS_tgkill,getpid(),syscall(SYS_gettid),64)==0);
    CHECK(pending()&(1ULL<<63));CHECK(consume(64)==64);
    CHECK(!(pending()&(1ULL<<63)));

    struct sigevent ev={0};ev.sigev_notify=SIGEV_SIGNAL;ev.sigev_signo=SIGUSR1;
    int timer=-1;
    CHECK(syscall(SYS_timer_create,CLOCK_MONOTONIC,&ev,&timer)==0);
    if(timer>=0) {
        struct itimerspec arm={.it_value={0,20000000}};
        CHECK(syscall(SYS_timer_settime,timer,0,&arm,NULL)==0);
        int seen=0;
        for(int i=0;i<200;i++) {
            if(pending()&bit){seen=1;break;}
            struct timespec pause={0,10000000};nanosleep(&pause,NULL);
        }
        CHECK(seen && (pending()&bit));CHECK(consume(SIGUSR1)==SIGUSR1);
        CHECK(!(pending()&bit));CHECK(syscall(SYS_timer_delete,timer)==0);
    }
    CHECK(pthread_barrier_init(&barrier,NULL,2)==0);
    pthread_t worker;
    int created=pthread_create(&worker,NULL,thread_pending,NULL);
    CHECK(created==0);
    if(!created) {
        pthread_barrier_wait(&barrier);
        CHECK(pthread_kill(worker,SIGUSR1)==0);
        CHECK(!(pending()&bit));
        pthread_barrier_wait(&barrier);
        void *result=NULL;CHECK(pthread_join(worker,&result)==0 && result==NULL);
    }
    CHECK(pthread_barrier_destroy(&barrier)==0);
    CHECK(pthread_sigmask(SIG_SETMASK,&old,NULL)==0);
    printf("SIGPENDINGCHECK %s checks=%d failures=%d\n",failures?"FAIL":"PASS",checks,failures);
    return failures?1:0;
}
