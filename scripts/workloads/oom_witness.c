#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/prctl.h>
int main(void) {
    prctl(PR_SET_NAME,"Memory witness",0,0,0);
    const unsigned bytes=2*1024*1024;
    volatile unsigned char* data=mmap(NULL,bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(data==MAP_FAILED)return 2;
    for(unsigned i=0;i<bytes;i+=4096)data[i]=(unsigned char)((i/4096)*17+3);
    puts("OOM_WITNESS_READY");fflush(stdout);
    for(unsigned round=0;round<600;++round) {
        for(unsigned i=0;i<bytes;i+=4096)if(data[i]!=(unsigned char)((i/4096)*17+3))return 4;
        usleep(100000);
    }
    munmap((void*)data,bytes);puts("OOM_WITNESS_INTEGRITY_PASS");return 0;
}
