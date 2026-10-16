#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"FRAMEALIASCHECK FAIL line=%d errno=%d\n",__LINE__,errno); exit(1); } } while (0)
static void expired(int sig) { (void)sig; _exit(124); }
int main(void) {
    struct sigaction action={0}; action.sa_handler=expired; CHECK(sigaction(SIGALRM,&action,NULL)==0);
    timer_t timer; struct sigevent event={0}; event.sigev_notify=SIGEV_SIGNAL; event.sigev_signo=SIGALRM;
    CHECK(timer_create(CLOCK_MONOTONIC,&event,&timer)==0);
    struct itimerspec deadline={0}; deadline.it_value.tv_sec=60; CHECK(timer_settime(timer,0,&deadline,NULL)==0);
    int fd=open("/dev/fb0",O_RDWR); CHECK(fd>=0);
    enum { count=512 }; volatile uint32_t *aliases[count];
    for(unsigned i=0;i<count;++i) {
        aliases[i]=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
        CHECK(aliases[i]!=MAP_FAILED);
    }
    CHECK(close(fd)==0);
    uint32_t canary=0x42524f4f;
    *aliases[0]=canary;
    for(unsigned i=0;i<count;++i) CHECK(*aliases[i]==canary);
    for(unsigned remaining=count;remaining>1;--remaining) {
        CHECK(munmap((void*)aliases[remaining-1],4096)==0);
        CHECK(*aliases[0]==canary);
        canary+=17; *aliases[0]=canary;
        CHECK(*aliases[remaining-2]==canary);
    }
    CHECK(munmap((void*)aliases[0],4096)==0);
    CHECK(timer_delete(timer)==0);
    puts("FRAMEALIASCHECK PASS aliases=512 shared-canary/close/release");
    return 0;
}
