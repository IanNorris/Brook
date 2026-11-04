/* IRQ-return signal ABI check. Run only in a disposable VM or as an ordinary
 * Linux process, with an external process-group timeout. Requires AVX. */
#define _GNU_SOURCE
#include <pthread.h>
#include <errno.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

extern void irq_test_spin(_Atomic int*,uint64_t*,const uint32_t*);
extern char irq_test_loop[],irq_test_return[];
__asm__(
".text\n"
".global irq_test_spin,irq_test_loop,irq_test_return\n"
".type irq_test_spin,@function\n"
"irq_test_spin:\n"
"push %r12\n"
"mov %rsi,%r12\n"
"movabs $0x13579bdf2468ace0,%rax\n"
"movabs $0x123456789abcdef0,%rcx\n"
"movabs $0x0fedcba987654321,%r11\n"
"movabs $0x1122334455667788,%r10\n"
"mov %r10,-128(%rsp)\n"
"vmovdqu (%rdx),%ymm15\n"
"stc\n"
"movl $1,(%rdi)\n"
"irq_test_loop: jmp irq_test_loop\n"
"irq_test_return:\n"
"mov %rax,0(%r12)\n"
"mov %rcx,8(%r12)\n"
"mov %r11,16(%r12)\n"
"pushfq\n popq 24(%r12)\n"
"mov -128(%rsp),%r10\n mov %r10,32(%r12)\n"
"vmovdqu %ymm15,40(%r12)\n"
"vzeroupper\n pop %r12\n ret\n"
".size irq_test_spin,.-irq_test_spin\n");

static _Atomic int started,send_result;
static _Atomic int acknowledged;
_Static_assert(ATOMIC_INT_LOCK_FREE==2,"handler acknowledgement must be lock-free");
static int request_ipi;
static volatile sig_atomic_t seen,context_ok,on_alt;
static pid_t process_id,target_tid;
static uint8_t alt_stack[65536] __attribute__((aligned(16)));
static const uint32_t seed[8] __attribute__((aligned(32)))={1,2,3,4,5,6,7,8};
static void handler(int sig,siginfo_t *info,void *opaque) {
    volatile char marker;ucontext_t *context=opaque;greg_t *g=context->uc_mcontext.gregs;
    on_alt=(uintptr_t)&marker>=(uintptr_t)alt_stack && (uintptr_t)&marker<(uintptr_t)alt_stack+sizeof(alt_stack);
    context_ok=sig==SIGUSR1 && info && info->si_signo==SIGUSR1 &&
        (uintptr_t)g[REG_RIP]==(uintptr_t)irq_test_loop &&
        (uint64_t)g[REG_RAX]==UINT64_C(0x13579bdf2468ace0) &&
        (uint64_t)g[REG_RCX]==UINT64_C(0x123456789abcdef0) &&
        (uint64_t)g[REG_R11]==UINT64_C(0x0fedcba987654321) && (g[REG_EFL]&1);
    g[REG_RIP]=(greg_t)(uintptr_t)irq_test_return;
    __asm__ volatile("vxorps %%ymm15,%%ymm15,%%ymm15":::"ymm15");seen=1;
    atomic_store_explicit(&acknowledged,1,memory_order_release);
}
static void *sender(void *unused) {
    (void)unused;while(!atomic_load_explicit(&started,memory_order_acquire)) {}
    int result=(int)syscall(SYS_tgkill,process_id,target_tid,SIGUSR1);
    if(result){dprintf(2,"SIGNAL_IRQ_IPI_FAIL tgkill errno=%d\n",errno);kill(process_id,SIGKILL);_exit(8);}
    if(!result && request_ipi) {
        // A successful IPI can target a CPU the thread has since left. Retry
        // until the handler acknowledges, with a bounded, paced test loop.
        int submitted=0;
        for(unsigned i=0;i<1000;++i) {
            result=(int)syscall(0xb31,1,(long)target_tid,0,0,0,0);
            if(result && errno!=EAGAIN){dprintf(2,"SIGNAL_IRQ_IPI_FAIL syscall errno=%d\n",errno);kill(process_id,SIGKILL);_exit(8);}
            if(!result)submitted=1;
            if(submitted && atomic_load_explicit(&acknowledged,memory_order_acquire))break;
            struct timespec delay={0,1000000};nanosleep(&delay,NULL);
        }
        if(!submitted || !atomic_load_explicit(&acknowledged,memory_order_acquire)) {
            dprintf(2,"SIGNAL_IRQ_IPI_FAIL no acknowledgement/submission\n");kill(process_id,SIGKILL);_exit(9);
        }
        result=0;
    }
    atomic_store_explicit(&send_result,result?-1:1,memory_order_release);return NULL;
}
int main(int argc,char **argv) {
    if(argc>2 || (argc==2 && strcmp(argv[1],"--ipi")))return 1;
    request_ipi=argc==2;
    sigset_t mask;sigemptyset(&mask);sigaddset(&mask,SIGUSR1);
    if(pthread_sigmask(SIG_BLOCK,&mask,NULL))return 1;
    stack_t ss={.ss_sp=alt_stack,.ss_size=sizeof(alt_stack)};if(sigaltstack(&ss,NULL))return 2;
    process_id=getpid();target_tid=(pid_t)syscall(SYS_gettid);
    for(int alternate=0;alternate<2;++alternate) {
        struct sigaction action={0};action.sa_sigaction=handler;action.sa_flags=SA_SIGINFO|(alternate?SA_ONSTACK:0);
        sigemptyset(&action.sa_mask);if(sigaction(SIGUSR1,&action,NULL))return 3;
        atomic_store(&started,0);atomic_store(&send_result,0);atomic_store(&acknowledged,0);seen=context_ok=on_alt=0;
        pthread_t thread;if(pthread_create(&thread,NULL,sender,NULL))return 4;
        if(pthread_sigmask(SIG_UNBLOCK,&mask,NULL))return 5;
        uint64_t output[9]={0};irq_test_spin(&started,output,seed);
        /* Keep the target runnable until the sender has submitted an IPI even
         * if an unrelated reschedule interrupt delivered the signal first. */
        while(request_ipi && !atomic_load_explicit(&send_result,memory_order_acquire)) {}
        if(pthread_sigmask(SIG_BLOCK,&mask,NULL) || pthread_join(thread,NULL))return 6;
        if(!seen || !context_ok || on_alt!=alternate || atomic_load(&send_result)!=1 ||
           output[0]!=UINT64_C(0x13579bdf2468ace0) || output[1]!=UINT64_C(0x123456789abcdef0) ||
           output[2]!=UINT64_C(0x0fedcba987654321) || !(output[3]&1) ||
           output[4]!=UINT64_C(0x1122334455667788) || memcmp(&output[5],seed,sizeof(seed))) {
            printf("SIGNAL_IRQ_REGISTERS_FAIL alt=%d seen=%d context=%d on_alt=%d\n",alternate,seen,context_ok,on_alt);return 7;
        }
    }
    puts("SIGNAL_IRQ_REGISTERS_PASS gpr/flags/redzone/ymm/altstack/context-edit");return 0;
}
