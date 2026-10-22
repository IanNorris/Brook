#define _GNU_SOURCE
#include <sys/signalfd.h>
#include <sys/epoll.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <poll.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
static void require(int ok,const char *stage) {
    if (!ok) {fprintf(stderr,"SIGNALFD_PROBE_FAIL stage=%s errno=%d\n",stage,errno);exit(1);}
}
static void expired(int signo) {(void)signo;_exit(124);}
static struct signalfd_siginfo consume(int fd,const char *stage) {
    struct signalfd_siginfo info={0};
    require(read(fd,&info,sizeof(info))==sizeof(info),stage);return info;
}
static void reap(pid_t pid,int expected,const char *stage) {
    int status=0;require(waitpid(pid,&status,0)==pid && WIFEXITED(status) && WEXITSTATUS(status)==expected,stage);
}
static int worker_fd;
static pthread_barrier_t worker_barrier;
static struct signalfd_siginfo worker_info;
static void *worker_read(void *unused) {
    (void)unused;
    struct signalfd_siginfo empty;errno=0;
    require(read(worker_fd,&empty,sizeof(empty))==-1 && errno==EAGAIN,
            "worker-cannot-steal-leader-private-signal");
    (void)pthread_barrier_wait(&worker_barrier);
    struct pollfd p={worker_fd,POLLIN,0};
    require(poll(&p,1,2000)==1 && (p.revents&POLLIN),"worker-shared-child-ready");
    worker_info=consume(worker_fd,"worker-shared-child-read");return NULL;
}
int main(int argc,char **argv) {
    if (argc==3 && !strcmp(argv[1],"--closed")) {
        errno=0;_exit(fcntl(atoi(argv[2]),F_GETFD)==-1 && errno==EBADF?0:40);
    }
    signal(SIGALRM,expired);alarm(15);
    sigset_t mask;sigemptyset(&mask);sigaddset(&mask,SIGUSR1);sigaddset(&mask,SIGUSR2);sigaddset(&mask,SIGCHLD);sigaddset(&mask,SIGWINCH);
    require(sigprocmask(SIG_BLOCK,&mask,NULL)==0,"block-signals");
    int fd=signalfd(-1,&mask,SFD_NONBLOCK|SFD_CLOEXEC);require(fd>=0,"create");
    require((fcntl(fd,F_GETFD)&FD_CLOEXEC) && (fcntl(fd,F_GETFL)&O_NONBLOCK),"flags");
    struct pollfd p={fd,POLLIN,0};require(poll(&p,1,0)==0,"empty-poll");
    struct signalfd_siginfo info={0};errno=0;
    require(read(fd,&info,sizeof(info))==-1 && errno==EAGAIN,"empty-nonblock");
    errno=0;require(read(fd,&info,127)==-1 && errno==EINVAL,"short-record");
    errno=0;require(write(fd,&info,sizeof(info))==-1 && errno==EINVAL,"not-writable");
    require(lseek(fd,0,SEEK_SET)==0,"linux-noop-seek");
    require(kill(getpid(),SIGUSR1)==0,"send-self");
    p.revents=0;require(poll(&p,1,1000)==1 && (p.revents&POLLIN),"ready-poll");
    info=consume(fd,"read-self");require(info.ssi_signo==SIGUSR1 && info.ssi_pid==(unsigned)getpid() && info.ssi_uid==(unsigned)getuid(),"sender-info");
    require(kill(getpid(),SIGWINCH)==0,"send-blocked-default-ignore");
    info=consume(fd,"blocked-default-ignore-readable");require(info.ssi_signo==SIGWINCH,"blocked-default-ignore-signo");
    require(signal(SIGUSR1,SIG_IGN)!=SIG_ERR,"install-ignore");
    require(kill(getpid(),SIGUSR1)==0,"send-blocked-explicit-ignore");
    info=consume(fd,"blocked-explicit-ignore-readable");require(info.ssi_signo==SIGUSR1,"blocked-explicit-ignore-signo");
    require(signal(SIGUSR1,SIG_DFL)!=SIG_ERR,"restore-default");
    int alias=dup(fd);require(alias>=0,"dup");
    sigset_t only;sigemptyset(&only);sigaddset(&only,SIGUSR2);
    require(signalfd(alias,&only,0)==alias,"update-shared-mask");
    require(kill(getpid(),SIGUSR1)==0,"send-excluded");errno=0;
    require(read(fd,&info,sizeof(info))==-1 && errno==EAGAIN,"excluded-remains-pending");
    require(kill(getpid(),SIGUSR2)==0,"send-included");info=consume(fd,"read-updated-mask");require(info.ssi_signo==SIGUSR2,"updated-mask-visible-through-alias");
    require(signalfd(alias,&mask,0)==alias,"restore-mask");
    // Fork shares the description but cannot read the parent's pending signals.
    pid_t child=fork();require(child>=0,"fork");
    if (!child) {
        errno=0;if(read(alias,&info,sizeof(info))!=-1 || errno!=EAGAIN)_exit(41);
        if(kill(getpid(),SIGUSR2))_exit(42);
        info=consume(alias,"child-own-signal");
        _exit(info.ssi_signo==SIGUSR2 && info.ssi_pid==(unsigned)getpid()?37:43);
    }
    info=consume(alias,"parent-pending-preserved");require(info.ssi_signo==SIGUSR1,"parent-signal");
    p.fd=fd;p.revents=0;require(poll(&p,1,2000)==1 && (p.revents&POLLIN),"child-exit-poll");
    info=consume(fd,"child-exit-record");
    require(info.ssi_signo==SIGCHLD && info.ssi_pid==(unsigned)child && info.ssi_code==CLD_EXITED && info.ssi_status==37,"child-exit-metadata");
    reap(child,37,"child-still-waitable");
    // Blocking read and epoll consume real signals, not synthetic readiness.
    int blocking=signalfd(-1,&mask,0);require(blocking>=0,"blocking-create");
    pid_t parent=getpid();child=fork();require(child>=0,"blocking-fork");
    if (!child) {usleep(20000);_exit(kill(parent,SIGUSR1)==0?0:44);}
    info=consume(blocking,"blocking-read-wake");require(info.ssi_signo==SIGUSR1,"blocking-selected-signal");
    reap(child,0,"blocking-child-reaped");
    info=consume(blocking,"blocking-child-signal");require(info.ssi_signo==SIGCHLD,"blocking-child-signo");
    int ep=epoll_create1(EPOLL_CLOEXEC);require(ep>=0,"epoll-create");
    struct epoll_event event={.events=EPOLLIN,.data.u64=0x123456};
    require(epoll_ctl(ep,EPOLL_CTL_ADD,fd,&event)==0,"epoll-add");
    child=fork();require(child>=0,"epoll-fork");
    if (!child) {usleep(20000);_exit(kill(parent,SIGUSR2)==0?0:45);}
    memset(&event,0,sizeof(event));require(epoll_wait(ep,&event,1,2000)==1 && (event.events&EPOLLIN) && event.data.u64==0x123456,"epoll-readiness");
    info=consume(fd,"epoll-read");require(info.ssi_signo==SIGUSR2,"epoll-selected-signal");
    reap(child,0,"epoll-child-reaped");info=consume(blocking,"epoll-child-signal");require(info.ssi_signo==SIGCHLD,"epoll-child-signo");
    // SIGCHLD is process-directed, but tgkill remains private to its target.
    require(syscall(SYS_tgkill,getpid(),syscall(SYS_gettid),SIGUSR2)==0,"queue-leader-private");
    worker_fd=fd;require(pthread_barrier_init(&worker_barrier,NULL,2)==0,"worker-barrier");
    pthread_t worker;require(pthread_create(&worker,NULL,worker_read,NULL)==0,"worker-create");
    (void)pthread_barrier_wait(&worker_barrier);
    child=fork();require(child>=0,"worker-child-fork");if(!child)_exit(39);
    require(pthread_join(worker,NULL)==0,"worker-join");
    require(worker_info.ssi_signo==SIGCHLD && worker_info.ssi_pid==(unsigned)child &&
            worker_info.ssi_code==CLD_EXITED && worker_info.ssi_status==39,"worker-child-metadata");
    info=consume(fd,"leader-private-preserved");require(info.ssi_signo==SIGUSR2,"leader-private-signo");
    reap(child,39,"worker-child-reap");require(pthread_barrier_destroy(&worker_barrier)==0,"worker-barrier-close");
    // A CLOEXEC descriptor closes while its ordinary dup remains independent.
    child=fork();require(child>=0,"exec-fork");
    if (!child) {
        char number[32];snprintf(number,sizeof(number),"%d",fd);
        char *args[]={argv[0],"--closed",number,NULL};execv(argv[0],args);_exit(46);
    }
    reap(child,0,"cloexec");info=consume(blocking,"exec-child-signal");require(info.ssi_signo==SIGCHLD,"exec-child-signo");
    require(close(fd)==0 && close(ep)==0 && close(blocking)==0,"close-descriptors");
    require(kill(getpid(),SIGUSR1)==0,"send-after-original-close");
    info=consume(alias,"dup-survives");require(info.ssi_signo==SIGUSR1,"dup-payload");
    require(close(alias)==0,"close-last-alias");alarm(0);
    puts("{\"test\":\"signalfd\",\"passed\":true,\"scope\":\"standard signals, sender/child metadata, mask sharing, fork reader identity, poll/epoll, blocking read, CLOEXEC and close; not realtime-signal queue acceptance\"}");return 0;
}
