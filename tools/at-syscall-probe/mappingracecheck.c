#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Requires a BROOK_MAPPING_TEST_HOOKS kernel. The two barriers run in the
 * actual syscall.cpp resolver: after retaining a range, and after preparing
 * its pages, before revalidation. A timeout, missing hook or unexpected signal
 * is a failure, never an accepted timing-only outcome. */
#define REQUIRE(x) do { if (!(x)) { fprintf(stderr,"MAPPINGRACECHECK FAIL line=%d errno=%d\n",__LINE__,errno); exit(1); } } while (0)
static long control(long action,void *address,long length,long phase,long count) {
    return syscall(0xB30,action,address,length,phase,count,0);
}
static uint64_t millis(void) {
    struct timespec time; REQUIRE(clock_gettime(CLOCK_MONOTONIC,&time)==0);
    return (uint64_t)time.tv_sec*1000+(uint64_t)time.tv_nsec/1000000;
}
static void await_faults(unsigned count) {
    uint64_t start=millis();
    for (;;) {
        long state=control(1,NULL,0,0,0);REQUIRE(state>=0 && !((uint64_t)state>>32));
        if ((state&65535)==count && ((state>>16)&65535)==count) return;
        REQUIRE(millis()-start<5000);usleep(1000);
    }
}
static void release_faults(void) { REQUIRE(control(2,NULL,0,0,0)==0); }
static volatile unsigned char *mapping;
static unsigned char observed[2];
static size_t offsets[2];
static void *reader(void *argument) {
    size_t index=(uintptr_t)argument;observed[index]=mapping[offsets[index]];return NULL;
}
static void setup(int fd,int memfd,unsigned phase,unsigned count) {
    mapping=mmap(NULL,8192,PROT_READ|PROT_WRITE,memfd?MAP_SHARED:MAP_PRIVATE,fd,0);
    REQUIRE(mapping!=MAP_FAILED);
    REQUIRE(control(0,(void *)mapping,8192,phase,count)==0);
}
static void competing(int fd,int memfd,unsigned phase,int adjacent) {
    setup(fd,memfd,phase,2);offsets[0]=0;offsets[1]=adjacent?4096:0;
    pthread_t threads[2];
    for (uintptr_t i=0;i<2;++i) REQUIRE(pthread_create(&threads[i],NULL,reader,(void *)i)==0);
    await_faults(2);release_faults();
    for (unsigned i=0;i<2;++i) { REQUIRE(pthread_join(threads[i],NULL)==0);REQUIRE(observed[i]==(offsets[i]?'B':'A')); }
    REQUIRE(munmap((void *)mapping,8192)==0);
}
static void replaced(int fd,int memfd,unsigned phase) {
    setup(fd,memfd,phase,1);offsets[0]=0;
    pthread_t thread;REQUIRE(pthread_create(&thread,NULL,reader,NULL)==0);
    await_faults(1);
    REQUIRE(mmap((void *)mapping,8192,PROT_READ|PROT_WRITE,MAP_FIXED|MAP_PRIVATE|MAP_ANONYMOUS,-1,0)==(void *)mapping);
    mapping[0]='Z';mapping[4096]='Y';release_faults();
    REQUIRE(pthread_join(thread,NULL)==0 && observed[0]=='Z');
    REQUIRE(mapping[4096]=='Y');REQUIRE(munmap((void *)mapping,8192)==0);
}
static int remove_mapping;
static void *mutator(void *unused) {
    (void)unused;await_faults(1);
    if (remove_mapping) REQUIRE(munmap((void *)mapping,8192)==0);
    else REQUIRE(mprotect((void *)mapping,8192,PROT_READ)==0);
    release_faults();return NULL;
}
static void denied(int fd,int memfd,unsigned phase,int remove) {
    pid_t child=fork();REQUIRE(child>=0);
    if (!child) {
        setup(fd,memfd,phase,1);remove_mapping=remove;
        pthread_t thread;REQUIRE(pthread_create(&thread,NULL,mutator,NULL)==0);
        if (remove) { volatile unsigned char byte=mapping[0];(void)byte; }
        else mapping[0]='X';
        _exit(9); /* A stale successful fault would reach this line. */
    }
    int status;REQUIRE(waitpid(child,&status,0)==child);
    REQUIRE(WIFSIGNALED(status) && WTERMSIG(status)==SIGSEGV);
    long state=control(1,NULL,0,0,0);REQUIRE(state==1);
}
static void cancelled(int fd,unsigned phase) {
    int ready[2];REQUIRE(pipe(ready)==0);
    pid_t child=fork();REQUIRE(child>=0);
    if (!child) {
        close(ready[0]);setup(fd,0,phase,1);
        if (write(ready[1],"R",1)!=1) _exit(8);
        volatile unsigned char byte=mapping[0];(void)byte;_exit(9);
    }
    close(ready[1]);char byte;REQUIRE(read(ready[0],&byte,1)==1 && byte=='R');close(ready[0]);
    await_faults(1);errno=0;REQUIRE(ftruncate(fd,4096)==-1 && errno==EBUSY);
    REQUIRE(kill(child,SIGKILL)==0);release_faults();
    int status;REQUIRE(waitpid(child,&status,0)==child);
    REQUIRE(WIFSIGNALED(status) && WTERMSIG(status)==SIGKILL);
    REQUIRE(control(1,NULL,0,0,0)==1);
    REQUIRE(ftruncate(fd,4096)==0 && ftruncate(fd,8192)==0);
    REQUIRE(pwrite(fd,"B",1,4096)==1);
}
int main(int argc,char **argv) {
    if (argc!=2) { fprintf(stderr,"usage: mappingracecheck /data/RACE.TST\n");return 2; }
    alarm(90);
    /* Fail clearly when accidentally run on a production kernel. */
    errno=0;REQUIRE(control(1,NULL,0,0,0)>=0);
    int file=open(argv[1],O_CREAT|O_TRUNC|O_RDWR,0600);REQUIRE(file>=0);
    int memory=memfd_create("mapping-race",0);REQUIRE(memory>=0);
    int descriptors[2]={file,memory};
    for (unsigned i=0;i<2;++i) {
        REQUIRE(ftruncate(descriptors[i],8192)==0);
        REQUIRE(pwrite(descriptors[i],"A",1,0)==1 && pwrite(descriptors[i],"B",1,4096)==1);
    }
    REQUIRE(fsync(file)==0);
    for (unsigned phase=1;phase<=2;++phase) {
        for (unsigned memfd=0;memfd<2;++memfd) {
            competing(descriptors[memfd],memfd,phase,0);
            competing(descriptors[memfd],memfd,phase,1);
            replaced(descriptors[memfd],memfd,phase);
            denied(descriptors[memfd],memfd,phase,0);
            denied(descriptors[memfd],memfd,phase,1);
            printf("MAPPINGRACECHECK phase=%u backing=%s PASS same/adjacent/fixed/protect/unmap\n",phase,memfd?"memfd":"file");fflush(stdout);
        }
        cancelled(file,phase);
        printf("MAPPINGRACECHECK phase=%u PASS cancelled-fault/wait/file-pin-release\n",phase);fflush(stdout);
    }
    close(file);close(memory);REQUIRE(unlink(argv[1])==0);
    puts("MAPPINGRACECHECK PASS phases=2 backings=2 cases=22");return 0;
}
