#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
static volatile sig_atomic_t child_pid;
static volatile sig_atomic_t signalled;
static int failures;
static void stop_child(void){if(child_pid>0){kill(child_pid,SIGKILL);while(waitpid(child_pid,NULL,0)<0&&errno==EINTR){}child_pid=0;}}
static void alarmed(int sig){(void)sig;if(child_pid>0)kill(child_pid,SIGKILL);_exit(124);}
static void caught(int sig){(void)sig;signalled=1;}
#define REQUIRE(x) do{if(!(x)){fprintf(stderr,"ACCEPTCHECK fatal line=%d errno=%d\n",__LINE__,errno);stop_child();exit(2);}}while(0)
static void check(int ok,const char *label){printf("ACCEPTCHECK %s %s\n",ok?"PASS":"FAIL",label);failures+=!ok;}
static uint64_t millis(void){struct timespec t;REQUIRE(!clock_gettime(CLOCK_MONOTONIC,&t));return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
static void delayed(void){struct timespec t={0,150000000};while(nanosleep(&t,&t)&&errno==EINTR){}}
static void one(unsigned mode){
 int listener=socket(AF_INET,SOCK_STREAM,0);REQUIRE(listener>=0);struct sockaddr_in a={.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};REQUIRE(!bind(listener,(void*)&a,sizeof(a))&&!listen(listener,2));socklen_t len=sizeof(a);REQUIRE(!getsockname(listener,(void*)&a,&len));
 if(mode==2)REQUIRE(!fcntl(listener,F_SETFL,O_NONBLOCK));
 int ep=-1;if(mode==3){ep=epoll_create1(EPOLL_CLOEXEC);REQUIRE(ep>=0);struct epoll_event e={.events=EPOLLIN,.data.u64=123};REQUIRE(!epoll_ctl(ep,EPOLL_CTL_ADD,listener,&e));}
 int barrier[2];REQUIRE(!pipe(barrier));pid_t p=fork();REQUIRE(p>=0);child_pid=p;
 if(!p){child_pid=0;syscall(SYS_alarm,10);close(listener);close(barrier[1]);char go;REQUIRE(read(barrier[0],&go,1)==1);close(barrier[0]);delayed();if(mode==4){REQUIRE(!kill(getppid(),SIGUSR1));delayed();}int c=socket(AF_INET,SOCK_STREAM,0);REQUIRE(c>=0&&!connect(c,(void*)&a,sizeof(a)));REQUIRE(send(c,"A",1,0)==1);close(c);_exit(0);}
 close(barrier[0]);REQUIRE(write(barrier[1],"G",1)==1);close(barrier[1]);uint64_t start=millis();
 if(mode==2){errno=0;int c=accept(listener,NULL,NULL);check(c<0&&(errno==EAGAIN||errno==EWOULDBLOCK),"nonblocking empty accept returns EAGAIN");if(c>=0)close(c);struct pollfd f={listener,POLLIN,0};check(poll(&f,1,3000)==1&&(f.revents&POLLIN),"poll registered before delayed connect becomes readable");}
 if(mode==3){struct epoll_event e={0};check(epoll_wait(ep,&e,1,3000)==1&&(e.events&EPOLLIN)&&e.data.u64==123,"epoll registered before delayed connect becomes readable");}
 if(mode==4){errno=0;int c=accept(listener,NULL,NULL);check(c<0&&errno==EINTR&&signalled,"blocking accept is interrupted by handled signal");if(c>=0)close(c);}
 errno=0;int c=mode==1?accept4(listener,NULL,NULL,SOCK_NONBLOCK|SOCK_CLOEXEC):accept(listener,NULL,NULL);int error=errno;uint64_t elapsed=millis()-start;
 check(c>=0,"delayed client is accepted");if(mode==0||mode==1)check(c>=0&&elapsed>=100,"blocking wait reaches delayed connection");
 if(c>=0){check(!!(fcntl(c,F_GETFL)&O_NONBLOCK)==(mode==1),"accepted O_NONBLOCK comes from accept4 flags only");check(!!(fcntl(c,F_GETFD)&FD_CLOEXEC)==(mode==1),"accepted FD_CLOEXEC follows accept4 flags");struct pollfd f={c,POLLIN,0};REQUIRE(poll(&f,1,3000)==1);char b;check(recv(c,&b,1,0)==1&&b=='A',"accepted connection receives known data");close(c);}else printf("ACCEPTCHECK accept_errno=%d mode=%u\n",error,mode);
 close(listener);if(ep>=0)close(ep);if(c<0)stop_child();else{int status;REQUIRE(waitpid(p,&status,0)==p&&WIFEXITED(status)&&WEXITSTATUS(status)==0);child_pid=0;}
}
struct close_wait { int fd,result,error;atomic_int entered; };
static void *accept_worker(void *arg){struct close_wait *w=arg;atomic_store_explicit(&w->entered,1,memory_order_release);w->result=accept(w->fd,NULL,NULL);w->error=errno;return NULL;}
static int connect_local(struct sockaddr_in *a){int c=socket(AF_INET,SOCK_STREAM,0);REQUIRE(c>=0&&!connect(c,(void*)a,sizeof(*a)));REQUIRE(send(c,"L",1,0)==1);return c;}
static void close_lifetime(void){
 int s=socket(AF_INET,SOCK_STREAM,0);REQUIRE(s>=0);struct sockaddr_in a={.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};REQUIRE(!bind(s,(void*)&a,sizeof(a))&&!listen(s,2));socklen_t len=sizeof(a);REQUIRE(!getsockname(s,(void*)&a,&len));int dupfd=dup(s);REQUIRE(dupfd>=0);
 struct close_wait w={.fd=s,.result=-1,.error=0,.entered=0};pthread_t worker;REQUIRE(!pthread_create(&worker,NULL,accept_worker,&w));while(!atomic_load_explicit(&w.entered,memory_order_acquire))usleep(1000);delayed();REQUIRE(!close(s));int c=connect_local(&a);REQUIRE(!pthread_join(worker,NULL));
 check(w.result>=0||(w.result<0&&w.error==EBADF),"accept concurrent close finishes or reports closed descriptor");
 if(w.result>=0)close(w.result);else{struct pollfd f={dupfd,POLLIN,0};REQUIRE(poll(&f,1,3000)==1);int accepted=accept(dupfd,NULL,NULL);REQUIRE(accepted>=0);close(accepted);}close(c);
 c=connect_local(&a);struct pollfd f={dupfd,POLLIN,0};REQUIRE(poll(&f,1,3000)==1);int accepted=accept(dupfd,NULL,NULL);check(accepted>=0,"duplicate listener remains usable after closing accepting descriptor");if(accepted>=0)close(accepted);close(c);close(dupfd);
}
int main(void){setbuf(stdout,NULL);struct sigaction a={.sa_handler=alarmed};REQUIRE(!sigaction(SIGALRM,&a,NULL));a.sa_handler=caught;REQUIRE(!sigaction(SIGUSR1,&a,NULL));syscall(SYS_alarm,25);for(unsigned mode=0;mode<5;++mode)one(mode);close_lifetime();syscall(SYS_alarm,0);printf("ACCEPTCHECK %s failures=%d\n",failures?"FAIL":"PASS",failures);return failures!=0;}
