/* Run under an external timeout. Linux is the oracle; Brook's result can be
 * compared without depending on libc's timer_t or a particular signal handler. */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s (errno=%d)\n",__LINE__,#x,errno); return 1; } } while (0)
static struct itimerval timer_us(long initial,long interval) {
    struct itimerval t={0};t.it_value.tv_sec=initial/1000000;t.it_value.tv_usec=initial%1000000;
    t.it_interval.tv_sec=interval/1000000;t.it_interval.tv_usec=interval%1000000;return t;
}
static void *cancel_other_thread(void *result) {
    struct itimerval zero={0},old={0};
    *(int *)result=setitimer(ITIMER_REAL,&zero,&old)==0 && old.it_value.tv_sec>0;
    return NULL;
}
int main(int argc,char **argv) {
    sigset_t mask;sigemptyset(&mask);sigaddset(&mask,SIGALRM);
    CHECK(!pthread_sigmask(SIG_BLOCK,&mask,NULL));
    if(argc==2 && !strcmp(argv[1],"--exec-child")) {
        struct itimerval current;CHECK(!getitimer(ITIMER_REAL,&current));
        CHECK(current.it_value.tv_sec>0);return 0;
    }
    struct itimerval value=timer_us(3000000,0),old={0},current={0};
    CHECK(!setitimer(ITIMER_REAL,&value,&old));CHECK(!getitimer(ITIMER_REAL,&current));
    CHECK(current.it_value.tv_sec>0);
    pthread_t thread;int shared=0;CHECK(!pthread_create(&thread,NULL,cancel_other_thread,&shared));
    CHECK(!pthread_join(thread,NULL) && shared);CHECK(!getitimer(ITIMER_REAL,&current));
    CHECK(!current.it_value.tv_sec && !current.it_value.tv_usec);
    value=timer_us(3000000,0);CHECK(!setitimer(ITIMER_REAL,&value,NULL));
    pid_t child=fork();CHECK(child>=0);
    if(!child) { struct itimerval inherited;_exit(getitimer(ITIMER_REAL,&inherited) || inherited.it_value.tv_sec || inherited.it_value.tv_usec); }
    int status;CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
    child=fork();CHECK(child>=0);
    if(!child) { value=timer_us(3000000,0);if(setitimer(ITIMER_REAL,&value,NULL))_exit(2);execl(argv[0],argv[0],"--exec-child",(char *)NULL);_exit(3); }
    CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
    CHECK(alarm(0)>=1);CHECK(!getitimer(ITIMER_REAL,&current));
    CHECK(!current.it_value.tv_sec && !current.it_value.tv_usec);
    value=timer_us(20000,20000);CHECK(!setitimer(ITIMER_REAL,&value,NULL));
    struct timespec pause={0,120000000};while(nanosleep(&pause,&pause) && errno==EINTR) {}
    CHECK(!getitimer(ITIMER_REAL,&current));CHECK(!current.it_value.tv_sec && !current.it_value.tv_usec);
    CHECK(current.it_interval.tv_usec==20000);
    siginfo_t info={0};struct timespec timeout={1,0};CHECK(sigtimedwait(&mask,&info,&timeout)==SIGALRM);
    CHECK(info.si_code==SI_KERNEL);CHECK(!getitimer(ITIMER_REAL,&current));
    CHECK(current.it_value.tv_sec || current.it_value.tv_usec);
    value=timer_us(0,0);CHECK(!setitimer(ITIMER_REAL,&value,NULL));
    puts("ITIMER_REAL_PASS shared-thread/fork/exec/alarm/masked-periodic");return 0;
}
