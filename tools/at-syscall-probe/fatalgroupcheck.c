#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* A fatal signal must end a process, even when delivered to one thread. The
 * controls distinguish this from raw SYS_exit, which must preserve peers. */
enum { WORKERS=4 };
enum { TERM_LEADER, TERM_THREAD, KILL_THREAD, ALARM_GROUP,
       MASKED_TERM, HANDLED_TERM, IGNORED_TERM, LEADER_EXIT, CLONE_RACE, USR1_GROUP, MASKED_USR2, REALTIME_GROUP, CASES };
static const char *names[CASES]={"term-leader","term-thread","kill-thread",
    "alarm-group","masked-term","handled-term","ignored-term","leader-exit","clone-race","usr1-group","masked-usr2","realtime-group"};
static unsigned ready, word, births;
static pid_t tids[WORKERS];
static int data_pipe[2], control_pipe[2], report_pipe[2], mode;
static volatile sig_atomic_t handled;
static int case_signal(int which) {
    if(which==KILL_THREAD)return SIGKILL;
    if(which==ALARM_GROUP)return SIGALRM;
    if(which==USR1_GROUP)return SIGUSR1;
    if(which==MASKED_USR2)return SIGUSR2;
    if(which==REALTIME_GROUP)return 34;
    return SIGTERM;
}
static uint64_t millis(void) {
    struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;
}
static void pause_ms(unsigned n) {
    struct timespec t={n/1000,(n%1000)*1000000};
    while(nanosleep(&t,&t)<0&&errno==EINTR){}
}
static void handler(int signal) {(void)signal;handled=1;}
static void *short_thread(void *arg) {(void)arg;sched_yield();return NULL;}
static void *worker(void *opaque) {
    uintptr_t id=(uintptr_t)opaque;
    tids[id]=(pid_t)syscall(SYS_gettid);
    __atomic_add_fetch(&ready,1,__ATOMIC_RELEASE);
    if(mode==LEADER_EXIT && id==0) {
        char byte;
        if(read(control_pipe[0],&byte,1)!=1)_exit(111);
        syscall(SYS_exit_group,0);_exit(112);
    }
    if(id==0)for(;;)syscall(SYS_futex,&word,128,0,NULL,NULL,0);
    if(id==1) {char byte;for(;;)read(data_pipe[0],&byte,1);}
    if(id==2) {
        int fd=epoll_create1(0);struct epoll_event event;
        if(fd<0)_exit(113);
        for(;;)epoll_wait(fd,&event,1,-1);
    }
    if(mode==CLONE_RACE)for(;;) {
        pthread_t thread;
        if(!pthread_create(&thread,NULL,short_thread,NULL)) {
            __atomic_add_fetch(&births,1,__ATOMIC_RELEASE);
            pthread_join(thread,NULL);
        }
        else sched_yield();
    }
    for(;;)pause_ms(10);
    return NULL;
}
static void child_run(int which) {
    mode=which;ready=word=births=0;handled=0;
    close(report_pipe[0]);close(control_pipe[1]);
    if((which==MASKED_TERM||which==MASKED_USR2)) {
        sigset_t mask;sigemptyset(&mask);sigaddset(&mask,case_signal(which));
        if(pthread_sigmask(SIG_BLOCK,&mask,NULL))_exit(114);
    }
    if(which==HANDLED_TERM||which==IGNORED_TERM) {
        struct sigaction action={0};sigemptyset(&action.sa_mask);
        action.sa_handler=which==HANDLED_TERM?handler:SIG_IGN;
        if(sigaction(SIGTERM,&action,NULL))_exit(115);
    }
    pthread_t threads[WORKERS];
    for(uintptr_t i=0;i<WORKERS;++i)
        if(pthread_create(&threads[i],NULL,worker,(void*)i))_exit(116);
    while(__atomic_load_n(&ready,__ATOMIC_ACQUIRE)!=WORKERS)sched_yield();
    if(which==CLONE_RACE)
        while(!__atomic_load_n(&births,__ATOMIC_ACQUIRE))sched_yield();
    if(write(report_pipe[1],tids,sizeof(tids))!=sizeof(tids))_exit(117);
    if(which==LEADER_EXIT) {syscall(SYS_exit,23);_exit(118);}
    if(which==ALARM_GROUP && syscall(SYS_alarm,1)<0)_exit(123);
    if(which==MASKED_TERM||which==MASKED_USR2||which==IGNORED_TERM) {
        char byte;if(read(control_pipe[0],&byte,1)!=1)_exit(119);
        if((which==MASKED_TERM||which==MASKED_USR2)) {
            sigset_t mask;sigemptyset(&mask);sigaddset(&mask,case_signal(which));
            if(pthread_sigmask(SIG_UNBLOCK,&mask,NULL))_exit(120);
        } else {syscall(SYS_exit_group,0);_exit(121);}
    }
    for(;;) {
        if(which==HANDLED_TERM&&handled) {syscall(SYS_exit_group,0);_exit(122);}
        pause_ms(10);
    }
}
static int wait_bounded(pid_t child,int *status,unsigned timeout) {
    uint64_t deadline=millis()+timeout;
    do {
        pid_t got=waitpid(child,status,WNOHANG);
        if(got==child)return 1;
        if(got<0&&errno!=EINTR)return -1;
        pause_ms(2);
    }while(millis()<deadline);
    return 0;
}
static int run_case(int which) {
    if(pipe(data_pipe)||pipe(control_pipe)||pipe(report_pipe))return 0;
    pid_t child=fork();if(!child)child_run(which);
    if(child<0)return 0;
    close(report_pipe[1]);close(control_pipe[0]);
    int status=0,passed=1,reaped=0;
    struct pollfd p={report_pipe[0],POLLIN,0};pid_t ids[WORKERS];
    if(poll(&p,1,5000)!=1||read(report_pipe[0],ids,sizeof(ids))!=sizeof(ids))passed=0;
    if(passed) {
        int signal=case_signal(which);
        long sent=0;
        if(which==TERM_THREAD||which==KILL_THREAD)
            sent=syscall(SYS_tgkill,child,ids[0],signal);
        else if(which!=ALARM_GROUP&&which!=LEADER_EXIT)sent=kill(child,signal);
        if(sent<0)passed=0;
        if(which==MASKED_TERM||which==MASKED_USR2||which==IGNORED_TERM||which==LEADER_EXIT) {
            pause_ms(100);
            pid_t got=waitpid(child,&status,WNOHANG);
            if(got!=0){passed=0;reaped=got==child;}
            if(!reaped&&write(control_pipe[1],"x",1)!=1)passed=0;
        }
        if(!reaped) {
            int got=wait_bounded(child,&status,which==ALARM_GROUP?3000:1500);
            reaped=got==1;if(!reaped) {
                printf("FATALGROUPCHECK timeout %s pid=%ld\n",names[which],(long)child);
                passed=0;
            }
        }
        if(passed) {
            if(which==HANDLED_TERM||which==IGNORED_TERM)
                passed=WIFEXITED(status)&&WEXITSTATUS(status)==0;
            else if(which==LEADER_EXIT)passed=WIFEXITED(status);
            else passed=WIFSIGNALED(status)&&WTERMSIG(status)==(which==ALARM_GROUP?SIGALRM:signal);
        }
    }
    if(!reaped) {kill(child,SIGKILL);if(wait_bounded(child,&status,2000)!=1) {
        printf("FATALGROUPCHECK cleanup failed pid=%ld\n",(long)child);return -1;
    }}
    close(report_pipe[0]);close(control_pipe[1]);close(data_pipe[0]);close(data_pipe[1]);
    printf("FATALGROUPCHECK %s %s status=%d\n",passed?"PASS":"FAIL",names[which],status);
    return passed;
}
int main(int argc,char **argv) {
    unsigned rounds=1;if(argc>2)return 2;
    if(argc==2) {char *end;unsigned long n=strtoul(argv[1],&end,10);
        if(!*argv[1]||*end||n<1||n>32)return 2;
        rounds=(unsigned)n;}
    setvbuf(stdout,NULL,_IONBF,0);signal(SIGPIPE,SIG_IGN);
    unsigned failures=0;
    for(unsigned round=0;round<rounds;++round)for(int which=0;which<CASES;++which) {
        int result=run_case(which);if(result<0)return 1;if(!result)++failures;
    }
    printf("FATALGROUPCHECK %s rounds=%u cases=%u failures=%u\n",failures?"FAIL":"PASS",rounds,CASES*rounds,failures);
    return failures?1:0;
}
