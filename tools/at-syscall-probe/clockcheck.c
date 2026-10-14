#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
static long long ns(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (long long)t.tv_sec*1000000000+t.tv_nsec;}
static struct timespec future(long long delay){long long n=ns()+delay;return (struct timespec){n/1000000000,n%1000000000};}
int main(void){
 pid_t child=fork(); if(child<0)return 1;
 if(child==0){
  long long before=ns();struct timespec t=future(20000000);
  if(clock_nanosleep(CLOCK_MONOTONIC,TIMER_ABSTIME,&t,NULL)!=0)_exit(2);
  long long elapsed=ns()-before;
  if(elapsed<15000000||elapsed>500000000)_exit(3);
  pthread_condattr_t attr;pthread_condattr_init(&attr);pthread_condattr_setclock(&attr,CLOCK_MONOTONIC);
  pthread_cond_t cv;pthread_cond_init(&cv,&attr);pthread_mutex_t m=PTHREAD_MUTEX_INITIALIZER;pthread_mutex_lock(&m);
  before=ns();t=future(30000000);int r=pthread_cond_timedwait(&cv,&m,&t);elapsed=ns()-before;
  if(r!=ETIMEDOUT||elapsed<25000000||elapsed>500000000)_exit(4);
  _exit(0);
 }
 int status=0;int done=0;
 for(int i=0;i<100;i++){if(waitpid(child,&status,WNOHANG)==child){done=1;break;}usleep(10000);}
 if(!done){kill(child,SIGKILL);waitpid(child,&status,0);}
 int good=done&&WIFEXITED(status)&&WEXITSTATUS(status)==0;
 printf("CLOCKCHECK %s: absolute sleep and monotonic condition timeout; watchdog=%s status=%d\n",good?"PASS":"FAIL",done?"not needed":"expired",status);
 return !good;
}
