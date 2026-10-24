#define _GNU_SOURCE
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

static volatile sig_atomic_t delivered,valid;
static int edit;
static uint32_t seed[8] __attribute__((aligned(32)))={1,2,3,4,5,6,7,8};
static uint32_t observed[8] __attribute__((aligned(32)));
static uint32_t load32(const unsigned char* p) { uint32_t v;memcpy(&v,p,4);return v; }
static void handler(int sig,siginfo_t* info,void* context) {
    (void)info;
    ucontext_t* uc=context;
    unsigned char* fp=(unsigned char*)uc->uc_mcontext.fpregs;
    delivered=1;valid=0;
    if(fp && !((uintptr_t)fp&63) && load32(fp+464)==0x46505853 &&
       load32(fp+468)>=836 && load32(fp+468)<=16384 && load32(fp+480)>=832 &&
       load32(fp+load32(fp+468)-4)==0x46505845) {
        valid=1;
        for(int n=0;n<4;++n)
            if(load32(fp+400+4*n)!=seed[n] || load32(fp+816+4*n)!=seed[n+4])valid=0;
        if(edit)for(int n=0;n<4;++n) {
            uint32_t low=seed[n]+10,high=seed[n+4]+10;
            memcpy(fp+400+4*n,&low,4);memcpy(fp+816+4*n,&high,4);
        }
    }
    if(sig==SIGILL)uc->uc_mcontext.gregs[REG_RIP]+=2;
    // A correct return restores saved/edited state, not this live handler state.
    __asm__ volatile("vpxor %%ymm15,%%ymm15,%%ymm15" ::: "ymm15");
}
int main(void) {
    unsigned char* stack=malloc(65536);if(!stack)return 2;
    stack_t ss={.ss_sp=stack,.ss_size=65536};if(sigaltstack(&ss,NULL))return 2;
    long pid=getpid(),tid=syscall(SYS_gettid);
    for(int alt=0;alt<2;++alt)for(int fault=0;fault<2;++fault)for(edit=0;edit<2;++edit) {
        struct sigaction sa={.sa_sigaction=handler,.sa_flags=SA_SIGINFO|(alt?SA_ONSTACK:0)};
        sigemptyset(&sa.sa_mask);if(sigaction(fault?SIGILL:SIGUSR1,&sa,NULL))return 2;
        delivered=valid=0;
        if(fault) {
            __asm__ volatile("vmovdqu %1,%%ymm15; ud2; vmovdqu %%ymm15,%0"
                :"=m"(observed):"m"(seed):"ymm15","memory");
        } else {
            register long nr __asm__("rax")=234;
            __asm__ volatile("vmovdqu %2,%%ymm15; syscall; vmovdqu %%ymm15,%1"
                :"+a"(nr),"=m"(observed):"m"(seed),"D"(pid),"S"(tid),"d"((long)SIGUSR1)
                :"rcx","r11","ymm15","memory");
        }
        int restored=1;
        for(int n=0;n<8;++n)if(observed[n]!=seed[n]+(edit?10u:0u))restored=0;
        if(!delivered || !valid || !restored) {
            printf("{\"signal_fp_pass\":false,\"altstack\":%d,\"synchronous\":%d,\"edited\":%d,\"delivered\":%d,\"valid_image\":%d,\"restored\":%d}\n",alt,fault,edit,delivered,valid,restored);
            return 1;
        }
    }
    puts("{\"signal_fp_pass\":true,\"cases\":8,\"scope\":\"XMM/YMM export and restore, edited state, async/sync, normal/alternate stack\"}");
    return 0;
}
