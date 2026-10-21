#define _GNU_SOURCE
/* Same finite x86-64 ELF on Linux/Brook. Parent observes sleeping state,
 * sends SIGCONT, rescues a missed wake, and reaps every original child. */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t caught, rescued;
static uint32_t cell;
static const char *names[]={"caught_poll","caught_futex","caught_epoll","caught_read",
    "default_read","ignored_read","masked_default_wait","masked_ignored_wait",
    "stopped_default_read","stopped_ignored_read","stopped_masked_read"};
static void handler(int sig){if(sig==SIGCONT)++caught;else ++rescued;}
static uint64_t now(void){struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))_exit(90);return (uint64_t)t.tv_sec*1000000000ULL+t.tv_nsec;}
static void delay(long ns){struct timespec t={ns/1000000000L,ns%1000000000L};while(nanosleep(&t,&t)&&errno==EINTR){}}
static int readable(int fd,unsigned ms){
    uint64_t end=now()+(uint64_t)ms*1000000ULL;
    for(;;){
        uint64_t stamp=now();if(stamp>=end)return 0;
        struct pollfd item={fd,POLLIN,0};int result=poll(&item,1,(end-stamp+999999)/1000000);
        if(result<0&&errno==EINTR)continue;
        return result>0&&(item.revents&POLLIN);
    }
}
static char state(pid_t child){
    char path[64],buffer[1024];snprintf(path,sizeof(path),"/proc/%d/stat",child);
    int fd=open(path,O_RDONLY);if(fd<0)return '?';ssize_t n=read(fd,buffer,sizeof(buffer)-1);close(fd);
    if(n<=0)return '?';
    buffer[n]=0;char *end=strrchr(buffer,')');return end&&end[1]==' '?end[2]:'?';
}
static int child_case(unsigned kind,int ready,int data,int done){
    caught=rescued=0;struct sigaction sa={0};sa.sa_handler=handler;sigemptyset(&sa.sa_mask);
    if(sigaction(SIGUSR1,&sa,0))return 91;
    const int masked=kind==6||kind==7||kind==10;
    sa.sa_handler=(kind==4||kind==6||kind==8)?SIG_DFL:(kind==5||kind==7||kind==9)?SIG_IGN:handler;
    if(sigaction(SIGCONT,&sa,0))return 92;
    sigset_t mask;sigemptyset(&mask);sigaddset(&mask,SIGCONT);
    if(masked&&sigprocmask(SIG_BLOCK,&mask,0))return 93;
    int epfd=kind==2?syscall(SYS_epoll_create1,0):-1;if(kind==2&&epfd<0)return 94;
    alarm(4);if(write(ready,"R",1)!=1)return 95;close(ready);
    struct __attribute__((packed)){uint32_t events;uint64_t data;} event;
    uint64_t waitset=1ULL<<(SIGCONT-1);char byte=0;errno=0;uint64_t started=now();long result;
    if(kind==0)result=syscall(SYS_poll,0,0,-1);
    else if(kind==1)result=syscall(SYS_futex,&cell,128,0,0,0,0);
    else if(kind==2)result=syscall(SYS_epoll_pwait,epfd,&event,1,-1,0,8);
    else if(kind==6||kind==7)result=syscall(SYS_rt_sigtimedwait,&waitset,0,0,8);
    else result=syscall(SYS_read,data,&byte,1);
    int error=result==-1?errno:0;uint64_t elapsed=now()-started;
    sigset_t pending;sigpending(&pending);int pending_cont=sigismember(&pending,SIGCONT);
    int good=!rescued&&(kind<4?result==-1&&error==EINTR&&caught==1:
        (kind==6||kind==7)?result==SIGCONT&&caught==0&&!pending_cont:
        result==1&&byte=='X'&&caught==0&&(kind!=10||pending_cont==1));
    printf("CASE {\"name\":\"%s\",\"result\":%ld,\"errno\":%d,\"caught\":%d,\"rescued\":%d,\"pending_cont\":%d,\"elapsed_ns\":%llu,\"passed\":%s}\n",
        names[kind],result,error,caught,rescued,pending_cont,(unsigned long long)elapsed,good?"true":"false");fflush(stdout);
    if(write(done,"D",1)!=1)return 96;
    if(epfd>=0)close(epfd);
    return good?0:1;
}
int main(void){
    signal(SIGPIPE,SIG_IGN);unsigned failed=0;
    for(unsigned kind=0;kind<sizeof(names)/sizeof(names[0]);++kind){
        int ready[2],data[2],done[2];if(pipe(ready)||pipe(data)||pipe(done))return 80;
        pid_t child=fork();if(child<0)return 81;
        if(!child){close(ready[0]);close(data[1]);close(done[0]);_exit(child_case(kind,ready[1],data[0],done[1]));}
        close(ready[1]);close(data[0]);close(done[1]);char byte=0;
        int startup_ok=readable(ready[0],4000)&&read(ready[0],&byte,1)==1;
        close(ready[0]);uint64_t end=now()+2000000000ULL;char observed='?';
        while(startup_ok&&(observed=state(child))!='S'&&now()<end)delay(1000000);
        int blocked=startup_ok&&observed=='S',stop_ok=1,continued_ok=1;
        if(blocked&&kind>=8){
            kill(child,SIGSTOP);int status=0;pid_t got=0;end=now()+2000000000ULL;
            while(!(got=waitpid(child,&status,WUNTRACED|WNOHANG))&&now()<end)delay(1000000);
            stop_ok=got==child&&WIFSTOPPED(status)&&WSTOPSIG(status)==SIGSTOP;
        }
        if(blocked&&stop_ok)kill(child,SIGCONT);else kill(child,SIGKILL);
        if(blocked&&stop_ok&&kind>=8){
            int status=0;pid_t got=0;end=now()+500000000ULL;
            while(!(got=waitpid(child,&status,WCONTINUED|WNOHANG))&&now()<end)delay(1000000);
            continued_ok=got==child&&WIFCONTINUED(status);
        }
        if(kind==4||kind==5||kind>=8){delay(150000000);write(data[1],"X",1);}
        int completed_before_rescue=readable(done[0],300);
        if(!completed_before_rescue)kill(child,SIGUSR1);
        close(data[1]);int status=0;pid_t got=0;end=now()+2000000000ULL;
        while(!(got=waitpid(child,&status,WNOHANG))&&now()<end)delay(1000000);
        if(!got){kill(child,SIGKILL);got=waitpid(child,&status,0);}
        close(done[0]); // retain the completion reader while a rescued child unwinds
        int good=blocked&&stop_ok&&continued_ok&&completed_before_rescue&&got==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0;
        if(!good)++failed;
        printf("CHILD {\"name\":\"%s\",\"blocked_before_signal\":%s,\"stop_ok\":%s,\"continued_ok\":%s,\"completed_before_rescue\":%s,\"wait_status\":%d,\"reaped\":%s,\"passed\":%s}\n",
            names[kind],blocked?"true":"false",stop_ok?"true":"false",continued_ok?"true":"false",completed_before_rescue?"true":"false",status,got==child?"true":"false",good?"true":"false");fflush(stdout);
    }
    printf("SIGCONT_WAITS_RESULT {\"cases\":11,\"failed\":%u,\"passed\":%s}\n",failed,failed?"false":"true");return failed?1:0;
}
