#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>
static long ctl(long op,long value){return syscall(0xB40,op,value,0L,0L,0L,0L);}
int main(void){
    for(unsigned i=0;i<13;++i){long v=ctl(22,i);assert(v>=0);printf("app%u=%ld ",i,v);}
    printf("slots=%ld ram_allocated=%ld ram_payload=%ld disk_writes=%ld disk_reads=%ld ram_writes=%ld ram_reads=%ld\n",
        ctl(2,0),ctl(14,1),ctl(14,2),ctl(14,12),ctl(14,13),ctl(14,14),ctl(14,15));
    for(unsigned i=0;i<512;++i){long v=ctl(23,i);assert(v>=0);if(v)printf("barrier%u=%ld ",i,v);}
    puts("APPLICATION_STATS_PASS");
}
