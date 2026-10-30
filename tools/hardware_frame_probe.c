/* Real syscall negative/lifetime tests, run only on a private test guest. */
#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "../src/shared/brook_debug_frame.h"
static int stale(long token){struct BrookDebugFrameInfo info;errno=0;return syscall(BROOK_SYS_DEBUG_FRAME,1L,token,(long)&info,(long)sizeof(info),0L,0L)==-1&&errno==ESTALE;}
int main(int argc,char** argv){
    long token=syscall(BROOK_SYS_DEBUG_FRAME,0L,0L,0L,0L,0L,0L);if(token<=0){perror("FRAME request");return 1;}
    int result=1;struct BrookDebugFrameInfo info={0};struct timespec pause={0,10000000};
    errno=0;if(syscall(BROOK_SYS_DEBUG_FRAME,0L,0L,0L,0L,0L,0L)!=-1||errno!=EBUSY)goto done;
    for(unsigned i=0;i<500;i++){if(syscall(BROOK_SYS_DEBUG_FRAME,1L,token,(long)&info,(long)sizeof(info),0L,0L))goto done;if(info.state!=1)break;nanosleep(&pause,NULL);}
    if(info.state!=2||info.bytes!=(uint64_t)info.width*info.height*4||info.stride!=info.width||info.format!=1||!info.epoch)goto done;
    if(argc==2&&!strcmp(argv[1],"--expiry")){
        puts("FRAME_PROBE_WAIT_EXPIRY");fflush(stdout);struct timespec expiry={31,0};
        while(nanosleep(&expiry,&expiry)&&errno==EINTR){}
        if(!stale(token))goto done;
        puts("FRAME_PROBE_EXPIRY_PASS");return 0;
    }
    if(argc==2&&!strcmp(argv[1],"--hold")){
        puts("FRAME_PROBE_HOLD");fflush(stdout);struct timespec hold={10,0};
        while(nanosleep(&hold,&hold)&&errno==EINTR){}
    }
    uint8_t chunk[64],again[64];
    if(syscall(BROOK_SYS_DEBUG_FRAME,2L,token,(long)chunk,64L,0L,0L)!=64)goto done;
    if(syscall(BROOK_SYS_DEBUG_FRAME,2L,token,(long)again,64L,0L,0L)!=64||memcmp(chunk,again,64))goto done;
    errno=0;if(syscall(BROOK_SYS_DEBUG_FRAME,2L,token,(long)chunk,65537L,0L,0L)!=-1||errno!=EINVAL)goto done;
    errno=0;if(syscall(BROOK_SYS_DEBUG_FRAME,2L,token,(long)chunk,1L,(long)info.bytes,0L)!=-1||errno!=EINVAL)goto done;
    errno=0;if(syscall(BROOK_SYS_DEBUG_FRAME,2L,token,0L,16L,0L,0L)!=-1||errno!=EFAULT)goto done;
    pid_t child=fork();if(child<0)goto done;
    if(!child){
        if(!stale(token))_exit(11);
        errno=0;if(syscall(BROOK_SYS_DEBUG_FRAME,3L,token,0L,0L,0L,0L)!=-1||errno!=ESTALE)_exit(12);
        if(setuid(1000))_exit(13);
        errno=0;if(syscall(BROOK_SYS_DEBUG_FRAME,0L,0L,0L,0L,0L,0L)!=-1||errno!=EPERM)_exit(14);
        _exit(0);
    }
    int status;if(waitpid(child,&status,0)!=child||!WIFEXITED(status)||WEXITSTATUS(status))goto done;
    if(syscall(BROOK_SYS_DEBUG_FRAME,3L,token,0L,0L,0L,0L))goto done;
    if(!stale(token))goto done;
    printf("FRAME_PROBE_PASS owner isolation root-only busy invalid-pointer bounds immutable-chunk released-stale width=%u height=%u epoch=%llu\n",info.width,info.height,(unsigned long long)info.epoch);return 0;
done:
    (void)syscall(BROOK_SYS_DEBUG_FRAME,3L,token,0L,0L,0L,0L);
    fprintf(stderr,"FRAME_PROBE_FAILED state=%u errno=%d\n",info.state,errno);return result;
}
