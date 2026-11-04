/* Test pending caught-signal delivery without a syscall in the target loop.
 * Run with a process-group timeout: the kernel under test may hang or fail to
 * deliver. The sender is a separate thread; SIGUSR1 is blocked there. */
#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdatomic.h>
#include <sys/syscall.h>
#include <unistd.h>

static _Atomic int started,send_result;
static _Thread_local volatile sig_atomic_t caught;
static pid_t process_id,target_tid;
static void handler(int sig) {(void)sig;caught=1;}
static void *sender(void *unused) {
    (void)unused;sigset_t mask;sigemptyset(&mask);sigaddset(&mask,SIGUSR1);
    if(pthread_sigmask(SIG_BLOCK,&mask,NULL)){atomic_store(&send_result,-1);return NULL;}
    while(!atomic_load_explicit(&started,memory_order_acquire)) {}
    int result=(int)syscall(SYS_tgkill,process_id,target_tid,SIGUSR1);
    atomic_store_explicit(&send_result,result?-1:1,memory_order_release);return NULL;
}
int main(void) {
    sigset_t mask;sigemptyset(&mask);sigaddset(&mask,SIGUSR1);
    if(pthread_sigmask(SIG_BLOCK,&mask,NULL))return 1;
    struct sigaction action={0};action.sa_handler=handler;sigemptyset(&action.sa_mask);
    if(sigaction(SIGUSR1,&action,NULL))return 1;
    process_id=getpid();target_tid=(pid_t)syscall(SYS_gettid);
    pthread_t thread;if(pthread_create(&thread,NULL,sender,NULL))return 2;
    if(pthread_sigmask(SIG_UNBLOCK,&mask,NULL))return 2;
    atomic_store_explicit(&started,1,memory_order_release);
    for(volatile unsigned long i=0;!caught && atomic_load_explicit(&send_result,memory_order_acquire)!=-1 && i<2000000000UL;++i) {}
    int delivered=caught;pthread_join(thread,NULL);
    if(!delivered || atomic_load(&send_result)!=1){puts("SIGNAL_IRQ_FAIL");return 3;}
    puts("SIGNAL_IRQ_PASS caught-target-no-syscall");return 0;
}
