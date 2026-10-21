#define _GNU_SOURCE
/* Bounded raw x86-64 syscall signal semantics; host and guest use the same ELF.
 * Each child owns one case; parent watchdogs and reaps every child. */
#include <errno.h>
#include <pthread.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
static volatile sig_atomic_t caught;
static uint32_t cell;
static void handler(int sig){(void)sig;++caught;}
static uint64_t now(int clock){struct timespec t;if(syscall(SYS_clock_gettime,clock,&t))_exit(90);return (uint64_t)t.tv_sec*1000000000ULL+t.tv_nsec;}
static void pause_ns(long ns){struct timespec t={0,ns};while(nanosleep(&t,&t)&&errno==EINTR){}}
static void* futex_waker(void* unused){(void)unused;sigset_t m;sigemptyset(&m);sigaddset(&m,SIGUSR1);pthread_sigmask(SIG_BLOCK,&m,0);pause_ns(200000000);__atomic_store_n(&cell,1,__ATOMIC_RELEASE);syscall(SYS_futex,&cell,129,1,0,0,0);return 0;}
static const char* names[]={"nanosleep","clock_relative","clock_absolute_mono","clock_absolute_real","poll","ppoll_mask","pselect_mask","epoll_pwait","futex_relative","futex_absolute_mono","futex_absolute_real","read_restart","futex_infinite_restart"};
static int run_case(unsigned kind,int ready,int data){
    caught=0;cell=0;struct sigaction sa={0};sa.sa_handler=handler;sa.sa_flags=SA_RESTART;sigemptyset(&sa.sa_mask);if(sigaction(SIGUSR1,&sa,0))return 91;
    alarm(3);const uint64_t duration=300000000;struct timespec request={0,duration},remaining={-7,-9};
    const int clock=kind==3||kind==10?CLOCK_REALTIME:CLOCK_MONOTONIC;
    if(kind==2||kind==3||kind==9||kind==10){uint64_t target=now(clock)+duration;request.tv_sec=target/1000000000;request.tv_nsec=target%1000000000;}
    sigset_t block;sigemptyset(&block);sigaddset(&block,SIGUSR1);
    const int temporary=kind==5||kind==6;if(temporary&&sigprocmask(SIG_BLOCK,&block,0))return 92;
    pthread_t worker;if(kind==12&&pthread_create(&worker,0,futex_waker,0))return 93;
    struct {uint64_t address,size;} maskarg;uint64_t empty=0;maskarg.address=(uintptr_t)&empty;maskarg.size=8;
    struct __attribute__((packed)){uint32_t events;uint64_t data;} event;
    int epfd=kind==7?syscall(SYS_epoll_create1,0):-1;if(kind==7&&epfd<0)return 94;
    if(write(ready,"R",1)!=1)return 95;
    close(ready);
    uint64_t started=now(CLOCK_MONOTONIC);errno=0;long result=-99;
    switch(kind){
    case 0:result=syscall(SYS_nanosleep,&request,&remaining);break;
    case 1:result=syscall(SYS_clock_nanosleep,CLOCK_MONOTONIC,0,&request,&remaining);break;
    case 2:case 3:result=syscall(SYS_clock_nanosleep,clock,1,&request,&remaining);break;
    case 4:result=syscall(SYS_poll,0,0,300);break;
    case 5:result=syscall(SYS_ppoll,0,0,&request,&empty,8);break;
    case 6:result=syscall(SYS_pselect6,0,0,0,0,&request,&maskarg);break;
    case 7:result=syscall(SYS_epoll_pwait,epfd,&event,1,300,0,8);break;
    case 8:result=syscall(SYS_futex,&cell,128,0,&request,0,0xffffffffu);break;
    case 9:case 10:result=syscall(SYS_futex,&cell,9|128|(clock==CLOCK_REALTIME?256:0),0,&request,0,0xffffffffu);break;
    case 11:{char byte;result=syscall(SYS_read,data,&byte,1);break;}
    case 12:result=syscall(SYS_futex,&cell,128,0,0,0,0);break;
    }
    int error=result==-1?errno:0;uint64_t finished=now(CLOCK_MONOTONIC);
    if(kind==12)pthread_join(worker,0);
    if(epfd>=0)close(epfd);
    sigset_t after;sigprocmask(SIG_SETMASK,0,&after);
    int mask_ok=!temporary||sigismember(&after,SIGUSR1)==1;
    int rem_ok=(kind==0||kind==1)?remaining.tv_sec>=0&&remaining.tv_nsec>=0&&remaining.tv_nsec<1000000000L&&((uint64_t)remaining.tv_sec*1000000000ULL+remaining.tv_nsec)>0&&((uint64_t)remaining.tv_sec*1000000000ULL+remaining.tv_nsec)<=duration:(kind==2||kind==3)?remaining.tv_sec==-7&&remaining.tv_nsec==-9:1;
    int passed=caught==1&&mask_ok&&rem_ok&&(kind==11?result==1:kind==12?result==0:result==-1&&error==EINTR);
    printf("CASE {\"name\":\"%s\",\"result\":%ld,\"errno\":%d,\"signals\":%d,\"elapsed_ns\":%llu,\"remaining_sec\":%ld,\"remaining_nsec\":%ld,\"mask_ok\":%s,\"remaining_ok\":%s,\"passed\":%s}\n",names[kind],result,error,caught,(unsigned long long)(finished-started),remaining.tv_sec,remaining.tv_nsec,mask_ok?"true":"false",rem_ok?"true":"false",passed?"true":"false");fflush(stdout);return passed?0:1;
}
int main(void){
    signal(SIGPIPE,SIG_IGN);
    unsigned failed=0;
    for(unsigned kind=0;kind<sizeof(names)/sizeof(names[0]);++kind){
        int ready[2],data[2];if(pipe(ready)||pipe(data))return 80;pid_t child=fork();if(child<0)return 81;
        if(!child){close(ready[0]);close(data[1]);int result=run_case(kind,ready[1],data[0]);_exit(result);}
        close(ready[1]);close(data[0]);char byte;struct pollfd startup={ready[0],POLLIN,0};
        int available=poll(&startup,1,4000);ssize_t got=available>0?read(ready[0],&byte,1):-1;close(ready[0]);
        if(got!=1)kill(child,SIGKILL);
        if(got==1){pause_ns(50000000);kill(child,SIGUSR1);if(kind==11){pause_ns(150000000);write(data[1],"X",1);}}
        close(data[1]);int status=0;uint64_t end=now(CLOCK_MONOTONIC)+4000000000ULL;pid_t collected=0;
        while(!(collected=waitpid(child,&status,WNOHANG))&&now(CLOCK_MONOTONIC)<end)pause_ns(5000000);
        if(!collected){kill(child,SIGKILL);collected=waitpid(child,&status,0);}
        int good=collected==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0;
        if(!good)++failed;
        printf("CHILD {\"name\":\"%s\",\"wait_status\":%d,\"reaped\":%s,\"passed\":%s}\n",names[kind],status,collected==child?"true":"false",good?"true":"false");fflush(stdout);
    }
    printf("SIGNAL_WAITS_RESULT {\"cases\":13,\"failed\":%u,\"passed\":%s}\n",failed,failed?"false":"true");return failed?1:0;
}
