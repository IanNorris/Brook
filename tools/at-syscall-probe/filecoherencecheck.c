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
#include <time.h>
#include <unistd.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"FILECOHERENCECHECK FAIL line=%d errno=%d\n",__LINE__,errno); exit(1); } } while (0)
enum { page=4096,threads=8 };
static pthread_barrier_t barrier;
struct worker { int fd;volatile unsigned char* alias; };
static void expired(int sig) { (void)sig;_exit(124); }
static void* fault(void* opaque) {
    struct worker* worker=opaque;
    worker->alias=mmap(NULL,page,PROT_READ,MAP_SHARED,worker->fd,2*page);CHECK(worker->alias!=MAP_FAILED);
    int result=pthread_barrier_wait(&barrier);CHECK(result==0||result==PTHREAD_BARRIER_SERIAL_THREAD);
    CHECK(worker->alias[17]==(unsigned char)((2*page+17)*13+7));return NULL;
}
int main(int argc,char** argv) {
    CHECK(argc==2);
    struct sigaction action={0};action.sa_handler=expired;CHECK(sigaction(SIGALRM,&action,NULL)==0);
    timer_t timer;struct sigevent event={0};event.sigev_notify=SIGEV_SIGNAL;event.sigev_signo=SIGALRM;
    CHECK(timer_create(CLOCK_MONOTONIC,&event,&timer)==0);
    struct itimerspec deadline={0};deadline.it_value.tv_sec=60;CHECK(timer_settime(timer,0,&deadline,NULL)==0);
    int fd=open(argv[1],O_CREAT|O_EXCL|O_RDWR,0600);CHECK(fd>=0);
    unsigned char pattern[4*page];for(unsigned i=0;i<sizeof(pattern);++i)pattern[i]=(unsigned char)(i*13+7);
    CHECK(write(fd,pattern,sizeof(pattern))==(ssize_t)sizeof(pattern));
    int other=open(argv[1],O_RDWR);CHECK(other>=0);
    volatile unsigned char* shared=mmap(NULL,sizeof(pattern),PROT_READ,MAP_SHARED,fd,0);CHECK(shared!=MAP_FAILED);
    unsigned char* private=mmap(NULL,sizeof(pattern),PROT_READ|PROT_WRITE,MAP_PRIVATE,other,0);CHECK(private!=MAP_FAILED);
    CHECK(shared[17]==pattern[17]&&private[17]==pattern[17]);private[17]=0x99;
    unsigned char value=0x41;CHECK(pwrite(other,&value,1,17)==1);
    CHECK(shared[17]==0x41&&private[17]==0x99);
    pthread_t tasks[threads];struct worker workers[threads];CHECK(pthread_barrier_init(&barrier,NULL,threads)==0);
    for(unsigned i=0;i<threads;++i){workers[i].fd=i&1?fd:other;CHECK(pthread_create(&tasks[i],NULL,fault,&workers[i])==0);}
    for(unsigned i=0;i<threads;++i)CHECK(pthread_join(tasks[i],NULL)==0);
    CHECK(pthread_barrier_destroy(&barrier)==0);
    value=0x85;CHECK(pwrite(fd,&value,1,2*page+17)==1);
    for(unsigned i=0;i<threads;++i)CHECK(workers[i].alias[17]==0x85);
    CHECK(pread(fd,private+page+64,16,0)==16);
    CHECK(!memcmp(private+page+64,pattern,16));
    value=0x32;CHECK(pwrite(fd,&value,1,page+64)==1);
    CHECK(shared[page+64]==0x32&&private[page+64]==pattern[0]);
    CHECK(pwrite(other,(const void*)(shared+page+64),8,31)==8);
    unsigned char output[8];CHECK(pread(fd,output,8,31)==8&&output[0]==0x32);
    CHECK(!memcmp(output,(const void*)(shared+page+64),8));
    // Page three has never been touched by this probe. Brook validates and
    // populates the user source before entering the file write gate.
    CHECK(pwrite(other,(const void*)(shared+3*page+128),8,63)==8);
    CHECK(pread(fd,output,8,63)==8&&!memcmp(output,pattern+3*page+128,8));
    for(unsigned i=0;i<threads;++i)CHECK(munmap((void*)workers[i].alias,page)==0);
    CHECK(munmap(private,sizeof(pattern))==0&&munmap((void*)shared,sizeof(pattern))==0);
    CHECK(ftruncate(fd,0)==0&&ftruncate(fd,sizeof(pattern))==0);
    shared=mmap(NULL,page,PROT_READ,MAP_SHARED,other,0);CHECK(shared!=MAP_FAILED);
    CHECK(shared[17]==0&&shared[4095]==0);
    CHECK(close(fd)==0&&close(other)==0&&unlink(argv[1])==0);
    CHECK(shared[17]==0&&munmap((void*)shared,page)==0);
    CHECK(timer_delete(timer)==0);
    puts("FILECOHERENCECHECK PASS separate-opens/parallel-faults/io/private-cow/unmapped-resize/cold-source/unlink");
    return 0;
}
