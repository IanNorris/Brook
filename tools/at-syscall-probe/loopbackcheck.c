#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <stdint.h>
#include <time.h>
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"LOOPBACKCHECK FAIL line=%d errno=%d\n",__LINE__,errno); exit(1); } } while(0)
static void expired(int sig) { (void)sig;_exit(124); }
static struct sockaddr_in addr(unsigned host, unsigned port) { struct sockaddr_in a={0};a.sin_family=AF_INET;a.sin_addr.s_addr=htonl(host);a.sin_port=htons(port);return a; }
static void ready(int fd,short events) { struct pollfd p={fd,events,0};int n;do{n=poll(&p,1,3000);}while(n<0&&errno==EINTR);CHECK(n==1&&(p.revents&events)); }
static void send_bytes(int fd,const unsigned char *bytes,size_t count) {
 while(count) { ready(fd,POLLOUT);ssize_t n=send(fd,bytes,count,0);if(n<0&&(errno==EAGAIN||errno==EINTR))continue;CHECK(n>0);bytes+=n;count-=(size_t)n; }
}
static void receive_bytes(int fd,unsigned char *bytes,size_t count) {
 while(count) { ready(fd,POLLIN);ssize_t n=recv(fd,bytes,count,0);if(n<0&&(errno==EAGAIN||errno==EINTR))continue;CHECK(n>0);bytes+=n;count-=(size_t)n; }
}
static void tcp(unsigned bound,unsigned target,int nonblock) {
 int s=socket(AF_INET,SOCK_STREAM,0);CHECK(s>=0);struct sockaddr_in a=addr(bound,0);CHECK(!bind(s,(void*)&a,sizeof(a))&&!listen(s,2));socklen_t len=sizeof(a);CHECK(!getsockname(s,(void*)&a,&len));a.sin_addr.s_addr=htonl(target);
 pid_t child=fork();CHECK(child>=0);
 if(!child) {
  syscall(SYS_alarm,20);
  close(s);int c=socket(AF_INET,SOCK_STREAM,0);CHECK(c>=0);
  if(nonblock)CHECK(!fcntl(c,F_SETFL,O_NONBLOCK));
  int result=connect(c,(void*)&a,sizeof(a));CHECK(result==0||(nonblock&&errno==EINPROGRESS));ready(c,POLLOUT);
  int error=-1;socklen_t elen=sizeof(error);CHECK(!getsockopt(c,SOL_SOCKET,SO_ERROR,&error,&elen)&&!error);
  struct sockaddr_in local={0};len=sizeof(local);CHECK(!getsockname(c,(void*)&local,&len)&&local.sin_addr.s_addr==htonl(0x7f000001));
  unsigned char bytes[8193];for(size_t i=0;i<sizeof(bytes);++i)bytes[i]=(unsigned char)(i*17+3);
  send_bytes(c,bytes,sizeof(bytes));receive_bytes(c,bytes,sizeof(bytes));for(size_t i=0;i<sizeof(bytes);++i)CHECK(bytes[i]==(unsigned char)((i*17+3)^0x5a));
  ready(c,POLLIN);CHECK(recv(c,bytes,1,0)==0);close(c);_exit(0);
 }
 ready(s,POLLIN);struct sockaddr_in peer={0};len=sizeof(peer);int c=accept(s,(void*)&peer,&len);CHECK(c>=0&&peer.sin_addr.s_addr==htonl(0x7f000001));
 struct sockaddr_in local={0};len=sizeof(local);CHECK(!getsockname(c,(void*)&local,&len)&&local.sin_addr.s_addr==htonl(target));
 unsigned char bytes[8193];receive_bytes(c,bytes,sizeof(bytes));for(size_t i=0;i<sizeof(bytes);++i){CHECK(bytes[i]==(unsigned char)(i*17+3));bytes[i]^=0x5a;}
 send_bytes(c,bytes,sizeof(bytes));struct timespec begin,end;CHECK(!clock_gettime(CLOCK_MONOTONIC,&begin));close(c);CHECK(!clock_gettime(CLOCK_MONOTONIC,&end));
 printf("LOOPBACKCHECK close_ns=%llu\n",(unsigned long long)((uint64_t)(end.tv_sec-begin.tv_sec)*1000000000ULL+end.tv_nsec-begin.tv_nsec));close(s);int status;CHECK(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0);
}
static void udp(void) {
 int s=socket(AF_INET,SOCK_DGRAM,0),c=socket(AF_INET,SOCK_DGRAM,0);CHECK(s>=0&&c>=0);struct sockaddr_in a=addr(0x7f000002,0);CHECK(!bind(s,(void*)&a,sizeof(a)));socklen_t len=sizeof(a);CHECK(!getsockname(s,(void*)&a,&len));CHECK(!connect(c,(void*)&a,sizeof(a)));CHECK(send(c,"TEST",4,0)==4);ready(s,POLLIN);
 char bytes[4];struct sockaddr_in peer={0};len=sizeof(peer);CHECK(recvfrom(s,bytes,4,0,(void*)&peer,&len)==4&&!memcmp(bytes,"TEST",4)&&peer.sin_addr.s_addr==htonl(0x7f000001));CHECK(sendto(s,"PASS",4,0,(void*)&peer,len)==4);ready(c,POLLIN);struct sockaddr_in source={0};len=sizeof(source);CHECK(recvfrom(c,bytes,4,0,(void*)&source,&len)==4&&!memcmp(bytes,"PASS",4)&&source.sin_addr.s_addr==htonl(0x7f000002));close(c);close(s);
}
int main(void) { struct sigaction sa={0};sa.sa_handler=expired;CHECK(!sigaction(SIGALRM,&sa,NULL));syscall(SYS_alarm,20);tcp(0x7f000001,0x7f000001,0);tcp(0,0x7f000002,1);tcp(0x7f000002,0x7f000002,0);udp();syscall(SYS_alarm,0);puts("LOOPBACKCHECK PASS TCP blocking/nonblocking wildcard/127.0.0.2 multi-segment/EOF and UDP echo");return 0; }
