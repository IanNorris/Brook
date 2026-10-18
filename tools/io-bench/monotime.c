#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

static uint64_t clock_ns(clockid_t which) {
    struct timespec t;
    if (clock_gettime(which, &t)) {perror("clock_gettime");exit(2);}
    return (uint64_t)t.tv_sec*1000000000ULL+(uint64_t)t.tv_nsec;
}
int main(int argc, char **argv) {
    if (argc == 1) {
        uint64_t n=clock_ns(CLOCK_MONOTONIC);
        uint64_t seconds=n/1000000000ULL, fraction=n%1000000000ULL;
        printf("%" PRIu64 ".%09" PRIu64 "\n", seconds,fraction);
        return 0;
    }
    if (argc!=3 || strcmp(argv[1],"--audit")) return 2;
    char *end;errno=0;unsigned long ms=strtoul(argv[2],&end,10);
    if(errno || *end || !ms || ms>30000)return 2;
    uint64_t start=clock_ns(CLOCK_MONOTONIC),previous[3]={0},backs[3]={0},largest[3]={0},samples=0;
    do {
        struct timeval tv;if(gettimeofday(&tv,NULL)){perror("gettimeofday");return 2;}
        uint64_t now[]={clock_ns(CLOCK_MONOTONIC),clock_ns(CLOCK_REALTIME),
            (uint64_t)tv.tv_sec*1000000000ULL+(uint64_t)tv.tv_usec*1000ULL};
        for(unsigned i=0;i<3;i++) {
            if(now[i]<previous[i]){backs[i]++;uint64_t d=previous[i]-now[i];if(d>largest[i])largest[i]=d;}
            previous[i]=now[i];
        }
        samples++;
        struct timespec delay={0,1000000};
        while(nanosleep(&delay,&delay) && errno==EINTR){}
    }while(clock_ns(CLOCK_MONOTONIC)-start<(uint64_t)ms*1000000ULL);
    printf("CLOCK_AUDIT samples=%" PRIu64 " monotonic_back=%" PRIu64 " realtime_back=%" PRIu64
        " gettimeofday_back=%" PRIu64 " realtime_max_back_ns=%" PRIu64 " gettimeofday_max_back_ns=%" PRIu64 "\n",
        samples,backs[0],backs[1],backs[2],largest[1],largest[2]);
    // Realtime regressions are diagnostic evidence, not a passing-clock claim.
    return backs[0]?1:0;
}
