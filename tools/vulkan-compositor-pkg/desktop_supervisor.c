#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stopping=0;
static void stop(int signalNumber){
    stopping=signalNumber;
}
static void sleep_seconds(unsigned seconds){
    struct timespec remaining={(time_t)seconds,0};
    while(!stopping && nanosleep(&remaining,&remaining) && errno==EINTR){}
}
int main(int argc,char **argv){
    int partial=0,asyncPartial=0,traceSource=0;const char *socketPath=NULL,*renderer=NULL;
    for(int i=1;i<argc;++i){
        if(!strcmp(argv[i],"--partial") && !partial && !asyncPartial)partial=1;
        else if(!strcmp(argv[i],"--async-partial") && !partial && !asyncPartial)asyncPartial=1;
        else if(!strcmp(argv[i],"--trace-gpu-source") && !traceSource)traceSource=1;
        else if(!strcmp(argv[i],"--gpu-source") && !socketPath && i+1<argc)socketPath=argv[++i];
        else if(!renderer && argv[i][0]!='-')renderer=argv[i];
        else return 2;
    }
    if(!renderer || renderer[0]!='/' || (socketPath && socketPath[0]!='/') || (traceSource && !socketPath)){
        fprintf(stderr,"usage: brook-desktop-supervisor [--partial | --async-partial] [--gpu-source /socket] [--trace-gpu-source] /absolute/path/to/brook-desktop-renderer\n");return 2;
    }
    if(partial && setenv("BROOK_RENDER_PARTIAL","1",1))return 1;
    if(asyncPartial && (setenv("BROOK_RENDER_ASYNC_PARTIAL","1",1) ||
       setenv("BROOK_RENDER_BUFFER_COUNT","2",1) ||
       setenv("BROOK_RENDER_ASYNC_PARTIAL_GRACE","1",0)))return 1;
    if(socketPath && (setenv("BROOK_DESKTOP_GPU_SOCKET",socketPath,1) || setenv("BROOK_RENDER_SELECTIVE_CAPTURE","1",1)))return 1;
    if(traceSource && setenv("BROOK_RENDER_GPU_SOURCE_TRACE","1",1))return 1;
#ifdef BROOK_SUPERVISOR_TEST_LATE_FENCE
    if(setenv("BROOK_RENDER_TEST_LATE_FENCE","1",1))return 1;
#endif
#ifdef BROOK_SUPERVISOR_TEST_BLOCKED_FENCE
    if(setenv("BROOK_RENDER_TEST_BLOCKED_FENCE","/tmp/brook-desktop-blocked-fixture-once",1))return 1;
#endif
#ifdef BROOK_SUPERVISOR_TEST_CHROME_SPANS
    if(setenv("BROOK_RENDER_CHROME_SPANS","1",1))return 1;
#endif
#ifdef BROOK_SUPERVISOR_TEST_GPU_SOURCE
    if(setenv("BROOK_DESKTOP_GPU_SOCKET","/tmp/brook-gpu-source-test.sock",1) || setenv("BROOK_RENDER_PROGRESS","1",1) ||
       setenv("BROOK_RENDER_GPU_SOURCE_TRACE","1",1))return 1;
#endif
#ifdef BROOK_SUPERVISOR_TEST_SELECTIVE_CAPTURE
    if(setenv("BROOK_DESKTOP_GPU_SOCKET","/tmp/brook-gpu-source-test.sock",1) || setenv("BROOK_RENDER_PROGRESS","1",1) ||
       setenv("BROOK_RENDER_GPU_SOURCE_TRACE","1",1) || setenv("BROOK_RENDER_SELECTIVE_CAPTURE","1",1))return 1;
#endif
#ifdef BROOK_SUPERVISOR_TEST_SELECTIVE_TIMING
    if(setenv("BROOK_DESKTOP_GPU_SOCKET","/tmp/brook-gpu-source-test.sock",1) || setenv("BROOK_RENDER_PROGRESS","1",1) ||
       setenv("BROOK_RENDER_SELECTIVE_CAPTURE","1",1) || setenv("BROOK_RENDER_TIMING_AGGREGATE","1",1))return 1;
#endif
    struct sigaction action={0};action.sa_handler=stop;sigemptyset(&action.sa_mask);
    if(sigaction(SIGTERM,&action,NULL) || sigaction(SIGINT,&action,NULL))return 1;
    unsigned delay=1;
    while(!stopping){
        struct timespec started={0},finished={0};(void)clock_gettime(CLOCK_MONOTONIC,&started);
        // Block termination across fork and publication, so the parent cannot
        // lose a stop request between creating and recording the child.
        sigset_t blocked,previous;sigemptyset(&blocked);sigaddset(&blocked,SIGTERM);sigaddset(&blocked,SIGINT);
        if(sigprocmask(SIG_BLOCK,&blocked,&previous))return 1;
        pid_t pid=fork();
        const int forkError=errno;
        if(!pid){
            struct sigaction reset={0};reset.sa_handler=SIG_DFL;sigemptyset(&reset.sa_mask);
            (void)sigaction(SIGTERM,&reset,NULL);(void)sigaction(SIGINT,&reset,NULL);
            (void)sigprocmask(SIG_SETMASK,&previous,NULL);
            execl(renderer,renderer,"--watch",(char*)NULL);_exit(127);
        }
        (void)sigprocmask(SIG_SETMASK,&previous,NULL);
        if(pid<0){fprintf(stderr,"DESKTOP_SUPERVISOR_FORK_FAILED errno=%d\n",forkError);}
        else{
            int status=0;pid_t done;
            if(stopping)(void)kill(pid,SIGTERM);
            do {done=waitpid(pid,&status,0);if(done<0 && errno==EINTR && stopping)(void)kill(pid,SIGTERM);}
            while(done<0 && errno==EINTR);
            if(done<0){fprintf(stderr,"DESKTOP_SUPERVISOR_WAIT_FAILED errno=%d\n",errno);return 1;}
            if(stopping)break;
            fprintf(stderr,"DESKTOP_SUPERVISOR_EXIT status=%d retry_s=%u\n",status,delay);
        }
        (void)clock_gettime(CLOCK_MONOTONIC,&finished);
        if(finished.tv_sec-started.tv_sec>=30)delay=1;
        sleep_seconds(delay);if(delay<30)delay=delay>15?30:delay*2;
    }
    return 0;
}
