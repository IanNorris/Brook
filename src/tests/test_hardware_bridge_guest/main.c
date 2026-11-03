#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <sys/syscall.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <pthread.h>
#include <signal.h>
static void* budget_thread(void* argument){
    int fd=*(int*)argument;char byte;return (void*)(uintptr_t)(read(fd,&byte,1)==1?0:1);
}
int main(int argc,char** argv){
    if(argc==3 && !strcmp(argv[1],"budget-child")){
        char byte;int fd=atoi(argv[2]);return read(fd,&byte,1)==1?0:90;
    }
    if(argc!=2 || getuid()!=1000)return 99;
    if(!strcmp(argv[1],"budget")){
        int release[3];pid_t children[3];
        for(int i=0;i<3;i++){
            int pair[2];if(pipe(pair))return 80;
            pid_t child=fork();if(child<0)return 81;
            if(!child){
                close(pair[1]);if(setsid()<0)_exit(82);
                char number[16];snprintf(number,sizeof(number),"%d",pair[0]);
                execl(argv[0],argv[0],"budget-child",number,(char*)0);_exit(83);
            }
            close(pair[0]);release[i]=pair[1];children[i]=child;
        }
        errno=0;pid_t extra=fork();
        if(extra==0)_exit(0);
        if(extra>0){waitpid(extra,0,0);return 84;}
        if(errno!=ENOMEM)return 85;
        if(write(release[0],"x",1)!=1)return 86;
        close(release[0]);int status=0;siginfo_t info={0};
        if(waitid(P_PID,children[0],&info,WEXITED|WNOWAIT) || info.si_pid!=children[0])return 79;
        extra=fork();if(extra==0)_exit(0);if(extra>0){waitpid(extra,0,0);return 78;}
        if(errno!=ENOMEM)return 77;
        if(waitpid(children[0],&status,0)!=children[0] || !WIFEXITED(status) || WEXITSTATUS(status))return 87;
        extra=fork();if(extra==0)_exit(0);if(extra<0)return 88;
        if(waitpid(extra,&status,0)!=extra || !WIFEXITED(status) || WEXITSTATUS(status))return 89;
        for(int i=1;i<3;i++){
            if(write(release[i],"x",1)!=1)return 91;
            close(release[i]);if(waitpid(children[i],&status,0)!=children[i] || !WIFEXITED(status) || WEXITSTATUS(status))return 92;
        }
        int pipes[3][2];pthread_t threads[3],extra_thread;void* thread_status=0;
        for(int i=0;i<3;i++){
            if(pipe(pipes[i]))return 93;
            if(pthread_create(&threads[i],0,budget_thread,&pipes[i][0]))return 94;
        }
        int refused=pthread_create(&extra_thread,0,budget_thread,&pipes[0][0]);
        if(refused!=EAGAIN && refused!=ENOMEM)return 95;
        if(write(pipes[0][1],"x",1)!=1 || pthread_join(threads[0],&thread_status) || thread_status)return 96;
        close(pipes[0][0]);close(pipes[0][1]);
        if(pipe(pipes[0]))return 97;
        int create_error=0;
        for(int attempt=0;attempt<100;attempt++){
            create_error=pthread_create(&extra_thread,0,budget_thread,&pipes[0][0]);
            if(!create_error)break;
            usleep(10000); // kernel PID reclamation may follow pthread_join
        }
        if(create_error)return 97;
        if(write(pipes[0][1],"x",1)!=1 || pthread_join(extra_thread,&thread_status) || thread_status)return 98;
        close(pipes[0][0]);close(pipes[0][1]);
        for(int i=1;i<3;i++){
            if(write(pipes[i][1],"x",1)!=1 || pthread_join(threads[i],&thread_status) || thread_status)return 99;
            close(pipes[i][0]);close(pipes[i][1]);
        }
        puts("DEBUG_JOB_BUDGET_PASS");return 0;
    }
    if(!strcmp(argv[1],"window")){
        struct {uint32_t id,stride;uint64_t pixels;} out={0};
        if(syscall(506L,160L,120L,"Hardware bridge window",&out,0L)<0)return 98;
        uint32_t* pixels=(uint32_t*)(uintptr_t)out.pixels;
        for(unsigned y=0;y<120;++y)for(unsigned x=0;x<160;++x)pixels[y*out.stride+x]=0x285b98;
        syscall(508L,(long)out.id);puts("window ready");fflush(stdout);
        sleep(30);syscall(507L,(long)out.id);return 0;
    }
    if(strcmp(argv[1],"hello world"))return 99;
    puts("hardware bridge: \"hello world\" ✓");
    fputs("separate stderr\n",stderr);return 17;
}
