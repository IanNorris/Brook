/* Diagnostic only: a pass does not prove all interrupt/return paths correct. */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

struct failure { uint64_t rax,rcx,r14,flags,which,remaining; };
extern int loader_cmov_probe(struct failure *,uint64_t);
static uint64_t number(const char *text,uint64_t maximum) {
    char *end;errno=0;uint64_t value=strtoull(text,&end,10);
    if(errno || !*text || *end || !value || value>maximum) {
        fprintf(stderr,"Invalid positive bounded argument: %s\n",text);exit(2);
    }
    return value;
}
int main(int argc,char **argv) {
    if(argc!=4) {fprintf(stderr,"Usage: %s workers iterations rounds\n",argv[0]);return 2;}
    unsigned workers=number(argv[1],64),rounds=number(argv[3],1000);
    uint64_t iterations=number(argv[2],1000000000);
    setvbuf(stdout,NULL,_IOLBF,0);
    struct timespec before,after;clock_gettime(CLOCK_MONOTONIC,&before);
    unsigned started=0;int bad=0;
    for(unsigned i=0;i<workers;++i) {
        pid_t pid=fork();
        if(pid<0) {perror("fork");bad=1;break;}
        if(!pid) {
            for(unsigned round=0;round<rounds;++round) {
                struct failure f={0};
                if(loader_cmov_probe(&f,iterations)) {
                    printf("CMOV_FAIL pid=%ld worker=%u round=%u case=%"PRIu64
                           " remaining=%"PRIu64" rax=%#"PRIx64" rcx=%#"PRIx64
                           " r14=%#"PRIx64" compare_flags=%#"PRIx64"\n",
                           (long)getpid(),i,round,f.which,f.remaining,f.rax,f.rcx,f.r14,f.flags);
                    _exit(1);
                }
            }
            printf("CMOV_WORKER_PASS worker=%u iterations=%"PRIu64" rounds=%u\n",i,iterations,rounds);
            _exit(0);
        }
        ++started;
    }
    for(unsigned i=0;i<started;++i) {
        int status;pid_t pid;
        do {pid=waitpid(-1,&status,0);} while(pid<0 && errno==EINTR);
        if(pid<0 || !WIFEXITED(status) || WEXITSTATUS(status))bad=1;
    }
    clock_gettime(CLOCK_MONOTONIC,&after);
    double elapsed=after.tv_sec-before.tv_sec+(after.tv_nsec-before.tv_nsec)/1e9;
    printf("CMOV_%s workers=%u pairs=%"PRIu64" elapsed_s=%.6f\n",bad?"FAILED":"PASS",
           started,(uint64_t)started*iterations*rounds,elapsed);
    return bad;
}
