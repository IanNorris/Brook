/* Disposable guest only: tests COW and a deliberately unwritable signal stack.
 * An external process-group timeout must terminate the run if a child hangs. */
#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

enum { STACK_BYTES=65536 };
static unsigned char *stack_memory;
static _Atomic int *progress;
static _Atomic int started,send_result;
static volatile sig_atomic_t caught,on_stack;
static pid_t process_id,target_tid;
static void handler(int sig) {
    volatile char local;(void)sig;
    on_stack=(uintptr_t)&local>=(uintptr_t)stack_memory && (uintptr_t)&local<(uintptr_t)stack_memory+STACK_BYTES;
    atomic_store_explicit(progress,3,memory_order_release);
    caught=1;
}
static void *sender(void *unused) {
    (void)unused;while(!atomic_load_explicit(&started,memory_order_acquire)) {}
    atomic_store_explicit(progress,2,memory_order_release);
    const int result=(int)syscall(SYS_tgkill,process_id,target_tid,SIGUSR1);
    atomic_store_explicit(&send_result,result?-1:1,memory_order_release);return NULL;
}
static void child(int inaccessible) {
    sigset_t mask;sigemptyset(&mask);sigaddset(&mask,SIGUSR1);
    if(pthread_sigmask(SIG_BLOCK,&mask,NULL))_exit(10);
    if(inaccessible && mprotect(stack_memory,STACK_BYTES,PROT_READ))_exit(11);
    stack_t stack={.ss_sp=stack_memory,.ss_size=STACK_BYTES};if(sigaltstack(&stack,NULL))_exit(12);
    struct sigaction action={0};action.sa_handler=handler;action.sa_flags=SA_ONSTACK;
    sigemptyset(&action.sa_mask);if(sigaction(SIGUSR1,&action,NULL))_exit(13);
    process_id=getpid();target_tid=(pid_t)syscall(SYS_gettid);pthread_t thread;
    if(pthread_create(&thread,NULL,sender,NULL) || pthread_sigmask(SIG_UNBLOCK,&mask,NULL))_exit(14);
    atomic_store_explicit(&started,1,memory_order_release);
    for(volatile unsigned long i=0;!caught && atomic_load_explicit(&send_result,memory_order_acquire)!=-1 && i<2000000000UL;++i) {}
    if(!caught)_exit(16); // do not let pthread_join deliver a pending signal
    if(pthread_join(thread,NULL))_exit(15);
    _exit(caught && on_stack && atomic_load(&send_result)==1?0:16);
}
int main(void) {
    stack_memory=mmap(NULL,STACK_BYTES,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(stack_memory==MAP_FAILED)return 1;
    progress=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);
    if(progress==MAP_FAILED)return 1;
    memset(stack_memory,0x5a,STACK_BYTES);
    for(int inaccessible=0;inaccessible<2;++inaccessible) {
        atomic_store(progress,0);pid_t pid=fork();if(pid<0)return 2;if(!pid)child(inaccessible);
        int status;if(waitpid(pid,&status,0)!=pid)return 3;
        const int ok=inaccessible?(WIFSIGNALED(status) && WTERMSIG(status)==SIGSEGV):
            (WIFEXITED(status) && WEXITSTATUS(status)==0);
        const int stage=atomic_load(progress);
        if(!ok || (inaccessible?stage!=2:stage!=3)) {printf("SIGNAL_IRQ_STACK_FAIL inaccessible=%d status=%d stage=%d\n",inaccessible,status,stage);return 4;}
        for(unsigned i=0;i<STACK_BYTES;++i)if(stack_memory[i]!=0x5a)return 5;
        volatile unsigned char *top=&stack_memory[STACK_BYTES-1];
        *top=0xa5;if(*top!=0xa5)return 6;*top=0x5a;
    }
    munmap(progress,4096);munmap(stack_memory,STACK_BYTES);puts("SIGNAL_IRQ_STACK_PASS private-isolation/readonly/parent-survived");return 0;
}
