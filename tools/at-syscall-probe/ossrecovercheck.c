#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
static unsigned checks;
#define CK(x) do {++checks;if(!(x)){fprintf(stderr,"OSS_RECOVERY FAIL line=%d %s errno=%d\n",__LINE__,#x,errno);exit(1);}}while(0)
static uint64_t now(void){struct timespec t;CK(!clock_gettime(CLOCK_MONOTONIC,&t));return (uint64_t)t.tv_sec*1000000000+t.tv_nsec;}
int main(int argc,char** argv){
 if(argc<2||strcmp(argv[1],"--disposable-vm")){fprintf(stderr,"Usage: ossrecovercheck --disposable-vm [--blocking]\n");return 2;}
 setbuf(stdout,NULL);int nonblock=!(argc>2&&!strcmp(argv[2],"--blocking"));
 int fd=open("/dev/dsp",O_WRONLY|(nonblock?O_NONBLOCK:0));CK(fd>=0);int value=16;CK(!ioctl(fd,0xc0045005,&value)&&value==16);value=2;CK(!ioctl(fd,0xc0045006,&value)&&value==2);value=44100;CK(!ioctl(fd,0xc0045002,&value)&&value==44100);
 CK(!ioctl(fd,0x5421,&nonblock));CK(!!(fcntl(fd,F_GETFL)&O_NONBLOCK)==nonblock);CK(!ioctl(fd,0x5008,0));
 int ep=epoll_create1(0);CK(ep>=0);struct epoll_event event={.events=EPOLLOUT,.data.fd=fd};CK(!epoll_ctl(ep,EPOLL_CTL_ADD,fd,&event));
 int16_t samples[16384];for(unsigned i=0;i<sizeof(samples)/sizeof(samples[0]);++i)samples[i]=((i/200)&1)?1500:-1500;
 uint64_t began=now(),bytes=0;unsigned waits=0;int announced=0;
 while(now()-began<16000000000ULL){
  struct pollfd p={.fd=fd,.events=POLLOUT};int n=poll(&p,1,100);CK(n>=0);CK(!(p.revents&(POLLERR|POLLHUP|POLLNVAL)));if(!n){++waits;continue;}
  int count=epoll_wait(ep,&event,1,0);CK(count>=0);if(count)CK(!(event.events&(EPOLLERR|EPOLLHUP)));
  int delay=-1;CK(!ioctl(fd,0x80045017,&delay)&&delay>=0);struct{int fragments,total,size,bytes;}space;CK(!ioctl(fd,0x8010500c,&space)&&space.bytes>=0&&space.size>0);
  ssize_t wrote=write(fd,samples,nonblock?4096:sizeof(samples));if(wrote<0){CK(errno==EAGAIN);++waits;continue;}CK(wrote>0);bytes+=wrote;
  if(!announced&&bytes>=32768){puts("OSS_STREAM_READY");announced=1;}
 }
 CK(bytes>131072);CK(!close(ep)&&!close(fd));printf("OSS_RECOVERY PASS checks=%u bytes=%llu waits=%u mode=%s\n",checks,(unsigned long long)bytes,waits,nonblock?"nonblocking":"blocking");return 0;
}
