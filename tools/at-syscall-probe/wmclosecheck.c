#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"WMCLOSECHECK FAIL line=%d errno=%d\n",__LINE__,errno); exit(1); } } while (0)
struct window_result { uint32_t id,stride;uint64_t pixels; };
struct event { uint8_t type,scan,ascii,modifiers;int16_t x,y;uint32_t reserved; };
static void expired(int sig) { (void)sig;_exit(124); }
static uint64_t millis(void) { struct timespec time;CHECK(clock_gettime(CLOCK_MONOTONIC,&time)==0);return (uint64_t)time.tv_sec*1000+time.tv_nsec/1000000; }
int main(void) {
    struct sigaction action={0};action.sa_handler=expired;CHECK(sigaction(SIGALRM,&action,NULL)==0);
    timer_t timer;struct sigevent notification={0};notification.sigev_notify=SIGEV_SIGNAL;notification.sigev_signo=SIGALRM;
    CHECK(timer_create(CLOCK_MONOTONIC,&notification,&timer)==0);
    struct itimerspec deadline={0};deadline.it_value.tv_sec=120;CHECK(timer_settime(timer,0,&deadline,NULL)==0);
    struct window_result window={0};CHECK(syscall(506,360,180,"Close request test",&window,0)>=0&&window.id&&window.pixels);
    volatile uint32_t* pixels=(volatile uint32_t*)(uintptr_t)window.pixels;
    for(unsigned y=0;y<180;++y)for(unsigned x=0;x<360;++x)pixels[y*window.stride+x]=0x0024333d;
    printf("WMCLOSECHECK READY id=%u click close twice; client deliberately defers teardown\n",window.id);fflush(stdout);
    unsigned closes=0;uint64_t latest=0;
    for(;;) {
        struct event events[32];long count=syscall(510,window.id,events,32);CHECK(count>=0&&count<=32);
        for(long i=0;i<count;++i)if(events[i].type==0x80){++closes;latest=millis();printf("WMCLOSECHECK REQUEST count=%u\n",closes);fflush(stdout);}
        if(closes>=2&&millis()-latest>=10000)break;
        usleep(10000);
    }
    CHECK(timer_delete(timer)==0);
    puts("WMCLOSECHECK PASS repeated-request/survived-10s/no-forced-termination");
    return 0;
}
