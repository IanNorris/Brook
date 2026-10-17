#define _GNU_SOURCE
/* Brook-only native desktop-scale ABI acceptance; do not run as a Linux control. */
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
int main(int argc,char **argv){
    long requested=syscall(0xB04L,0L,0L),applied=syscall(0xB04L,2L,0L);
    printf("SCALECHECK initial requested=%ld applied=%ld\n",requested,applied);
    if(requested<0||applied<0)return 1;
    if(argc>1){
        unsigned target=(unsigned)strtoul(argv[1],0,10);
        if(syscall(0xB04L,1L,(long)target)<0){perror("scale request");return 1;}
        for(int i=0;i<100;++i){if(syscall(0xB04L,2L,0L)==target)break;usleep(50000);}
        if(syscall(0xB04L,0L,0L)!=target||syscall(0xB04L,2L,0L)!=target){puts("SCALECHECK FAIL application deadline");return 1;}
    }
    long before=syscall(0xB04L,0L,0L);
    errno=0;if(syscall(0xB04L,1L,125L)!=-1||errno!=EINVAL)return 2;
    errno=0;if(syscall(0xB04L,3L,100L)!=-1||errno!=EINVAL)return 3;
    if(syscall(0xB04L,0L,0L)!=before)return 4;
    printf("SCALECHECK PASS requested=%ld applied=%ld invalid_requests_rejected=2\n",before,syscall(0xB04L,2L,0L));return 0;
}
