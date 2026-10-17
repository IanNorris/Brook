#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

enum { WORKER_SEGV, WORKER_ILL, WORKER_FPE, LEADER_SEGV, HANDLED_SEGV,
       EXITED_LEADER_SEGV, SIGWAIT_LEADER_SEGV, SIGWAIT_WORKER_SEGV,
       SIGWAIT_GROUP_KILL, CASES };
static const char *names[]={"worker-segv","worker-ill","worker-fpe",
    "leader-segv","handled-worker-segv","exited-leader-worker-segv",
    "sigwait-leader-worker-segv","sigwait-peer-worker-segv","sigwait-group-kill"};
static int mode,go[2],report[2],life[2],hold[2],word;
static unsigned ready;
static pid_t parent_pid;
static volatile sig_atomic_t handled,wait_timed_out;
static void wait_alarm(int signal) {(void)signal;wait_timed_out=1;}
static uint64_t ms(void) {
    struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;
}
static void pause_ms(unsigned n) {
    struct timespec t={n/1000,(n%1000)*1000000};
    while(nanosleep(&t,&t)<0 && errno==EINTR){}
}
static void wait_for_parent(void) {
    char path[64];snprintf(path,sizeof(path),"/proc/%d/status",parent_pid);
    uint64_t until=ms()+1500;
    do {
        FILE *f=fopen(path,"r");char line[256],state=0;
        if(f) {
            while(fgets(line,sizeof(line),f))
                if(sscanf(line,"State: %c",&state)==1)break;
            fclose(f);
        }
        if(state=='S')return;
        sched_yield();
    }while(ms()<until);
    _exit(101);
}
static void fault(void) {
    if(mode==WORKER_ILL)__asm__ volatile("ud2");
    else if(mode==WORKER_FPE)
        __asm__ volatile("mov $1,%%eax;xor %%edx,%%edx;xor %%ecx,%%ecx;div %%ecx" :::"rax","rcx","rdx","cc");
    else __asm__ volatile("xor %%eax,%%eax;movq (%%rax),%%rdx" :::"rax","rdx","memory");
}
static void recover(int signal,siginfo_t *info,void *context) {
    (void)info;
    if(signal!=SIGSEGV)_exit(91);
    ++handled;
    /* Skip precisely the three-byte movq load in fault(), not arbitrary code. */
    ((ucontext_t*)context)->uc_mcontext.gregs[REG_RIP]+=3;
}
static void signal_wait(void) {
    uint64_t mask=1ULL<<(SIGUSR1-1);
    syscall(SYS_rt_sigtimedwait,&mask,NULL,NULL,8);
    _exit(102); // No requested signal is sent; only fatal cancellation may end it.
}
static void *worker(void *arg) {
    uintptr_t id=(uintptr_t)arg;
    __atomic_add_fetch(&ready,1,__ATOMIC_RELEASE);
    if(id==2 && mode!=LEADER_SEGV && mode!=SIGWAIT_GROUP_KILL) {
        char c;if(read(go[0],&c,1)!=1)_exit(92);
        wait_for_parent();
        fault();
        if(mode!=HANDLED_SEGV || handled!=1)_exit(93);
        if(write(report[1],"H",1)!=1)_exit(94);
    }
    if(mode==SIGWAIT_GROUP_KILL || (id==0 && mode==SIGWAIT_WORKER_SEGV))signal_wait();
    if(id==1) {char c;for(;;)read(hold[0],&c,1);}
    for(;;)syscall(SYS_futex,&word,128,0,NULL,NULL,0);
    return NULL;
}
static void child(void) {
    close(go[1]);close(report[0]);close(life[0]);
    if(mode>=SIGWAIT_LEADER_SEGV) {
        sigset_t mask;sigemptyset(&mask);sigaddset(&mask,SIGUSR1);
        if(pthread_sigmask(SIG_BLOCK,&mask,NULL))_exit(103);
    }
    if(mode==HANDLED_SEGV) {
        struct sigaction a={0};a.sa_sigaction=recover;a.sa_flags=SA_SIGINFO;
        sigemptyset(&a.sa_mask);if(sigaction(SIGSEGV,&a,NULL))_exit(95);
    }
    pthread_t threads[3];ready=0;
    for(uintptr_t i=0;i<3;i++)if(pthread_create(&threads[i],NULL,worker,(void*)i))_exit(96);
    while(__atomic_load_n(&ready,__ATOMIC_ACQUIRE)!=3)sched_yield();
    if(write(report[1],"R",1)!=1)_exit(97);
    if(mode==EXITED_LEADER_SEGV){syscall(SYS_exit,0);_exit(98);}
    if(mode==SIGWAIT_LEADER_SEGV || mode==SIGWAIT_GROUP_KILL)signal_wait();
    if(mode==LEADER_SEGV) {char c;if(read(go[0],&c,1)!=1)_exit(99);wait_for_parent();fault();_exit(100);}
    for(;;)syscall(SYS_futex,&word,128,0,NULL,NULL,0);
}
static int byte_with_timeout(int fd,int expected) {
    uint64_t until=ms()+4000;
    for(;;) {
        uint64_t now=ms();if(now>=until)return 0;
        struct pollfd p={fd,POLLIN,0};char c=0;
        int result=poll(&p,1,(int)(until-now));
        if(result<0 && errno==EINTR)continue;
        if(result!=1)return 0;
        ssize_t got=read(fd,&c,1);
        if(got<0 && errno==EINTR)continue;
        return expected<0?got==0:got==1 && c==expected;
    }
    return 0;
}
static int wait_bounded(pid_t pid,int *status,unsigned timeout) {
    uint64_t until=ms()+timeout;
    do {
        pid_t got=waitpid(pid,status,WNOHANG);
        if(got==pid)return 1;
        if(got<0 && errno!=EINTR)return 0;
        pause_ms(2);
    }while(ms()<until);
    return 0;
}
static int wait_blocking(pid_t pid,int *status) {
    /* A real blocked parent must wake on group death. The non-restarting
     * alarm bounds failure without turning the acceptance path into polling. */
    wait_timed_out=0;
    if(syscall(SYS_alarm,3)<0)return 0;
    pid_t got;
    do {got=waitpid(pid,status,0);} while(got<0 && errno==EINTR && !wait_timed_out);
    syscall(SYS_alarm,0);
    return got==pid;
}
static int run_case(int which) {
    mode=which;handled=word=0;
    parent_pid=getpid();
    if(pipe(go)||pipe(report)||pipe(life)||pipe(hold))return 0;
    pid_t pid=fork();if(!pid)child();if(pid<0)return 0;
    close(go[0]);close(report[1]);close(life[1]);close(hold[0]);close(hold[1]);
    const char *stage="ready";
    int ok=byte_with_timeout(report[0],'R'),status=0,reaped=0;
    pause_ms(30); // Let the leader and ordinary peer waits settle.
    if(ok){stage="trigger";ok=mode==SIGWAIT_GROUP_KILL?kill(pid,SIGKILL)==0:write(go[1],"G",1)==1;}
    if(ok && mode==HANDLED_SEGV) {
        stage="handled";ok=byte_with_timeout(report[0],'H') && waitpid(pid,&status,WNOHANG)==0;
        kill(pid,SIGKILL);
    }
    if(ok){stage="wait";reaped=wait_blocking(pid,&status);}
    ok=ok && reaped;
    if(ok) {
        stage="signal-status";
        int expected=mode==WORKER_ILL?SIGILL:mode==WORKER_FPE?SIGFPE:
            (mode==HANDLED_SEGV || mode==SIGWAIT_GROUP_KILL)?SIGKILL:SIGSEGV;
        ok=WIFSIGNALED(status) && WTERMSIG(status)==expected;
    }
    int observed_status=status,saved_errno=errno;
    if(!reaped) {
        kill(pid,SIGKILL);
        if(!wait_bounded(pid,&status,3000)) {
            printf("FAULTGROUPCHECK cleanup failed pid=%d\n",pid);return -1;
        }
    }
    if(ok){stage="pipe-eof";ok=byte_with_timeout(life[0],-1);}
    close(go[1]);close(report[0]);close(life[0]);
    printf("FAULTGROUPCHECK %s %s status=%d observed_status=%d reaped=%d stage=%s errno=%d\n",
           ok?"PASS":"FAIL",names[which],status,observed_status,reaped,stage,saved_errno);
    return ok;
}
int main(int argc,char **argv) {
    unsigned rounds=1;if(argc>2)return 2;
    if(argc==2) {char *end;long n=strtol(argv[1],&end,10);if(!*argv[1]||*end||n<1||n>16)return 2;rounds=n;}
    struct rlimit limit={0,0};setrlimit(RLIMIT_CORE,&limit);
    struct sigaction a={0};a.sa_handler=wait_alarm;sigemptyset(&a.sa_mask);
    if(sigaction(SIGALRM,&a,NULL))return 2;
    signal(SIGPIPE,SIG_IGN);setvbuf(stdout,NULL,_IONBF,0);
    unsigned failures=0;
    for(unsigned r=0;r<rounds;r++)for(int c=0;c<CASES;c++) {
        int result=run_case(c);if(result<0)return 1;if(!result)++failures;
    }
    printf("FAULTGROUPCHECK %s cases=%u failures=%u\n",failures?"FAIL":"PASS",CASES*rounds,failures);
    return failures?1:0;
}
