#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
static uint64_t ns(void){struct timespec t;assert(clock_gettime(CLOCK_MONOTONIC,&t)==0);return (uint64_t)t.tv_sec*1000000000+t.tv_nsec;}
static int run(char** argv){
    pid_t child=fork();assert(child>=0);
    if(!child){execv(argv[0],argv);perror(argv[0]);_exit(127);}
    int status;pid_t got;do{got=waitpid(child,&status,0);}while(got<0 && errno==EINTR);assert(got==child);
    if(!WIFEXITED(status) || WEXITSTATUS(status)){fprintf(stderr,"COMPILER_CHILD_FAIL executable=%s wait_status=%d\n",argv[0],status);return 1;}
    return 0;
}
int main(int argc,char** argv){
    assert(argc==6);assert(setenv("PATH",argv[5],1)==0 && setenv("TMPDIR","/tmp",1)==0 && setenv("HOME","/data/home",1)==0);
    setvbuf(stdout,NULL,_IONBF,0);
    char* compile[]={argv[1],"-x","c++","-std=c++17","-O2","-pthread",argv[2],"-o",argv[3],NULL};
    uint64_t start=ns();if(run(compile))return 1;uint64_t compiled=ns();
    char* execute[]={argv[3],argv[4],NULL};if(run(execute))return 1;uint64_t end=ns();
    printf("COMPILER_ACCEPT_PASS compile_ns=%llu run_ns=%llu total_ns=%llu\n",(unsigned long long)(compiled-start),(unsigned long long)(end-compiled),(unsigned long long)(end-start));
}
