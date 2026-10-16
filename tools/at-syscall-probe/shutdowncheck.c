#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static unsigned checks;
static volatile sig_atomic_t pipe_signals;
static void got_pipe(int signal_number) { (void)signal_number; ++pipe_signals; }
#define CHECK(x) do { ++checks; if(!(x)){fprintf(stderr,"SHUTDOWNCHECK FAIL line=%d errno=%d\n",__LINE__,errno);exit(1);} } while(0)
static void pair(int s[2]) { CHECK(socketpair(AF_UNIX,SOCK_STREAM,0,s)==0); }
static void nap(void) { struct timespec t={0,50000000}; nanosleep(&t,0); }
static void readiness(int fd,int required,int forbidden) {
    struct pollfd p={fd,POLLIN|POLLOUT|POLLRDHUP,0};CHECK(poll(&p,1,0)==1);
    CHECK((p.revents&required)==required && !(p.revents&forbidden));
    int ep=epoll_create1(0);CHECK(ep>=0);
    struct epoll_event e={.events=EPOLLIN|EPOLLOUT|EPOLLRDHUP,.data.u64=0x1234};
    CHECK(epoll_ctl(ep,EPOLL_CTL_ADD,fd,&e)==0);CHECK(epoll_wait(ep,&e,1,0)==1);
    CHECK((e.events&(unsigned)required)==(unsigned)required && !(e.events&(unsigned)forbidden) && e.data.u64==0x1234);close(ep);
}
static void basic(void) {
    int s[2];char b[8];
    errno=0;CHECK(shutdown(-1,SHUT_RDWR)==-1 && errno==EBADF);
    int n=open("/dev/null",O_RDWR);CHECK(n>=0);errno=0;CHECK(shutdown(n,SHUT_RDWR)==-1 && errno==ENOTSOCK);close(n);
    pair(s);errno=0;CHECK(shutdown(s[0],3)==-1 && errno==EINVAL);
    CHECK(write(s[0],"abc",3)==3);CHECK(shutdown(s[0],SHUT_WR)==0);CHECK(shutdown(s[0],SHUT_WR)==0);
    readiness(s[1],POLLIN|POLLRDHUP,POLLHUP);CHECK(read(s[1],b,8)==3 && !memcmp(b,"abc",3));CHECK(read(s[1],b,8)==0);
    errno=0;CHECK(send(s[0],"x",1,MSG_NOSIGNAL)==-1 && errno==EPIPE);
    errno=0;CHECK(send(s[0],"",0,MSG_NOSIGNAL)==-1 && errno==EPIPE);
    CHECK(signal(SIGPIPE,got_pipe)!=SIG_ERR);
    errno=0;CHECK(write(s[0],"x",1)==-1 && errno==EPIPE && pipe_signals==1);
    errno=0;CHECK(send(s[0],"x",1,MSG_NOSIGNAL)==-1 && errno==EPIPE && pipe_signals==1);
    errno=0;CHECK(send(s[0],"x",1,0)==-1 && errno==EPIPE && pipe_signals==2);
    CHECK(signal(SIGPIPE,SIG_IGN)!=SIG_ERR);
    CHECK(write(s[1],"r",1)==1);CHECK(read(s[0],b,8)==1 && b[0]=='r');close(s[0]);close(s[1]);
    pair(s);CHECK(write(s[1],"abc",3)==3);int alias=dup(s[0]);CHECK(alias>=0);
    CHECK(shutdown(alias,SHUT_RD)==0);close(alias);
    readiness(s[0],POLLIN|POLLRDHUP,POLLHUP);
    CHECK(read(s[0],b,8)==3 && !memcmp(b,"abc",3));CHECK(recv(s[0],b,8,0)==0);
    errno=0;CHECK(send(s[1],"x",1,MSG_NOSIGNAL)==-1 && errno==EPIPE);
    CHECK(write(s[0],"r",1)==1);CHECK(read(s[1],b,8)==1 && b[0]=='r');
    CHECK(shutdown(s[0],SHUT_RDWR)==0);readiness(s[0],POLLIN|POLLRDHUP|POLLHUP,0);
    close(s[0]);close(s[1]);
}
struct Worker { int fd,kind,ep; atomic_int* started; int result,error,events; };
static void* worker(void* arg) {
    struct Worker* w=arg;char byte;
    atomic_fetch_add(w->started,1);
    if(w->kind==0)w->result=read(w->fd,&byte,1);
    else if(w->kind==1)w->result=send(w->fd,"x",1,MSG_NOSIGNAL);
    else if(w->kind==2){struct pollfd p={w->fd,POLLIN|POLLRDHUP,0};w->result=poll(&p,1,2000);w->events=p.revents;}
    else {struct epoll_event e;w->result=epoll_wait(w->ep,&e,1,2000);w->events=e.events;}
    w->error=errno;return 0;
}
static void blocked(int how,int peer,int writing,int close_active) {
    int s[2];pair(s);int control=dup(s[0]);CHECK(control>=0);
    if(writing){char buf[4096]={0};CHECK(fcntl(s[0],F_SETFL,O_NONBLOCK)==0);while(send(s[0],buf,sizeof(buf),MSG_NOSIGNAL)>0){}CHECK(errno==EAGAIN);CHECK(fcntl(s[0],F_SETFL,0)==0);}
    atomic_int started=0;struct Worker w[6];pthread_t t[6];int count=writing?4:6;
    for(int i=0;i<count;i++){
        w[i]=(struct Worker){.fd=s[0],.kind=writing?1:(i<4?0:i-2),.ep=-1,.started=&started};
        if(w[i].kind==3){w[i].ep=epoll_create1(0);CHECK(w[i].ep>=0);struct epoll_event e={.events=EPOLLIN|EPOLLRDHUP};CHECK(epoll_ctl(w[i].ep,EPOLL_CTL_ADD,s[0],&e)==0);}
        CHECK(pthread_create(&t[i],0,worker,&w[i])==0);
    }
    while(atomic_load(&started)<count)nap();nap();
    if(close_active)CHECK(close(s[0])==0);
    CHECK(shutdown(peer?s[1]:control,how)==0);
    for(int i=0;i<count;i++){
        CHECK(pthread_join(t[i],0)==0);
        if(writing)CHECK(w[i].result==-1 && w[i].error==EPIPE);
        else if(w[i].kind==0)CHECK(w[i].result==0);
        else CHECK(w[i].result==1 && (w[i].events&(POLLIN|POLLRDHUP))==(POLLIN|POLLRDHUP));
        if(w[i].ep>=0)close(w[i].ep);
    }
    close(control);if(!close_active)close(s[0]);close(s[1]);
}
static void rights(void) {
    int s[2];pair(s);int fd=memfd_create("shutdown-right",MFD_CLOEXEC);CHECK(fd>=0);CHECK(write(fd,"m",1)==1);CHECK(lseek(fd,0,SEEK_SET)==0);
    char byte='x';struct iovec iov={&byte,1};union {struct cmsghdr a;char b[CMSG_SPACE(sizeof(int))];} c={0};
    struct msghdr m={.msg_iov=&iov,.msg_iovlen=1,.msg_control=c.b,.msg_controllen=sizeof(c)};
    struct cmsghdr* h=CMSG_FIRSTHDR(&m);h->cmsg_level=SOL_SOCKET;h->cmsg_type=SCM_RIGHTS;h->cmsg_len=CMSG_LEN(sizeof(int));memcpy(CMSG_DATA(h),&fd,sizeof(fd));
    CHECK(sendmsg(s[0],&m,MSG_NOSIGNAL)==1);close(fd);CHECK(shutdown(s[0],SHUT_WR)==0);CHECK(shutdown(s[1],SHUT_RD)==0);
    memset(&c,0,sizeof(c));CHECK(recvmsg(s[1],&m,MSG_CMSG_CLOEXEC)==1);h=CMSG_FIRSTHDR(&m);CHECK(h && h->cmsg_type==SCM_RIGHTS);memcpy(&fd,CMSG_DATA(h),sizeof(fd));CHECK(read(fd,&byte,1)==1 && byte=='m');close(fd);
    CHECK(read(s[1],&byte,1)==0);close(s[0]);close(s[1]);
}
int main(void) {
    setvbuf(stdout,0,_IONBF,0);syscall(SYS_alarm,60);signal(SIGPIPE,SIG_IGN);
    basic();rights();
    for(int round=0;round<8;round++) {
        blocked(SHUT_RDWR,0,0,0); // local cancellation: four readers plus poll/epoll
        blocked(SHUT_WR,1,0,0);   // peer FIN
        blocked(SHUT_WR,0,1,1);   // full write queue, close the pinned fd, shutdown alias
        blocked(SHUT_RD,1,1,0);   // peer stops reading
    }
    printf("SHUTDOWNCHECK PASS checks=%u rounds=8\n",checks);return 0;
}
