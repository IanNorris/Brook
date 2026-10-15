#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>
struct window {uint32_t id,stride;uint64_t pixels;};
int main(int argc,char **argv) {
    unsigned count=argc>1?(unsigned)atoi(argv[1]):20;
    if(count<2||count>28)return 2;
    setvbuf(stdout,NULL,_IONBF,0);
    struct window windows[28]={0};unsigned created=0;
    for(unsigned i=0;i<count;++i) {
        char title[64];snprintf(title,sizeof(title),"Window %02u - overflow check",i+1);
        if(syscall(506L,320L,120L,title,&windows[i],0L)<0)break;
        ++created;uint32_t *pixels=(void*)(uintptr_t)windows[i].pixels;
        for(unsigned y=0;y<120;++y)for(unsigned x=0;x<320;++x)pixels[y*windows[i].stride+x]=0xff203040+((i*5)<<8);
        syscall(508L,(long)windows[i].id);
    }
    printf("WMOVERFLOWCHECK READY pid=%d windows=%u\n",getpid(),created);
    /* Bounded visual fixture. Quit early by sending SIGTERM to this process. */
    struct timespec began,now;clock_gettime(CLOCK_MONOTONIC,&began);
    do {usleep(10000);clock_gettime(CLOCK_MONOTONIC,&now);}while(now.tv_sec-began.tv_sec<300);
    for(unsigned i=0;i<created;++i)syscall(507L,(long)windows[i].id);
    return created==count?0:1;
}
