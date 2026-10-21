#define _GNU_SOURCE
/* Finite, unchanged Linux/Brook x86-64 timer syscall probe. Every case has
 * an alarm and a parent watchdog; original children are always reaped. */
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

struct event_abi {uint64_t value;int32_t signo,notify,target;unsigned char padding[44];};
static volatile sig_atomic_t caught,code;
static uint64_t delivered_value;
static uint32_t cell;
static int worker_tid,worker_good;
static const uint64_t cookie=0x123456789ULL,bit=1ULL<<(SIGUSR1-1);
static const char *names[]={"relative_mono_sleep","relative_real_poll","absolute_mono_futex",
    "absolute_real_epoll","pending_sigsuspend","masked_sigwait","masked_ignored_sigwait",
    "ignored_sleep","none_sleep","timerfd_relative_mono","timerfd_absolute_real",
    "periodic_overrun","deleted_timer","invalid_timerfd","invalid_posix_timer",
    "thread_target","disarmed_pending","timerfd_rearm_clears_pending"};
static uint64_t now(int clock){struct timespec t;if(clock_gettime(clock,&t))_exit(90);return (uint64_t)t.tv_sec*1000000000ULL+t.tv_nsec;}
static struct timespec spec(uint64_t ns){return (struct timespec){ns/1000000000ULL,ns%1000000000ULL};}
static void delay(uint64_t ns){struct timespec t=spec(ns);while(nanosleep(&t,&t)&&errno==EINTR){}}
static void handler(int sig,siginfo_t *info,void *context){(void)sig;(void)context;++caught;code=info->si_code;delivered_value=(uintptr_t)info->si_value.sival_ptr;}
static int create(int clock,int notify,int target){
    struct event_abi event={.value=cookie,.signo=SIGUSR1,.notify=notify,.target=target};int32_t id=-1;
    if(syscall(SYS_timer_create,clock,&event,&id))return -1;
    return id;
}
static int set(int id,int flags,uint64_t initial,uint64_t interval){
    struct itimerspec value={.it_value=spec(initial),.it_interval=spec(interval)};
    return syscall(SYS_timer_settime,id,flags,&value,0);
}
static int wait_signal(siginfo_t *info){struct timespec timeout={1,0};return syscall(SYS_rt_sigtimedwait,&bit,info,&timeout,8);}
static void* targeted(void *unused){
    (void)unused;__atomic_store_n(&worker_tid,(int)syscall(SYS_gettid),__ATOMIC_RELEASE);
    siginfo_t info={0};int result=wait_signal(&info);
    worker_good=result==SIGUSR1&&info.si_code==SI_TIMER&&(uintptr_t)info.si_value.sival_ptr==cookie;
    return 0;
}
static int run_case(unsigned kind){
    caught=code=0;delivered_value=0;worker_tid=worker_good=0;
    struct sigaction action={0};sigemptyset(&action.sa_mask);action.sa_flags=SA_SIGINFO;action.sa_sigaction=handler;
    if(kind==6||kind==7)action.sa_handler=SIG_IGN;
    if(sigaction(SIGUSR1,&action,0))return 91;
    const int masked=kind==4||kind==5||kind==6||kind==11||kind==15||kind==16;
    sigset_t mask;sigemptyset(&mask);sigaddset(&mask,SIGUSR1);if(masked&&sigprocmask(SIG_BLOCK,&mask,0))return 92;
    alarm(3);int clock=(kind==1||kind==3||kind==10)?CLOCK_REALTIME:CLOCK_MONOTONIC;
    int absolute=kind==2||kind==3||kind==10;
    int timerfd=kind==9||kind==10||kind==13||kind==17;
    pthread_t worker;
    if(kind==15){
        if(pthread_create(&worker,0,targeted,0))return 93;
        uint64_t end=now(CLOCK_MONOTONIC)+1000000000ULL;
        while(!__atomic_load_n(&worker_tid,__ATOMIC_ACQUIRE)&&now(CLOCK_MONOTONIC)<end)delay(1000000);
        if(!worker_tid)return 94;
    }
    int id=timerfd?(int)syscall(SYS_timerfd_create,clock,TFD_NONBLOCK):create(clock,kind==8?SIGEV_NONE:kind==15?4:SIGEV_SIGNAL,worker_tid);
    if(id<0)return 95;
    struct itimerspec value={.it_value=spec((absolute?now(clock):0)+25000000ULL)};
    if(kind==11)value.it_interval=spec(10000000);
    uint64_t start=now(CLOCK_MONOTONIC);errno=0;long result=-99;int good=0,error=0,detail=0;
    if(kind==13||kind==14){
        value.it_value.tv_nsec=1000000000;
        result=syscall(timerfd?SYS_timerfd_settime:SYS_timer_settime,id,0,&value,0);error=errno;
        struct itimerspec after={0};int query=syscall(timerfd?SYS_timerfd_gettime:SYS_timer_gettime,id,&after);
        good=result==-1&&error==EINVAL&&!query&&!after.it_value.tv_sec&&!after.it_value.tv_nsec;
    }else{
        if(syscall(timerfd?SYS_timerfd_settime:SYS_timer_settime,id,absolute?1:0,&value,0))return 96;
        if(kind==12){if(syscall(SYS_timer_delete,id))return 97;id=-1;}
        switch(kind){
        case 0:{struct timespec request={1,0};result=syscall(SYS_nanosleep,&request,0);error=errno;break;}
        case 1:result=syscall(SYS_poll,0,0,-1);error=errno;break;
        case 2:result=syscall(SYS_futex,&cell,128,0,0,0,0);error=errno;break;
        case 3:{struct __attribute__((packed)){uint32_t events;uint64_t data;} event;
            int epfd=syscall(SYS_epoll_create1,0);if(epfd<0)return 98;
            result=syscall(SYS_epoll_pwait,epfd,&event,1,-1,0,8);error=errno;close(epfd);break;}
        case 4:{delay(60000000);uint64_t pending=0;syscall(SYS_rt_sigpending,&pending,8);detail=(pending&bit)!=0;
            uint64_t empty=0;result=syscall(SYS_rt_sigsuspend,&empty,8);error=errno;break;}
        case 5:case 6:case 11:case 16:{
            if(kind==11||kind==16)delay(85000000);
            if(kind==16&&set(id,0,0,0))return 99;
            siginfo_t info={0};result=wait_signal(&info);error=errno;
            good=result==SIGUSR1&&info.si_code==SI_TIMER&&(uintptr_t)info.si_value.sival_ptr==cookie&&!caught;
            if(kind==11){detail=syscall(SYS_timer_getoverrun,id);good=good&&info.si_overrun>=3&&detail==info.si_overrun;}
            break;}
        case 7:case 8:case 12:{struct timespec request=spec(85000000);result=syscall(SYS_nanosleep,&request,0);error=errno;good=result==0&&!caught&&now(CLOCK_MONOTONIC)-start>=85000000;break;}
        case 9:case 10:{struct pollfd p={id,POLLIN,0};result=poll(&p,1,500);uint64_t count=0;
            detail=read(id,&count,8);good=result==1&&(p.revents&POLLIN)&&detail==8&&count==1;break;}
        case 15:pthread_join(worker,0);result=worker_good;good=worker_good&&!caught;break;
        case 17:{delay(60000000);value.it_value=spec(25000000);if(syscall(SYS_timerfd_settime,id,0,&value,0))return 100;
            uint64_t count=0;result=read(id,&count,8);error=errno;good=result==-1&&error==EAGAIN;break;}
        }
        if(kind<=4)good=result==-1&&error==EINTR&&caught==1&&code==SI_TIMER&&delivered_value==cookie&&(kind!=4||detail);
    }
    if(id>=0){if(timerfd)close(id);else if(syscall(SYS_timer_delete,id))good=0;}
    printf("CASE {\"name\":\"%s\",\"result\":%ld,\"errno\":%d,\"caught\":%d,\"si_code\":%d,\"value\":%llu,\"detail\":%d,\"elapsed_ns\":%llu,\"passed\":%s}\n",
        names[kind],result,error,caught,code,(unsigned long long)delivered_value,detail,(unsigned long long)(now(CLOCK_MONOTONIC)-start),good?"true":"false");fflush(stdout);
    return good?0:1;
}
int main(void){
    unsigned failed=0;
    for(unsigned kind=0;kind<sizeof(names)/sizeof(names[0]);++kind){
        pid_t child=fork();if(child<0)return 80;if(!child)_exit(run_case(kind));
        uint64_t end=now(CLOCK_MONOTONIC)+4000000000ULL;int status=0;pid_t collected=0;
        while(!(collected=waitpid(child,&status,WNOHANG))&&now(CLOCK_MONOTONIC)<end)delay(1000000);
        if(!collected){kill(child,SIGKILL);collected=waitpid(child,&status,0);}
        int good=collected==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0;if(!good)++failed;
        printf("CHILD {\"name\":\"%s\",\"wait_status\":%d,\"reaped\":%s,\"passed\":%s}\n",names[kind],status,collected==child?"true":"false",good?"true":"false");fflush(stdout);
    }
    printf("POSIX_TIMER_WAITS_RESULT {\"cases\":18,\"failed\":%u,\"passed\":%s}\n",failed,failed?"false":"true");return failed?1:0;
}
