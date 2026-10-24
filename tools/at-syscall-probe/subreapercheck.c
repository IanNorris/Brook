#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <unistd.h>

static volatile sig_atomic_t child_signals;
static void child_signal(int s) {(void)s;++child_signals;}
static int get_flag(void) {int n=-1;return prctl(PR_GET_CHILD_SUBREAPER,&n,11UL,22UL,33UL)==0?n:-1;}
static int set_flag(int n) {return prctl(PR_SET_CHILD_SUBREAPER,(unsigned long)n,11UL,22UL,33UL);}
static void* set_from_thread(void* arg) {return (void*)(long)set_flag((int)(long)arg);}
static int transfer(int fd,void* data,size_t n,int writing) {
    char* p=data;
    while(n) {ssize_t k=writing?write(fd,p,n):read(fd,p,n);if(k<0 && errno==EINTR)continue;if(k<=0)return 0;p+=k;n-=(size_t)k;}
    return 1;
}
static int wait_exit(pid_t p,int code) {int s;pid_t r;do {r=waitpid(p,&s,0);}while(r<0 && errno==EINTR);return r==p && WIFEXITED(s) && WEXITSTATUS(s)==code;}
static int adoption(int zombie) {
    int report[2],gate[2];if(pipe(report)||pipe(gate))return 0;
    pid_t expected=getpid(),child=fork();
    if(child==0) {
        close(report[0]);close(gate[1]);pid_t grand=fork();
        if(grand==0) {char c;if(zombie)_exit(23);if(!transfer(gate[0],&c,1,0))_exit(90);_exit(getppid()==expected?17:91);}
        if(grand<0)_exit(92);
        if(zombie) {siginfo_t si;int r;do {r=waitid(P_PID,grand,&si,WEXITED|WNOWAIT);}while(r<0 && errno==EINTR);if(r || si.si_pid!=grand)_exit(93);}
        if(!transfer(report[1],&grand,sizeof(grand),1))_exit(94);
        _exit(0);
    }
    close(report[1]);close(gate[0]);pid_t grand=-1;
    int ok=child>0 && transfer(report[0],&grand,sizeof(grand),0) && wait_exit(child,0);
    if(ok && !zombie) {char c='g';ok=transfer(gate[1],&c,1,1);}
    if(ok)ok=wait_exit(grand,zombie?23:17);
    close(report[0]);close(gate[1]);return ok;
}
static int nested_adoption(int clear) {
    int report[2],gate[2];if(pipe(report)||pipe(gate))return 0;
    pid_t outer=getpid(),child=fork();
    if(child==0) {
        close(report[0]);
        if(set_flag(1) || (clear && set_flag(0)))_exit(80);
        pid_t expected=clear?outer:getpid(),middle=fork();
        if(middle==0) {
            pid_t grand=fork();
            if(grand==0) {char c;if(!transfer(gate[0],&c,1,0))_exit(81);_exit(getppid()==expected?31:82);}
            if(grand<0 || !transfer(report[1],&grand,sizeof(grand),1))_exit(83);
            _exit(0);
        }
        if(middle<0 || !wait_exit(middle,0))_exit(84);
        if(!clear) {
            char c='g';if(!transfer(gate[1],&c,1,1))_exit(85);
            int s;pid_t g;do {g=waitpid(-1,&s,0);}while(g<0 && errno==EINTR);
            if(g<=0 || !WIFEXITED(s) || WEXITSTATUS(s)!=31)_exit(86);
        }
        _exit(0);
    }
    close(report[1]);close(gate[0]);pid_t grand=-1;
    int ok=child>0 && transfer(report[0],&grand,sizeof(grand),0) && wait_exit(child,0);
    if(ok && clear) {char c='g';ok=transfer(gate[1],&c,1,1) && wait_exit(grand,31);}
    if(ok && !clear) {int s;ok=waitpid(grand,&s,WNOHANG)==-1 && errno==ECHILD;}
    close(report[0]);close(gate[1]);return ok;
}
static void* surviving_thread(void* arg) {
    char byte;
    if(!transfer((int)(long)arg,&byte,1,0))_exit(60);
    _exit(adoption(0) && adoption(1)?0:61);
}
static int exited_leader(void) {
    int gate[2];if(pipe(gate))return 0;
    pid_t child=fork();
    if(child==0) {
        close(gate[1]);pthread_t thread;
        if(set_flag(1) || pthread_create(&thread,NULL,surviving_thread,(void*)(long)gate[0]))_exit(62);
        syscall(SYS_exit,0);_exit(63);
    }
    close(gate[0]);int zombie=0;
    for(int n=0;child>0 && n<300;++n) {
        char path[64],line[256];snprintf(path,sizeof(path),"/proc/%ld/status",(long)child);
        FILE* f=fopen(path,"r");
        if(f) {while(fgets(line,sizeof(line),f))if(!strncmp(line,"State:\tZ",8))zombie=1;fclose(f);}
        if(zombie)break;
        usleep(10000);
    }
    char byte='g';int ok=zombie && transfer(gate[1],&byte,1,1);
    close(gate[1]);
    if(!ok && child>0)kill(child,SIGKILL);
    return child>0 && wait_exit(child,0) && ok;
}
#define CHECK(test,name) do {if(!(test)){printf("{\"subreaper_pass\":false,\"check\":\"%s\",\"errno\":%d}\n",name,errno);return 1;}}while(0)
int main(int argc,char** argv) {
    if(argc==2 && !strcmp(argv[1],"exec-child"))return get_flag()!=1;
    alarm(30);
    struct sigaction sa={.sa_handler=child_signal,.sa_flags=SA_RESTART};sigemptyset(&sa.sa_mask);
    CHECK(sigaction(SIGCHLD,&sa,NULL)==0,"SIGCHLD handler");
    CHECK(get_flag()==0,"initial GET writes disabled flag");
    CHECK(prctl(PR_GET_CHILD_SUBREAPER,(void*)1,0UL,0UL,0UL)==-1 && errno==EFAULT,"invalid GET pointer");
    CHECK(set_flag(7)==0 && get_flag()==1,"nonzero SET and ignored unused arguments");
    pthread_t t;void* result;
    CHECK(pthread_create(&t,NULL,set_from_thread,(void*)0L)==0 && pthread_join(t,&result)==0 && !result && get_flag()==0,"thread shares disabled setting");
    CHECK(pthread_create(&t,NULL,set_from_thread,(void*)1L)==0 && pthread_join(t,&result)==0 && !result && get_flag()==1,"thread shares enabled setting");
    pid_t child=fork();if(child==0)_exit(get_flag()!=0);
    CHECK(child>0 && wait_exit(child,0),"fork clears flag");
    child=fork();if(child==0) {if(set_flag(1))_exit(70);execl(argv[0],argv[0],"exec-child",(char*)NULL);_exit(71);}
    CHECK(child>0 && wait_exit(child,0),"exec preserves flag");
    for(int n=0;n<12;++n)CHECK(adoption(n&1),"running/zombie adoption and wait status");
    CHECK(nested_adoption(0),"nearest nested subreaper");
    CHECK(nested_adoption(1),"cleared nested subreaper skipped");
    CHECK(exited_leader(),"exited leader with live subreaper sibling");
    CHECK(child_signals>0,"SIGCHLD notification");
    alarm(0);
    puts("{\"subreaper_pass\":true,\"adoption_rounds\":12,\"scope\":\"GET/SET, thread sharing, fork/exec, running/zombie adoption, nearest/cleared reaper, SIGCHLD and wait status\"}");return 0;
}
