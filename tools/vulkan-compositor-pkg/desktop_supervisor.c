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
#include "desktop_supervisor_restart.h"

static volatile sig_atomic_t stopping=0;
static void stop(int signalNumber){
    stopping=signalNumber;
}
static void sleep_seconds(unsigned seconds){
    struct timespec remaining={(time_t)seconds,0};
    while(!stopping && nanosleep(&remaining,&remaining) && errno==EINTR){}
}
static bool monotonic_ns(uint64_t *ns){
    struct timespec elapsed;
    if(clock_gettime(CLOCK_MONOTONIC,&elapsed))return false;
    *ns=(uint64_t)elapsed.tv_sec*1000000000ull+(uint64_t)elapsed.tv_nsec;
    return true;
}

int main(int argc,char **argv){
    int partial=0,asyncPartial=0,traceSource=0,pipeline=0;
    const char *socketPath=NULL,*renderer=NULL,*nvIcd=NULL;
    for(int i=1;i<argc;++i){
        if(!strcmp(argv[i],"--partial") && !partial && !asyncPartial)partial=1;
        else if(!strcmp(argv[i],"--async-partial") && !partial && !asyncPartial)asyncPartial=1;
        else if(!strcmp(argv[i],"--trace-gpu-source") && !traceSource)traceSource=1;
        else if(!strcmp(argv[i],"--pipeline") && !pipeline)pipeline=1;
        else if(!strcmp(argv[i],"--gpu-source") && !socketPath && i+1<argc)socketPath=argv[++i];
        else if(!strcmp(argv[i],"--nv-glass") && !nvIcd && i+1<argc)nvIcd=argv[++i];
        else if(!renderer && argv[i][0]!='-')renderer=argv[i];
        else return 2;
    }
    if(!renderer || renderer[0]!='/' || (socketPath && socketPath[0]!='/') || (traceSource && !socketPath) ||
       (pipeline && !asyncPartial) || (nvIcd && (nvIcd[0]!='/' || partial || asyncPartial || socketPath))){
        fprintf(stderr,"usage: brook-desktop-supervisor [--partial | --async-partial [--pipeline]] [--gpu-source /socket] [--trace-gpu-source] [--nv-glass /nvk-icd.json] /absolute/path/to/brook-desktop-renderer\n");return 2;
    }
    if(nvIcd){
        if(access(nvIcd,R_OK)!=0){
            fprintf(stderr,"DESKTOP_SUPERVISOR_NV_FALLBACK ICD unavailable: %s\n",nvIcd);
            return 0;
        }
        if(setenv("VK_DRIVER_FILES",nvIcd,1) || setenv("BROOK_RENDER_NV_GLASS","1",1) ||
           setenv("BROOK_RENDER_BLUR","1",0) || setenv("BROOK_RENDER_PIPELINE","1",1) ||
           unsetenv("BROOK_RENDER_PARTIAL") || unsetenv("BROOK_RENDER_ASYNC_PARTIAL") ||
           unsetenv("BROOK_DESKTOP_GPU_SOCKET"))return 1;
    }
#ifdef BROOK_SUPERVISOR_DIAGNOSTIC_REUSE_BACKDROP
    if(setenv("BROOK_RENDER_REUSE_TILE_BACKDROP","1",1))return 1;
#endif
#ifdef BROOK_SUPERVISOR_DIAGNOSTIC_BARE
    if(setenv("BROOK_RENDER_BARE","1",1))return 1;
#endif
    if(partial && setenv("BROOK_RENDER_PARTIAL","1",1))return 1;
    if(asyncPartial && (setenv("BROOK_RENDER_ASYNC_PARTIAL","1",1) ||
       setenv("BROOK_RENDER_BUFFER_COUNT","2",1) ||
       setenv("BROOK_RENDER_ASYNC_PARTIAL_GRACE","1",0)))return 1;
    /* Two frames in flight (desktop_pipeline.h). Now the renderer's default;
     * kept as an explicit, harmless request. An explicit environment value,
     * including BROOK_RENDER_PIPELINE=1, wins. */
    if(pipeline && setenv("BROOK_RENDER_PIPELINE","2",0))return 1;
    /* Opt-in Glass backdrop blur without rebuilding the image: the marker
     * /nix/var/brook/render-blur (persistent ext2; /etc is rebuilt from the
     * initrd each boot) or /etc/brook-render-blur enables it; an explicit
     * environment value wins. */
    if((access("/nix/var/brook/render-blur",F_OK)==0 || access("/etc/brook-render-blur",F_OK)==0) &&
       setenv("BROOK_RENDER_BLUR","1",0))return 1;
#ifdef BROOK_SUPERVISOR_SERIAL
    if(!asyncPartial || setenv("BROOK_RENDER_ASYNC_PARTIAL_SERIAL","1",1))return 1;
#endif
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
    struct DesktopRestartPolicy restart={.retry_s=1};
    while(!stopping){
        uint64_t started_ns=0,finished_ns=0;
        const bool started_ok=monotonic_ns(&started_ns);
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
        int status=-1;
        if(pid<0){fprintf(stderr,"DESKTOP_SUPERVISOR_FORK_FAILED errno=%d\n",forkError);}
        else{
            pid_t done;
            if(stopping)(void)kill(pid,SIGTERM);
            do {done=waitpid(pid,&status,0);if(done<0 && errno==EINTR && stopping)(void)kill(pid,SIGTERM);}
            while(done<0 && errno==EINTR);
            if(done<0){fprintf(stderr,"DESKTOP_SUPERVISOR_WAIT_FAILED errno=%d\n",errno);return 1;}
        }
        if(stopping)break;
        if(nvIcd && pid>0 && WIFEXITED(status) && WEXITSTATUS(status)==78){
            // Renderer rejected a permanent configuration, including dual-head.
            fprintf(stderr,"DESKTOP_SUPERVISOR_NV_FALLBACK renderer declined this configuration\n");
            return 0;
        }
        const bool finished_ok=monotonic_ns(&finished_ns);
        const uint64_t runtime_ns=pid>0 && started_ok && finished_ok && finished_ns>=started_ns?
            finished_ns-started_ns:0;
        const unsigned delay=desktop_restart_delay(&restart,status,finished_ok,finished_ns,runtime_ns);
        if(pid>0)fprintf(stderr,"DESKTOP_SUPERVISOR_EXIT status=%d retry_s=%u\n",status,delay);
        sleep_seconds(delay);
    }
    return 0;
}
