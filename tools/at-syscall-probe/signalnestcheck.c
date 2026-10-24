#define _GNU_SOURCE
#include <signal.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <ucontext.h>
#include <unistd.h>

static char alternate[65536] __attribute__((aligned(64)));
static sigjmp_buf recovery;
static volatile sig_atomic_t jumps,stage,valid;
static uintptr_t outer_sp;
static uint32_t initial=0x3f80,outer=0x5f80;
static int mask_has(int signal) {sigset_t mask;return sigprocmask(SIG_SETMASK,NULL,&mask)==0 && sigismember(&mask,signal)==1;}
static int on_alt(uintptr_t p) {return p>(uintptr_t)alternate && p<(uintptr_t)alternate+sizeof(alternate);}
static void longjump_handler(int signal) {(void)signal;++jumps;siglongjmp(recovery,1);}
static void nested(int signal,siginfo_t* info,void* context) {
    (void)info;
    char local;uint32_t control;
    __asm__ volatile("stmxcsr %0":"=m"(control));
    if(control!=0x1f80 || !on_alt((uintptr_t)&local) || !mask_has(SIGUSR1))valid=0;
    if(((ucontext_t*)context)->uc_stack.ss_flags!=0)valid=0;
    if(signal==SIGUSR2) {
        if(stage!=1 || !mask_has(SIGUSR2) || (uintptr_t)&local>=outer_sp)valid=0;
        stage=2;
        // Inner live FP state must not leak into the outer handler.
        __asm__ volatile("ldmxcsr %0"::"m"(initial));
    } else {
        if(stage || mask_has(SIGUSR2))valid=0;
        stage=1;outer_sp=(uintptr_t)&local;
        __asm__ volatile("ldmxcsr %0"::"m"(outer));
        if(raise(SIGUSR2))valid=0;
        __asm__ volatile("stmxcsr %0":"=m"(control));
        if(stage!=2 || control!=outer || mask_has(SIGUSR2) || !mask_has(SIGUSR1))valid=0;
        stage=3;
    }
}
static void nodefer(int signal,siginfo_t* info,void* context) {
    (void)signal;(void)info;
    if(mask_has(SIGILL))valid=0;
    if(stage==0) {stage=1;__asm__ volatile("ud2");if(stage!=2)valid=0;stage=3;}
    else if(stage==1)stage=2;
    else valid=0;
    ((ucontext_t*)context)->uc_mcontext.gregs[REG_RIP]+=2;
}
static void forbidden(int signal) {(void)signal;_exit(99);}
static void overflow(int signal) {
    (void)signal;
    volatile char consume[32];consume[0]=1;
    __asm__ volatile("ud2");
    if(consume[0]!=1)_exit(98);
}
static int dies(pid_t child,int sig) {int status;return child>0 && waitpid(child,&status,0)==child && WIFSIGNALED(status) && WTERMSIG(status)==sig;}
#define CHECK(test,name) do { if(!(test)) {printf("{\"signal_nested_pass\":false,\"check\":\"%s\"}\n",name);return 1;} } while(0)
int main(void) {
    alarm(15);
    stack_t ss={.ss_sp=alternate,.ss_size=sizeof(alternate)};
    CHECK(sigaltstack(&ss,NULL)==0,"install altstack");
    struct sigaction jump={.sa_handler=longjump_handler,.sa_flags=SA_ONSTACK};
    sigemptyset(&jump.sa_mask);CHECK(sigaction(SIGSEGV,&jump,NULL)==0,"jump sigaction");
    for(volatile int n=0;n<2;++n)if(sigsetjmp(recovery,1)==0)
        __asm__ volatile("xor %%rax,%%rax; mov (%%rax),%%rax":::"rax","memory");
    CHECK(jumps==2 && !mask_has(SIGSEGV),"two synchronous nonlocal returns");
    struct sigaction sa={.sa_sigaction=nested,.sa_flags=SA_SIGINFO|SA_ONSTACK};
    sigemptyset(&sa.sa_mask);
    CHECK(sigaction(SIGUSR1,&sa,NULL)==0 && sigaction(SIGUSR2,&sa,NULL)==0,"nested sigaction");
    stage=0;valid=1;
    __asm__ volatile("ldmxcsr %0"::"m"(initial));
    CHECK(raise(SIGUSR1)==0,"raise outer");
    uint32_t control;__asm__ volatile("stmxcsr %0":"=m"(control));
    CHECK(stage==3 && valid && control==initial && !mask_has(SIGUSR1) && !mask_has(SIGUSR2),"nested masks stacks and FP");
    sa.sa_sigaction=nodefer;sa.sa_flags|=SA_NODEFER;
    CHECK(sigaction(SIGILL,&sa,NULL)==0,"nodefer sigaction");
    stage=0;valid=1;__asm__ volatile("ud2");
    CHECK(stage==3 && valid && !mask_has(SIGILL),"same synchronous signal SA_NODEFER");
    pid_t child=fork();
    if(child==0) {struct sigaction bad={.sa_handler=forbidden};sigemptyset(&bad.sa_mask);sigaction(SIGILL,&bad,NULL);sigset_t set;sigemptyset(&set);sigaddset(&set,SIGILL);sigprocmask(SIG_BLOCK,&set,NULL);__asm__ volatile("ud2");_exit(97);}
    CHECK(dies(child,SIGILL),"blocked synchronous fault is fatal");
    child=fork();
    if(child==0) {signal(SIGSEGV,SIG_DFL);struct sigaction recurse={.sa_handler=overflow,.sa_flags=SA_ONSTACK|SA_NODEFER};sigemptyset(&recurse.sa_mask);sigaction(SIGILL,&recurse,NULL);void* mem=mmap(NULL,24576,PROT_READ|PROT_WRITE,MAP_ANONYMOUS|MAP_PRIVATE,-1,0);if(mem==MAP_FAILED)_exit(96);stack_t small={.ss_sp=(char*)mem+8192,.ss_size=8192};if(sigaltstack(&small,NULL))_exit(95);__asm__ volatile("ud2");_exit(94);}
    CHECK(dies(child,SIGSEGV),"nested alternate-stack overflow");
    alarm(0);
    puts("{\"signal_nested_pass\":true,\"cases\":5,\"scope\":\"nonlocal return, nested FP/masks/altstack, SA_NODEFER, blocked fault, altstack overflow\"}");return 0;
}
