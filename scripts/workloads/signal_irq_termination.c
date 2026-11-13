/* Run only in a disposable test kernel with mapping test hooks enabled.
 * The parent kills the child while signal-frame usercopy is held. */
#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

enum { STACK_BYTES = 65536 };
struct shared_state { _Atomic int ready, sent, handler; };
static struct shared_state *shared;
static unsigned char *stack_memory;
static _Atomic int target_ready;
static pid_t target_tid, group_id;
static int request_ipi;

static long barrier(unsigned action, void *address, unsigned long length) {
    return syscall(0xb30, action, address, length, 4ul, 1ul, 0ul);
}
static void handler(int signal_number) {
    (void)signal_number;
    atomic_store(&shared->handler, 1);
}
static void *sender(void *unused) {
    (void)unused;
    while(!atomic_load_explicit(&target_ready,memory_order_acquire)) {}
    if(syscall(SYS_tgkill,group_id,target_tid,SIGUSR1))_exit(20);
    atomic_store_explicit(&shared->sent,1,memory_order_release);
    if(request_ipi) {
        for(unsigned i=0;i<1000;++i) {
            (void)syscall(0xb31,1,(long)target_tid,0,0,0,0);
            usleep(1000);
        }
    }
    return NULL;
}
static void child(void) {
    sigset_t blocked;sigemptyset(&blocked);sigaddset(&blocked,SIGUSR1);
    if(pthread_sigmask(SIG_BLOCK,&blocked,NULL))_exit(10);
    stack_t alternate={.ss_sp=stack_memory,.ss_size=STACK_BYTES};
    if(sigaltstack(&alternate,NULL))_exit(11);
    struct sigaction action={0};
    action.sa_handler=handler;action.sa_flags=SA_ONSTACK;sigemptyset(&action.sa_mask);
    if(sigaction(SIGUSR1,&action,NULL))_exit(12);
    group_id=getpid();target_tid=(pid_t)syscall(SYS_gettid);
    pthread_t thread;if(pthread_create(&thread,NULL,sender,NULL))_exit(13);
    if(barrier(0,stack_memory,STACK_BYTES))_exit(14);
    if(pthread_sigmask(SIG_UNBLOCK,&blocked,NULL))_exit(15);
    atomic_store_explicit(&shared->ready,1,memory_order_release);
    atomic_store_explicit(&target_ready,1,memory_order_release);
    for(;;) __asm__ volatile("pause");
}
int main(int argc,char **argv) {
    if(argc>2 || (argc==2 && strcmp(argv[1],"--ipi")))return 2;
    request_ipi=argc==2;
    shared=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);
    if(shared==MAP_FAILED)return 1;
    stack_memory=mmap(NULL,STACK_BYTES,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(stack_memory==MAP_FAILED)return 1;
    memset(stack_memory,0x5a,STACK_BYTES);
    pid_t pid=fork();if(pid<0)return 2;if(!pid)child();
    int status=0,armed=0,killed=0,early=0,still_waiting=0;
    for(unsigned i=0;i<5000;++i) {
        long state=barrier(1,NULL,0);
        if(state<0)break;
        if(((unsigned long)state>>16)&0xffff) {armed=1;break;}
        if(waitpid(pid,&status,WNOHANG)==pid) {early=1;break;}
        usleep(1000);
    }
    if(armed) {
        killed=kill(pid,SIGKILL)==0;
        early=waitpid(pid,&status,WNOHANG)==pid;
        long state=barrier(1,NULL,0);
        still_waiting=state>=0 && (((unsigned long)state>>16)&0xffff)==1 &&
            !((unsigned long)state>>32);
    }
    long released=barrier(2,NULL,0);
    if(!armed && !early)(void)kill(pid,SIGKILL);
    if(!early && waitpid(pid,&status,0)!=pid)return 3;
    long final_state=barrier(1,NULL,0);
    int stack_intact=1;
    for(unsigned i=0;i<STACK_BYTES;++i)if(stack_memory[i]!=0x5a)stack_intact=0;
    stack_memory[STACK_BYTES-1]=0xa5;
    stack_intact=stack_intact && stack_memory[STACK_BYTES-1]==0xa5;
    int ok=armed && killed && !early && still_waiting && released==0 && final_state>=0 &&
        !(((unsigned long)final_state>>16)&0xffff) && stack_intact && WIFSIGNALED(status) &&
        WTERMSIG(status)==SIGKILL && !atomic_load(&shared->handler);
    printf("SIGNAL_IRQ_TERMINATION_%s barrier=%d killed=%d early=%d waiting=%d released=%ld status=%d sent=%d handler=%d stack=%d\n",
           ok?"PASS":"FAIL",armed,killed,early,still_waiting,released,status,atomic_load(&shared->sent),atomic_load(&shared->handler),stack_intact);
    return ok?0:4;
}
