#define _GNU_SOURCE
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <unistd.h>

// Brook currently detaches a nonleader exec into its own AS/group. Exercise
// that behavior; a leader exec correctly kills its siblings and cannot be used
// to test a surviving old-AS thread.
static uintptr_t expected;
static int kick[2], reply[2], exec_kick[2], exec_reply[2];
static const char* executable;
static int receive(int fd,char* result) {
    struct pollfd p={fd,POLLIN,0};
    return poll(&p,1,5000)==1&&read(fd,result,1)==1;
}
static int zero_growth(uintptr_t base) {
    uintptr_t page=(base+4095)&~(uintptr_t)4095;
    if((uintptr_t)syscall(SYS_brk,page+4096)!=page+4096)return 0;
    volatile unsigned char* bytes=(void*)page;
    for(unsigned i=0;i<4096;++i)if(bytes[i])return 0;
    bytes[0]=0x7b;return 1;
}
static void* old_thread(void* unused) {
    (void)unused; char token=0;
    if(!receive(kick[0],&token))return NULL;
    uintptr_t current=(uintptr_t)syscall(SYS_brk,0);
    char result=current==expected&&zero_growth(current)?'P':'F';
    write(reply[1],&result,1);return NULL;
}
static void* exec_thread(void* unused) {
    (void)unused; char token=0;
    if(!receive(exec_kick[0],&token))return NULL;
    char fd[24];snprintf(fd,sizeof(fd),"%d",exec_reply[1]);
    execl(executable,executable,"new-image",fd,(char*)0);
    write(exec_reply[1],"F",1);return NULL;
}
int main(int argc,char**argv) {
    if(argc==3) {
        int fd=atoi(argv[2]);uintptr_t base=(uintptr_t)syscall(SYS_brk,0);
        char result=zero_growth(base)?'N':'F';
        return write(fd,&result,1)==1&&result=='N'?0:2;
    }
    executable=argv[0];
    if(pipe(kick)||pipe(reply)||pipe(exec_kick)||pipe(exec_reply))return 3;
    pthread_t peer,execer;
    if(pthread_create(&peer,NULL,old_thread,NULL)||pthread_create(&execer,NULL,exec_thread,NULL))return 4;
    uintptr_t base=(uintptr_t)syscall(SYS_brk,0);
    expected=(base+65536+4095)&~(uintptr_t)4095;
    if((uintptr_t)syscall(SYS_brk,expected)!=expected)return 5;
    char result=0;
    if(write(exec_kick[1],"x",1)!=1||!receive(exec_reply[0],&result)||result!='N') {
        fprintf(stderr,"BRKEXECCHECK FAIL nonleader exec result=%c\n",result);return 6;
    }
    if((uintptr_t)syscall(SYS_brk,0)!=expected)return 7;
    if(write(kick[1],"x",1)!=1||!receive(reply[0],&result)||result!='P') {
        fprintf(stderr,"BRKEXECCHECK FAIL shared old-AS break result=%c\n",result);return 8;
    }
    if((uintptr_t)syscall(SYS_brk,0)!=expected+4096)return 9;
    if(pthread_join(peer,NULL))return 10;
    puts("BRKEXECCHECK PASS nonleader-exec/isolated-break/shared-old-AS/zero-growth");
    return 0;
}
