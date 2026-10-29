#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <time.h>
static void *chunks[1024];
static unsigned long free_kib(void) {
    char text[2048];int fd=open("/proc/meminfo",O_RDONLY);
    if(fd<0)return 0;
    ssize_t n=read(fd,text,sizeof(text)-1);close(fd);
    if(n<=0)return 0;
    text[n]=0;char *p=strstr(text,"MemFree:");
    return p?strtoul(p+8,NULL,10):0;
}
static uint64_t ms(void) {
    struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))return 0;
    return (uint64_t)t.tv_sec*1000+(uint64_t)t.tv_nsec/1000000;
}
int main(void) {
    prctl(PR_SET_NAME,"Memory test",0,0,0);
    unsigned count=0;uint64_t deadline=ms()+60000;
    while(ms()<deadline) {
        unsigned long free=free_kib();
        if(free>7*1024 && count<1024) {
            void *p=mmap(NULL,1024*1024,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
            if(p==MAP_FAILED){usleep(10000);continue;}
            for(unsigned i=0;i<1024*1024;i+=4096)((volatile unsigned char*)p)[i]=(unsigned char)(count+1);
            chunks[count++]=p;
            if(!(count%32)){printf("OOM_HOG_HELD_MIB=%u free_kib=%lu\n",count,free);fflush(stdout);}
            usleep(2000);
        } else usleep(20000);
    }
    for(unsigned i=0;i<count;++i)munmap(chunks[i],1024*1024);
    fprintf(stderr,"OOM_HOG_TIMEOUT_NO_KILL held_mib=%u\n",count);return 3;
}
