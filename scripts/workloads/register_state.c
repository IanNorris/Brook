#define _GNU_SOURCE
#include <stdint.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include <ucontext.h>

/* Link with register_state.S. No libc call occurs between load and snapshot. */
uint64_t register_actual_redzone[15];
uint64_t register_seed[15],register_actual[17],register_flags,register_expected_rsp;
unsigned char register_seed_fx[512] __attribute__((aligned(16)));
unsigned char register_actual_fx[512] __attribute__((aligned(16)));
extern void register_probe_syscall(void),register_probe_fault(void);
extern void register_probe_signal(void),register_probe_timer(void);
extern char register_syscall_resume[];
static const char* names[]={"rax","rbx","rcx","rdx","rsi","rdi","rbp","r8","r9","r10","r11","r12","r13","r14","r15"};
static const int indices[]={REG_RAX,REG_RBX,REG_RCX,REG_RDX,REG_RSI,REG_RDI,REG_RBP,REG_R8,REG_R9,REG_R10,REG_R11,REG_R12,REG_R13,REG_R14,REG_R15};
static unsigned passed,total;
static volatile sig_atomic_t signal_seen,signal_bad,signal_edit;
static uint64_t expected[15];
static void seed(void) {
    for(unsigned i=0;i<15;++i)register_seed[i]=0x1234567800000100ULL+17*i;
    memset(register_seed_fx,0,sizeof register_seed_fx);
    /* Empty x87 stack, nondefault rounding modes, distinct XMM lanes. */
    const uint16_t fcw=0x077f;const uint32_t mxcsr=0x3f80;
    memcpy(register_seed_fx,&fcw,2);memcpy(register_seed_fx+24,&mxcsr,4);
    for(unsigned i=0;i<256;++i)register_seed_fx[160+i]=(unsigned char)(i*13+37);
    register_flags=0xed7; /* IF, DF and known arithmetic flags. */
    memcpy(expected,register_seed,sizeof expected);
}
static void check(const char* label,uint64_t flags,int vector) {
    int ok=1;
    for(unsigned i=0;i<15;++i)if(register_actual[i]!=expected[i]) {
        printf("FAIL %s %s expected=%#llx actual=%#llx\n",label,names[i],(unsigned long long)expected[i],(unsigned long long)register_actual[i]);ok=0;
    }
    for(unsigned i=0;i<15;++i)if(register_actual_redzone[i]!=0x35792468) {printf("FAIL %s stack canary -%u\n",label,16+i*8);ok=0;}
    if(register_actual[15]!=register_expected_rsp) {printf("FAIL %s rsp\n",label);ok=0;}
    if((register_actual[16]&0xed7)!=(flags&0xed7)) {
        printf("FAIL %s flags expected=%#llx actual=%#llx\n",label,(unsigned long long)flags,(unsigned long long)register_actual[16]);ok=0;
    }
    if(vector&&(memcmp(register_seed_fx,register_actual_fx,2)||memcmp(register_seed_fx+24,register_actual_fx+24,4)||memcmp(register_seed_fx+160,register_actual_fx+160,256))) {
        printf("FAIL %s x87-control/MXCSR/XMM0-15\n",label);ok=0;
    }
    ++total;passed+=ok;printf("REGISTER_CASE %s %s\n",label,ok?"PASS":"FAIL");
}
static void trap(int sig,siginfo_t* info,void* context) {
    (void)info;
    ucontext_t* uc=context;
    uint64_t live_flags;__asm__ volatile("pushfq; pop %0":"=r"(live_flags));
    if(live_flags&0x400)signal_bad=1;
    if(sig!=SIGILL)_exit(92);
    for(unsigned i=0;i<15;++i) {
        if((uint64_t)uc->uc_mcontext.gregs[indices[i]]!=register_seed[i])signal_bad=1;
        if(signal_edit)uc->uc_mcontext.gregs[indices[i]]=(greg_t)expected[i];
    }
    if((uint64_t)uc->uc_mcontext.gregs[REG_RSP]!=register_expected_rsp)signal_bad=1;
    uc->uc_mcontext.gregs[REG_RIP]+=2;
    if(signal_edit)uc->uc_mcontext.gregs[REG_EFL]=0x202;
    ++signal_seen;
}
static void syscall_case(const char* label,long number,long result) {
    register_seed[0]=(uint64_t)number;memcpy(expected,register_seed,sizeof expected);
    expected[0]=(uint64_t)result;expected[2]=(uintptr_t)register_syscall_resume;expected[10]=register_flags;
    register_probe_syscall();check(label,register_flags,1);
}
int main(int argc,char** argv) {
    const int kernel=argc==2&&!strcmp(argv[1],"--kernel");
    const int denied=argc==2&&!strcmp(argv[1],"--kernel-denied");
    if(argc>1&&!kernel&&!denied)return 64;
    setvbuf(stdout,NULL,_IONBF,0);
    alarm(30);
    seed();syscall_case("getpid",SYS_getpid,getpid());
    seed();syscall_case("gettid",SYS_gettid,syscall(SYS_gettid));
    seed();syscall_case("ENOSYS",0x7fffffff,-38);
    seed();syscall_case("sched-yield",SYS_sched_yield,0);
    struct timespec delay={0,2000000};seed();register_seed[5]=(uintptr_t)&delay;register_seed[4]=0;
    syscall_case("blocking-nanosleep",SYS_nanosleep,0);
    seed();register_seed[5]=(uint64_t)-1;syscall_case("EBADF-close",SYS_close,-9);
    void* page=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(page==MAP_FAILED)return 2;
    seed();register_seed[5]=(uintptr_t)page;memcpy(expected,register_seed,sizeof expected);
    register_probe_fault();check("demand-page-fault",register_flags,1);
    if(*(volatile unsigned char*)page!=0x5a)return 3;
    munmap(page,4096);
    struct sigaction sa={0};sa.sa_sigaction=trap;sa.sa_flags=SA_SIGINFO;
    if(sigaction(SIGILL,&sa,NULL))return 4;
    for(int edit=0;edit<2;++edit) {
        seed();signal_edit=edit;signal_bad=0;signal_seen=0;
        if(edit)for(unsigned i=0;i<15;++i)expected[i]^=0x55aa55aa55aa55aaULL;
        register_probe_signal();check(edit?"signal-edited-GPRs":"signal-preserved-GPRs",edit?0x202:register_flags,0);
        if(signal_bad||signal_seen!=1)return 5;
    }
    /* Long no-syscall user interval exposes timer IRQ/context-switch returns.
       LOOP changes RCX only; it leaves the seeded arithmetic flags intact. */
    seed();register_seed[2]=50000000;memcpy(expected,register_seed,sizeof expected);expected[2]=0;
    register_probe_timer();check("timer-exposure",register_flags,1);
    if(kernel||denied) {
        errno=0;long result=syscall(0xb31,0ul,0ul,0ul,0ul,0ul,0ul);
        const int ok=kernel?result==4:(result==-1&&errno==EPERM);
        ++total;passed+=ok;
        printf("REGISTER_CASE %s %s result=%ld errno=%d\n",kernel?"kernel-entry":"kernel-permission",ok?"PASS":"FAIL",result,errno);
    }
    printf("REGISTER_RESULT %u/%u\n",passed,total);
    return passed==total?0:1;
}
