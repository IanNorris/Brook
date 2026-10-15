#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"WAITIDCHECK FAIL line=%d errno=%d\n",__LINE__,errno); ++failures; } } while(0)
static int raw_wait(int type, pid_t pid, siginfo_t *info, int options) {
    return (int)syscall(SYS_waitid,type,pid,info,options,NULL);
}
static siginfo_t wait_event(int type,pid_t pid,int options) {
    siginfo_t info; memset(&info,0xa5,sizeof(info));
    CHECK(raw_wait(type,pid,&info,options)==0);
    return info;
}
static pid_t blocked_child(int pipefd[2], int status) {
    CHECK(pipe(pipefd)==0); pid_t child=fork(); CHECK(child>=0);
    if (!child) { close(pipefd[1]);char c;ssize_t n;do {n=read(pipefd[0],&c,1);} while(n<0&&errno==EINTR); _exit(n==1?status:111); }
    close(pipefd[0]); return child;
}
static void release_child(int pipefd[2]) { CHECK(write(pipefd[1],"x",1)==1);close(pipefd[1]); }
static void basics(void) {
    siginfo_t info; errno=0;
    CHECK(raw_wait(P_ALL,0,&info,WEXITED|WNOHANG)==-1&&errno==ECHILD);
    CHECK(raw_wait(999,0,&info,WEXITED)==-1&&errno==EINVAL);
    CHECK(raw_wait(P_ALL,0,&info,0)==-1&&errno==EINVAL);
    CHECK(raw_wait(P_ALL,0,&info,WEXITED|0x4000)==-1&&errno==EINVAL);
    CHECK(raw_wait(P_PID,0,&info,WEXITED)==-1&&errno==EINVAL);
    CHECK(raw_wait(P_PID,-1,&info,WEXITED)==-1&&errno==EINVAL);
    CHECK(raw_wait(P_PGID,-1,&info,WEXITED)==-1&&errno==EINVAL);
    int fds[2];pid_t child=blocked_child(fds,200);
    info=wait_event(P_PID,child,WEXITED|WNOHANG);
    CHECK(info.si_pid==0&&info.si_signo==0&&info.si_code==0);
    errno=0;CHECK(raw_wait(P_PID,child+65536,&info,WEXITED|WNOHANG)==-1&&errno==ECHILD);
    release_child(fds);
    info=wait_event(P_PID,child,WEXITED|WNOWAIT);
    CHECK(info.si_pid==child&&info.si_signo==SIGCHLD&&info.si_code==CLD_EXITED&&info.si_status==200&&info.si_uid==getuid());
    info=wait_event(P_PID,child,WEXITED|WNOWAIT|WNOHANG);
    CHECK(info.si_pid==child&&info.si_status==200);
    errno=0;CHECK(raw_wait(P_PID,child,(void *)1,WEXITED|WNOWAIT)==-1&&errno==EFAULT);
    info=wait_event(P_PID,child,WEXITED|WNOHANG);
    CHECK(info.si_pid==child&&info.si_status==200);
    errno=0;CHECK(raw_wait(P_PID,child,&info,WEXITED|WNOHANG)==-1&&errno==ECHILD);
    child=fork();CHECK(child>=0);if(!child)_exit(0);
    CHECK(raw_wait(P_PID,child,NULL,WEXITED)==0);
    child=blocked_child(fds,0);CHECK(kill(child,SIGKILL)==0);close(fds[1]);
    info=wait_event(P_PID,child,WEXITED);
    CHECK(info.si_pid==child&&info.si_code==CLD_KILLED&&info.si_status==SIGKILL);
    puts("WAITIDCHECK basics done");
}
static pid_t shared_child;
static void *forker(void *unused) { (void)unused;shared_child=fork();if(!shared_child)_exit(71);return NULL; }
static void *sibling_waiter(void *unused) {
    (void)unused;siginfo_t info=wait_event(P_PID,shared_child,WEXITED);
    CHECK(info.si_pid==shared_child&&info.si_status==71);return NULL;
}
static void *surviving_parent(void *unused) {
    (void)unused;usleep(20000);pid_t child=fork();if(!child)_exit(73);
    siginfo_t info;int r=raw_wait(P_PID,child,&info,WEXITED);
    _exit(r==0&&info.si_pid==child&&info.si_status==73?0:99);
}
static void threads(void) {
    pthread_t thread;CHECK(pthread_create(&thread,NULL,forker,NULL)==0);CHECK(pthread_join(thread,NULL)==0);
    CHECK(pthread_create(&thread,NULL,sibling_waiter,NULL)==0);CHECK(pthread_join(thread,NULL)==0);
    pid_t controller=fork();CHECK(controller>=0);
    if(!controller) {
        if(pthread_create(&thread,NULL,surviving_parent,NULL))_exit(98);
        syscall(SYS_exit,0);_exit(97);
    }
    int status;CHECK(waitpid(controller,&status,0)==controller&&WIFEXITED(status)&&WEXITSTATUS(status)==0);
    puts("WAITIDCHECK thread-group parents done");
}
static pthread_barrier_t race_barrier;
struct race_result { int kind,result,error,status;pid_t pid; };
static void *consumer(void *opaque) {
    struct race_result *r=opaque;pthread_barrier_wait(&race_barrier);errno=0;
    if(r->kind) {
        siginfo_t info;r->result=raw_wait(P_PID,r->pid,&info,WEXITED|WNOHANG);r->error=errno;
        if(!r->result){r->result=info.si_pid;r->status=info.si_status;}
    } else {r->result=waitpid(r->pid,&r->status,WNOHANG);r->error=errno;if(r->result>0)r->status=WEXITSTATUS(r->status);}
    return NULL;
}
static void races(void) {
    for(int round=0;round<32;++round) {
        pid_t child=fork();CHECK(child>=0);if(!child)_exit(42);
        siginfo_t info=wait_event(P_PID,child,WEXITED|WNOWAIT);CHECK(info.si_pid==child);
        CHECK(pthread_barrier_init(&race_barrier,NULL,3)==0);
        struct race_result results[2]={{.kind=0,.pid=child},{.kind=1,.pid=child}};pthread_t t[2];
        CHECK(pthread_create(&t[0],NULL,consumer,&results[0])==0);CHECK(pthread_create(&t[1],NULL,consumer,&results[1])==0);
        pthread_barrier_wait(&race_barrier);CHECK(pthread_join(t[0],NULL)==0);CHECK(pthread_join(t[1],NULL)==0);
        int won=0,lost=0;
        for(int j=0;j<2;++j){if(results[j].result==child&&results[j].status==42)++won;else if(results[j].result==-1&&results[j].error==ECHILD)++lost;}
        CHECK(won==1&&lost==1);CHECK(pthread_barrier_destroy(&race_barrier)==0);
    }
    puts("WAITIDCHECK concurrent consumers done");
}
static void groups(void) {
    int a[2],b[2];pid_t ca=blocked_child(a,31);pid_t cb=blocked_child(b,32);
    CHECK(setpgid(ca,ca)==0);CHECK(setpgid(cb,cb)==0);release_child(b);release_child(a);
    siginfo_t info=wait_event(P_PGID,ca,WEXITED);CHECK(info.si_pid==ca&&info.si_status==31);
    int status;CHECK(waitpid(-cb,&status,0)==cb&&WIFEXITED(status)&&WEXITSTATUS(status)==32);
    pid_t c=blocked_child(a,33);release_child(a);info=wait_event(P_PGID,0,WEXITED);CHECK(info.si_pid==c&&info.si_status==33);
    puts("WAITIDCHECK process groups done");
}
static void stopped(int round) {
    int fds[2],finish[2];CHECK(pipe(fds)==0);CHECK(pipe(finish)==0);
    pid_t child=fork();CHECK(child>=0);
    if(!child) {
        close(fds[1]);close(finish[1]);char c;ssize_t n;
        do{n=read(fds[0],&c,1);}while(n<0&&errno==EINTR);
        if(n!=1)_exit(112);
        do{n=read(finish[0],&c,1);}while(n<0&&errno==EINTR);
        _exit(n==1?12:113);
    }
    close(fds[0]);close(finish[0]);CHECK(kill(child,SIGSTOP)==0);
    siginfo_t info=wait_event(P_PID,child,WSTOPPED|WNOWAIT);
    CHECK(info.si_pid==child&&info.si_code==CLD_STOPPED&&info.si_status==SIGSTOP);
    info=wait_event(P_PID,child,WSTOPPED|WNOWAIT|WNOHANG);CHECK(info.si_pid==child&&info.si_status==SIGSTOP);
    int status;CHECK(waitpid(child,&status,WUNTRACED|WNOHANG)==child&&WIFSTOPPED(status)&&WSTOPSIG(status)==SIGSTOP);
    // A readable pipe and an ignored signal must not resume a stopped job.
    // Delay alternate rounds to exercise both immediate and quiescent resume.
    if (round & 1) {
        release_child(fds);CHECK(kill(child,SIGWINCH)==0);usleep(2000);
        info=wait_event(P_PID,child,WEXITED|WNOHANG);CHECK(info.si_pid==0);
    }
    CHECK(kill(child,SIGCONT)==0);info=wait_event(P_PID,child,WCONTINUED|WNOWAIT);
    CHECK(info.si_pid==child&&info.si_code==CLD_CONTINUED&&info.si_status==SIGCONT);
    info=wait_event(P_PID,child,WCONTINUED);CHECK(info.si_pid==child&&info.si_status==SIGCONT);
    info=wait_event(P_PID,child,WCONTINUED|WNOHANG);CHECK(info.si_pid==0);
    if (!(round & 1)) {
        CHECK(kill(child,SIGCONT)==0);
        info=wait_event(P_PID,child,WCONTINUED|WNOHANG);CHECK(info.si_pid==0);
        release_child(fds);
    }
    release_child(finish);info=wait_event(P_PID,child,WEXITED);CHECK(info.si_status==12);
}
static void killed_waiter(void) {
    int gate[2],report[2];CHECK(pipe(gate)==0);CHECK(pipe(report)==0);
    pid_t controller=fork();CHECK(controller>=0);
    if(!controller) {
        close(gate[1]);close(report[0]);
        pid_t child=fork();if(child<0)_exit(90);
        if(!child) {
            char marker;ssize_t n;do{n=read(gate[0],&marker,1);}while(n<0&&errno==EINTR);
            pid_t parent=getppid();
            if(n==1)write(report[1],&parent,sizeof(parent));
            _exit(n==1?0:91);
        }
        close(gate[0]);
        if(write(report[1],&child,sizeof(child))!=sizeof(child))_exit(92);
        siginfo_t info;raw_wait(P_PID,child,&info,WEXITED);_exit(93);
    }
    close(gate[0]);close(report[1]);pid_t orphan=0;
    CHECK(read(report[0],&orphan,sizeof(orphan))==sizeof(orphan)&&orphan>0);
    usleep(20000);CHECK(kill(controller,SIGKILL)==0);
    int status;CHECK(waitpid(controller,&status,0)==controller&&WIFSIGNALED(status)&&WTERMSIG(status)==SIGKILL);
    CHECK(write(gate[1],"x",1)==1);close(gate[1]);
    pid_t new_parent=-1;CHECK(read(report[0],&new_parent,sizeof(new_parent))==sizeof(new_parent)&&new_parent!=controller);
    close(report[0]);siginfo_t info;errno=0;
    CHECK(raw_wait(P_PID,orphan,&info,WEXITED|WNOHANG)==-1&&errno==ECHILD);
    puts("WAITIDCHECK killed waiter/orphan done");
}
static volatile sig_atomic_t caught;
static pthread_t main_thread;
static void handler(int signal) { (void)signal;caught=1; }
static void *interrupter(void *unused) { (void)unused;usleep(20000);pthread_kill(main_thread,SIGUSR1);return NULL; }
static void interrupted(void) {
    struct sigaction action={.sa_handler=handler};sigemptyset(&action.sa_mask);CHECK(sigaction(SIGUSR1,&action,NULL)==0);
    int fds[2];pid_t child=blocked_child(fds,7);pthread_t t;main_thread=pthread_self();CHECK(pthread_create(&t,NULL,interrupter,NULL)==0);
    siginfo_t info;errno=0;CHECK(raw_wait(P_PID,child,&info,WEXITED)==-1&&errno==EINTR&&caught);
    CHECK(pthread_join(t,NULL)==0);release_child(fds);info=wait_event(P_PID,child,WEXITED);CHECK(info.si_pid==child&&info.si_status==7);
    puts("WAITIDCHECK EINTR done");
}
int main(void) {
    siginfo_t info;errno=0;int r=raw_wait(P_ALL,0,&info,WEXITED|WNOHANG);
    if(r==-1&&errno==ENOSYS){puts("WAITIDCHECK FAIL missing syscall 247");return 1;}
    basics();threads();races();groups();for(int i=0;i<32;++i)stopped(i);puts("WAITIDCHECK stop/continue done rounds=32");killed_waiter();interrupted();
    printf("WAITIDCHECK %s races=32 stops=32 failures=%d\n",failures?"FAIL":"PASS",failures);
    return failures?1:0;
}
