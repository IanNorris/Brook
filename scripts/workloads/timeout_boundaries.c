#define _GNU_SOURCE
/* Structurally valid huge timeouts remain interruptible on Linux. A short
 * independent timer bounds each wait; parent watchdogs reap every child. */
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

struct event_abi {uint64_t value;int32_t signo,notify,target;unsigned char padding[44];};
static volatile sig_atomic_t caught;
static uint32_t cell;
static const char *names[]={"huge_nanosleep","huge_clock_relative_mono","huge_clock_absolute_mono",
    "huge_clock_absolute_real","huge_clock_relative_real","huge_futex_relative","huge_futex_absolute_mono",
    "huge_futex_absolute_real","huge_sigtimedwait","huge_ppoll","huge_pselect",
    "huge_timerfd_relative","huge_timerfd_absolute","huge_posix_relative","huge_posix_absolute",
    "negative_nanosleep","invalid_nsec_clock","negative_futex","invalid_posix_nsec","invalid_timerfd_nsec",
    "finite_futex_timeout","past_absolute_sleep","zero_relative_sleep","invalid_ppoll_nsec"};
static void handler(int signal){(void)signal;++caught;}
static uint64_t now(void){struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))_exit(90);return (uint64_t)t.tv_sec*1000000000ULL+t.tv_nsec;}
static void delay(void){struct timespec t={0,1000000};while(nanosleep(&t,&t)&&errno==EINTR){}}
static int create(int clock,int notify,int signo){struct event_abi event={.notify=notify,.signo=signo};int32_t id=-1;if(syscall(SYS_timer_create,clock,&event,&id))return -1;return id;}
static int run_case(unsigned kind){
    caught=0;struct sigaction action={0};action.sa_handler=handler;sigemptyset(&action.sa_mask);
    if(sigaction(SIGUSR1,&action,0))return 91;
    alarm(3);int guard=create(CLOCK_MONOTONIC,SIGEV_SIGNAL,SIGUSR1);if(guard<0)return 92;
    struct itimerspec interrupt={.it_value={0,200000000}};
    if(syscall(SYS_timer_settime,guard,0,&interrupt,0))return 93;
    struct timespec request={INT64_MAX,999999999},remaining={-7,-9};
    if(kind==15||kind==17)request=(struct timespec){-1,0};
    if(kind==16||kind==18||kind==19||kind==23)request=(struct timespec){0,1000000000};
    if(kind==20)request=(struct timespec){0,20000000};
    if(kind==21)request=(struct timespec){0,1};
    if(kind==22)request=(struct timespec){0,0};
    uint64_t start=now();errno=0;long result=-99;int error=0,fd=-1,id=-1,query=-99,good=0;
    struct itimerspec after={0};
    switch(kind){
    case 0:case 15:case 22:result=syscall(SYS_nanosleep,&request,&remaining);break;
    case 1:case 2:case 3:case 4:case 16:case 21:
        result=syscall(SYS_clock_nanosleep,(kind==3||kind==4)?CLOCK_REALTIME:CLOCK_MONOTONIC,
            kind==2||kind==3||kind==16||kind==21?1:0,&request,&remaining);break;
    case 5:case 6:case 7:case 17:case 20:
        result=syscall(SYS_futex,&cell,(kind==6||kind==7?9:0)|128|(kind==7?256:0),0,&request,0,0xffffffffu);break;
    case 8:{uint64_t set=1ULL<<(SIGUSR2-1);siginfo_t info;result=syscall(SYS_rt_sigtimedwait,&set,&info,&request,8);break;}
    case 9:case 23:result=syscall(SYS_ppoll,0,0,&request,0,8);break;
    case 10:result=syscall(SYS_pselect6,0,0,0,0,&request,0);break;
    case 11:case 12:case 19:{
        fd=syscall(SYS_timerfd_create,kind==12?CLOCK_REALTIME:CLOCK_MONOTONIC,TFD_NONBLOCK);if(fd<0)return 94;
        struct itimerspec value={.it_interval={INT64_MAX,999999999},.it_value=request};
        result=syscall(SYS_timerfd_settime,fd,kind==12?1:0,&value,0);error=errno;
        query=syscall(SYS_timerfd_gettime,fd,&after);break;}
    case 13:case 14:case 18:{
        id=create(kind==14?CLOCK_REALTIME:CLOCK_MONOTONIC,SIGEV_NONE,SIGUSR2);if(id<0)return 95;
        struct itimerspec value={.it_interval={INT64_MAX,999999999},.it_value=request};
        result=syscall(SYS_timer_settime,id,kind==14?1:0,&value,0);error=errno;
        query=syscall(SYS_timer_gettime,id,&after);break;}
    }
    if(fd<0&&id<0)error=errno;
    uint64_t elapsed=now()-start;
    if(kind<=10)good=result==-1&&error==EINTR&&caught==1;
    if(kind>=11&&kind<=14)good=result==0&&query==0&&after.it_value.tv_sec>1000000000&&after.it_interval.tv_sec==9223372036LL&&after.it_interval.tv_nsec==854775807;
    if((kind>=15&&kind<=19)||kind==23)good=result==-1&&error==EINVAL&&!caught;
    if(kind==20)good=result==-1&&error==ETIMEDOUT&&!caught&&elapsed>=20000000;
    if(kind==21||kind==22)good=result==0&&!caught;
    if(kind==0||kind==1||kind==4)good=good&&remaining.tv_sec>0&&remaining.tv_nsec>=0&&remaining.tv_nsec<1000000000L;
    if(kind==2||kind==3)good=good&&remaining.tv_sec==-7&&remaining.tv_nsec==-9;
    syscall(SYS_timer_delete,guard);if(id>=0)syscall(SYS_timer_delete,id);if(fd>=0)close(fd);
    printf("CASE {\"name\":\"%s\",\"result\":%ld,\"errno\":%d,\"caught\":%d,\"elapsed_ns\":%llu,\"remaining_sec\":%ld,\"remaining_nsec\":%ld,\"timer_sec\":%ld,\"interval_sec\":%ld,\"interval_nsec\":%ld,\"passed\":%s}\n",
        names[kind],result,error,caught,(unsigned long long)elapsed,remaining.tv_sec,remaining.tv_nsec,after.it_value.tv_sec,after.it_interval.tv_sec,after.it_interval.tv_nsec,good?"true":"false");fflush(stdout);return good?0:1;
}
int main(void){
    unsigned failed=0;
    for(unsigned kind=0;kind<sizeof(names)/sizeof(names[0]);++kind){
        pid_t child=fork();if(child<0)return 80;if(!child)_exit(run_case(kind));
        int status=0;pid_t collected=0;uint64_t end=now()+4000000000ULL;
        while(!(collected=waitpid(child,&status,WNOHANG))&&now()<end)delay();
        if(!collected){kill(child,SIGKILL);collected=waitpid(child,&status,0);}
        int good=collected==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0;if(!good)++failed;
        printf("CHILD {\"name\":\"%s\",\"wait_status\":%d,\"reaped\":%s,\"passed\":%s}\n",names[kind],status,collected==child?"true":"false",good?"true":"false");fflush(stdout);
    }
    printf("TIMEOUT_BOUNDARIES_RESULT {\"cases\":24,\"failed\":%u,\"passed\":%s}\n",failed,failed?"false":"true");return failed?1:0;
}
