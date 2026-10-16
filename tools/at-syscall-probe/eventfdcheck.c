#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"EVENTFDCHECK FAIL line=%d errno=%d\n",__LINE__,errno);exit(1); } } while(0)
enum { writers=4, increments=2000 };
static int shared;
static atomic_uint done;
static pthread_barrier_t barrier;
static volatile sig_atomic_t child;
static void expired(int sig) { (void)sig;if(child>0)kill(child,SIGKILL);_exit(124); }
static void* produce(void* unused) {
    (void)unused;int r=pthread_barrier_wait(&barrier);CHECK(r==0||r==PTHREAD_BARRIER_SERIAL_THREAD);
    uint64_t one=1;
    for(unsigned i=0;i<increments;++i)CHECK(write(shared,&one,8)==8);
    atomic_fetch_add_explicit(&done,1,memory_order_release);return NULL;
}
int main(int argc,char** argv) {
    if(argc==3&&!strcmp(argv[1],"--closed")) {
        errno=0;return !(fcntl(atoi(argv[2]),F_GETFD)==-1&&errno==EBADF);
    }
    CHECK(argc==1);
    struct sigaction action={0};action.sa_handler=expired;CHECK(!sigaction(SIGALRM,&action,NULL));
    timer_t timer;struct sigevent event={0};event.sigev_notify=SIGEV_SIGNAL;event.sigev_signo=SIGALRM;
    CHECK(!timer_create(CLOCK_MONOTONIC,&event,&timer));
    struct itimerspec deadline={0};deadline.it_value.tv_sec=60;CHECK(!timer_settime(timer,0,&deadline,NULL));
    for(unsigned i=0;i<96;++i) {
        errno=0;CHECK(syscall(SYS_eventfd2,0,~0u)==-1&&errno==EINVAL);
    }
    int fd=eventfd(3,EFD_SEMAPHORE|EFD_NONBLOCK|EFD_CLOEXEC);CHECK(fd>=0);
    CHECK(fcntl(fd,F_GETFD)&FD_CLOEXEC);
    CHECK((fcntl(fd,F_GETFL)&(O_ACCMODE|O_NONBLOCK))==(O_RDWR|O_NONBLOCK));
    uint64_t value=0;
    for(unsigned i=0;i<3;++i)CHECK(read(fd,&value,8)==8&&value==1);
    errno=0;CHECK(read(fd,&value,8)==-1&&errno==EAGAIN);
    pid_t pid=fork();CHECK(pid>=0);child=pid;
    if(!pid) {
        char number[24];snprintf(number,sizeof(number),"%d",fd);
        execl("/proc/self/exe","eventfdcheck","--closed",number,(char*)NULL);_exit(2);
    }
    int status;CHECK(waitpid(pid,&status,0)==pid);child=0;
    CHECK(WIFEXITED(status)&&!WEXITSTATUS(status));CHECK(!close(fd));
    shared=eventfd(0,EFD_NONBLOCK);CHECK(shared>=0);
    CHECK(!pthread_barrier_init(&barrier,NULL,writers+1));pthread_t threads[writers];
    for(unsigned i=0;i<writers;++i)CHECK(!pthread_create(&threads[i],NULL,produce,NULL));
    int r=pthread_barrier_wait(&barrier);CHECK(r==0||r==PTHREAD_BARRIER_SERIAL_THREAD);
    uint64_t total=0;
    while(total<writers*increments) {
        ssize_t bytes=read(shared,&value,8);
        if(bytes==8) { CHECK(value>0&&value<=writers*increments-total);total+=value; }
        else {
            CHECK(bytes==-1&&errno==EAGAIN);
            if(atomic_load_explicit(&done,memory_order_acquire)==writers) {
                // Re-read after observing every producer's completed write.
                bytes=read(shared,&value,8);
                CHECK(bytes==8&&value>0&&value<=writers*increments-total);total+=value;
            } else usleep(1000);
        }
    }
    for(unsigned i=0;i<writers;++i)CHECK(!pthread_join(threads[i],NULL));
    errno=0;CHECK(read(shared,&value,8)==-1&&errno==EAGAIN);
    CHECK(!close(shared)&&!pthread_barrier_destroy(&barrier)&&!timer_delete(timer));
    puts("EVENTFDCHECK PASS invalid-flags/nonblock/semaphore/descriptor-flags/exec-close/concurrent-counter");return 0;
}
