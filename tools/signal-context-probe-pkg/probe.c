#define _GNU_SOURCE
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

extern char signal_probe_fault[], signal_probe_resume[];
struct observation { uint64_t cs, ss, rsp, expected_rsp, rip, flags; int seen, errors; };
static struct observation observations[3];
static volatile sig_atomic_t current;
static uintptr_t expected_rsp;
static unsigned char alternate[65536] __attribute__((aligned(16)));
static uint16_t live_cs, live_ss;

static void handler(int sig, siginfo_t *info, void *opaque) {
    ucontext_t *uc=opaque;
    unsigned index=(unsigned)current;
    if (index>=3) _exit(98);
    struct observation *o=&observations[index];
    uint64_t packed=(uint64_t)uc->uc_mcontext.gregs[REG_CSGSFS];
    o->cs=packed&65535; o->ss=packed>>48;
    o->rsp=(uint64_t)uc->uc_mcontext.gregs[REG_RSP]; o->expected_rsp=expected_rsp;
    o->rip=(uint64_t)uc->uc_mcontext.gregs[REG_RIP]; o->flags=uc->uc_flags;
    o->seen++;
    if (o->cs!=live_cs) o->errors|=1;
    if (o->ss!=live_ss) o->errors|=2;
    if (o->rsp!=expected_rsp) o->errors|=4;
    if (sig==SIGSEGV) {
        if (info->si_addr || o->rip!=(uintptr_t)signal_probe_fault) _exit(97);
        if (index==2 && ((uintptr_t)&opaque<(uintptr_t)alternate ||
            (uintptr_t)&opaque>=(uintptr_t)alternate+sizeof(alternate))) o->errors|=8;
        uc->uc_mcontext.gregs[REG_RIP]=(greg_t)(uintptr_t)signal_probe_resume;
    } else if (sig!=SIGUSR1 || index!=0) _exit(96);
}

__attribute__((noinline)) static void fault(void) {
    __asm__ volatile("movq %%rsp,%0\n\txorl %%eax,%%eax\n\t"
        ".global signal_probe_fault\nsignal_probe_fault:\n\tmovl (%%rax),%%eax\n\t"
        ".global signal_probe_resume\nsignal_probe_resume:\n\t"
        : "=m"(expected_rsp) : : "rax","memory");
}

int main(void) {
    __asm__ volatile("movw %%cs,%0;movw %%ss,%1":"=r"(live_cs),"=r"(live_ss));
    struct sigaction sa={0};sa.sa_sigaction=handler;sa.sa_flags=SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGUSR1,&sa,0) || sigaction(SIGSEGV,&sa,0)) return 90;
    long pid=getpid(),tid=syscall(SYS_gettid),nr;
    __asm__ volatile("movq %%rsp,%0\n\tsyscall":"=m"(expected_rsp),"=a"(nr)
        :"a"((long)SYS_tgkill),"D"(pid),"S"(tid),"d"((long)SIGUSR1):"rcx","r11","memory");
    if (nr) return 91;
    current=1;fault();
    stack_t stack={.ss_sp=alternate,.ss_size=sizeof(alternate)};
    if (sigaltstack(&stack,0)) return 92;
    sa.sa_flags=SA_SIGINFO|SA_ONSTACK;if (sigaction(SIGSEGV,&sa,0)) return 93;
    current=2;fault();
    int failed=0;
    printf("{\"live_cs\":%u,\"live_ss\":%u,\"cases\":[",live_cs,live_ss);
    for (unsigned i=0;i<3;i++) {
        struct observation *o=&observations[i];failed|=o->seen!=1 || o->errors;
        printf("%s{\"case\":%u,\"seen\":%d,\"cs\":%lu,\"ss\":%lu,\"rsp\":%lu,\"expected_rsp\":%lu,\"flags\":%lu,\"errors\":%d}",
            i?",":"",i,o->seen,o->cs,o->ss,o->rsp,o->expected_rsp,o->flags,o->errors);
    }
    printf("],\"passed\":%s}\n",failed?"false":"true");return failed?1:0;
}
