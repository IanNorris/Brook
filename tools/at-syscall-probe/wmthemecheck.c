#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
int main(int argc,char **argv) {
    if(argc!=2)return 2;
    long expected=strtol(argv[1],NULL,10),before=syscall(0xB02L,0L,0L);
    errno=0;long invalid=syscall(0xB02L,1L,3L);int invalid_error=errno;
    errno=0;long unknown=syscall(0xB02L,2L,0L);int unknown_error=errno;
    long after=syscall(0xB02L,0L,0L);
    if(before!=expected||after!=before||invalid!=-1||invalid_error!=EINVAL||unknown!=-1||unknown_error!=EINVAL) {
        printf("WMTHEMECHECK FAIL expected=%ld before=%ld after=%ld invalid=%ld/%d op=%ld/%d\n",expected,before,after,invalid,invalid_error,unknown,unknown_error);return 1;
    }
    printf("WMTHEMECHECK PASS restored=%ld invalid_selection=preserved\n",after);return 0;
}
