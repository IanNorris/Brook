#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHECK(x) do { if(!(x)) { fprintf(stderr,"NAMEDSHMCHECK FAIL line=%d errno=%d\n",__LINE__,errno);exit(1); } } while(0)
enum { names=96, workers=8 };
static char paths[names][160];
static int owned[names],descriptors[names];
static pthread_barrier_t barrier;
static int racers[workers],race_errors[workers];
static volatile sig_atomic_t child;
static void cleanup(void) {
    if(child>0) { kill(child,SIGKILL);while(waitpid(child,NULL,0)<0&&errno==EINTR){} }
    for(unsigned i=0;i<names;++i) {
        if(owned[i])unlink(paths[i]);
        if(descriptors[i]>=0)close(descriptors[i]);
    }
}
static void expired(int sig) { (void)sig;if(child>0)kill(child,SIGKILL);_exit(124); }
static void* race(void* opaque) {
    uintptr_t i=(uintptr_t)opaque;
    int r=pthread_barrier_wait(&barrier);CHECK(r==0||r==PTHREAD_BARRIER_SERIAL_THREAD);
    racers[i]=open(paths[1],O_CREAT|O_EXCL|O_RDWR,0600);race_errors[i]=errno;
    return NULL;
}
int main(void) {
    for(unsigned i=0;i<names;++i)descriptors[i]=-1;
    CHECK(atexit(cleanup)==0);
    struct sigaction action={0};action.sa_handler=expired;CHECK(!sigaction(SIGALRM,&action,NULL));
    timer_t timer;struct sigevent event={0};event.sigev_notify=SIGEV_SIGNAL;event.sigev_signo=SIGALRM;
    CHECK(!timer_create(CLOCK_MONOTONIC,&event,&timer));
    struct itimerspec deadline={0};deadline.it_value.tv_sec=60;CHECK(!timer_settime(timer,0,&deadline,NULL));
    struct timespec stamp;CHECK(!clock_gettime(CLOCK_MONOTONIC,&stamp));
    for(unsigned i=0;i<names;++i) {
        snprintf(paths[i],sizeof(paths[i]),"/dev/shm/brook-named-%ld-%ld-%ld-%u",(long)getpid(),stamp.tv_sec,stamp.tv_nsec,i);
        descriptors[i]=open(paths[i],O_CREAT|O_EXCL|O_RDWR,0600);CHECK(descriptors[i]>=0);owned[i]=1;
        CHECK(!ftruncate(descriptors[i],4096));
        int other=open(paths[i],O_RDONLY|O_CLOEXEC);CHECK(other>=0);
        struct stat a,b;CHECK(!fstat(descriptors[i],&a)&&!fstat(other,&b));
        CHECK(a.st_ino&&a.st_ino==b.st_ino&&a.st_dev==b.st_dev&&a.st_size==4096);
        CHECK(fcntl(other,F_GETFD)&FD_CLOEXEC);
        errno=0;int duplicate=open(paths[i],O_CREAT|O_EXCL|O_RDWR,0600);CHECK(duplicate<0&&errno==EEXIST);
        CHECK(!close(other));
    }
    volatile unsigned char* old=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED,descriptors[0],0);CHECK(old!=MAP_FAILED);
    int reader=open(paths[0],O_RDONLY);CHECK(reader>=0);
    volatile unsigned char* alias=mmap(NULL,4096,PROT_READ,MAP_SHARED,reader,0);CHECK(alias!=MAP_FAILED);
    old[17]=0x52;CHECK(alias[17]==0x52);
    unsigned char value;CHECK(pread(reader,&value,1,17)==1&&value==0x52);
    struct stat original,recreated;CHECK(!fstat(reader,&original));
    CHECK(!unlink(paths[0]));owned[0]=0;
    errno=0;CHECK(open(paths[0],O_RDONLY)<0&&errno==ENOENT);
    errno=0;CHECK(unlink(paths[0])<0&&errno==ENOENT);
    int fresh=open(paths[0],O_CREAT|O_EXCL|O_RDWR,0600);CHECK(fresh>=0);owned[0]=1;
    CHECK(!ftruncate(fresh,4096)&&!fstat(fresh,&recreated)&&recreated.st_ino!=original.st_ino);
    volatile unsigned char* replacement=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED,fresh,0);CHECK(replacement!=MAP_FAILED);
    CHECK(!replacement[17]&&old[17]==0x52&&alias[17]==0x52);
    replacement[17]=0x63;CHECK(old[17]==0x52);
    CHECK(!close(reader)&&!close(descriptors[0]));descriptors[0]=-1;
    pid_t pid=fork();CHECK(pid>=0);child=pid;
    if(!pid) { child=0;old[31]=0x74;_exit(alias[17]==0x52&&replacement[17]==0x63?0:1); }
    int status;CHECK(waitpid(pid,&status,0)==pid);child=0;
    CHECK(WIFEXITED(status)&&!WEXITSTATUS(status)&&old[31]==0x74);
    CHECK(!munmap((void*)old,4096)&&!munmap((void*)alias,4096));
    CHECK(!unlink(paths[0])&&!close(fresh)&&!munmap((void*)replacement,4096));owned[0]=0;
    for(unsigned i=1;i<names;++i) { CHECK(!unlink(paths[i]));owned[i]=0;CHECK(!close(descriptors[i]));descriptors[i]=-1; }
    // Reclaim names immediately, even while a previous incarnation's mappings
    // existed; the concurrent O_EXCL race must publish exactly one new object.
    CHECK(!pthread_barrier_init(&barrier,NULL,workers));pthread_t threads[workers];
    for(uintptr_t i=0;i<workers;++i)CHECK(!pthread_create(&threads[i],NULL,race,(void*)i));
    unsigned winners=0;
    for(unsigned i=0;i<workers;++i) {
        CHECK(!pthread_join(threads[i],NULL));
        if(racers[i]>=0) { ++winners;owned[1]=1;CHECK(!close(racers[i])); }
        else CHECK(race_errors[i]==EEXIST);
    }
    CHECK(winners==1&&!unlink(paths[1]));owned[1]=0;
    CHECK(!pthread_barrier_destroy(&barrier));
    CHECK(!timer_delete(timer));
    puts("NAMEDSHMCHECK PASS 96-active/named-open/identity/cloexec/unlink/recreate/mapped-lifetime/fork/exclusive-race");
    return 0;
}
