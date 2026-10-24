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
static uint32_t input_mxcsr=0x3f80,output_mxcsr,default_mxcsr=0x1f80;
static uint16_t input_cw=0x077f,output_cw,default_cw=0x037f;
static double output_st;
static uint32_t load32(const unsigned char* p) { uint32_t v;memcpy(&v,p,4);return v; }
static void handler(int sig,siginfo_t* info,void* context) {
    uint16_t live_cw;uint32_t live_mxcsr;
    __asm__ volatile("fnstcw %0; stmxcsr %1":"=m"(live_cw),"=m"(live_mxcsr));
    (void)info;
    ucontext_t* uc=context;
    unsigned char* fp=(unsigned char*)uc->uc_mcontext.fpregs;
    delivered=1;valid=0;
    if(fp && !((uintptr_t)fp&63) && load32(fp+464)==0x46505853 &&
       load32(fp+468)>=836 && load32(fp+468)<=16384 && load32(fp+480)>=832 &&
       load32(fp+load32(fp+468)-4)==0x46505845) {
        valid=1;
        if(live_cw!=default_cw || live_mxcsr!=default_mxcsr)valid=0;
        if(load32(fp+24)!=input_mxcsr || (load32(fp)&65535)!=input_cw)valid=0;
        if(load32(fp+32)!=0 || load32(fp+36)!=0x80000000 ||
           (load32(fp+40)&65535)!=0x3fff)valid=0;
        for(int n=0;n<4;++n)
            if(load32(fp+400+4*n)!=seed[n] || load32(fp+816+4*n)!=seed[n+4])valid=0;
        if(edit)for(int n=0;n<4;++n) {
            uint32_t low=seed[n]+10,high=seed[n+4]+10;
            memcpy(fp+400+4*n,&low,4);memcpy(fp+816+4*n,&high,4);
        }
        if(edit) {
            uint32_t mx=0x5f80;uint16_t cw=0x0b7f,exponent=0x4000;
            memcpy(fp+24,&mx,4);memcpy(fp,&cw,2);memcpy(fp+40,&exponent,2);
        }
    }
    if(sig==SIGILL)uc->uc_mcontext.gregs[REG_RIP]+=2;
    // A correct return restores saved/edited state, not this live handler state.
    __asm__ volatile("fninit; fldcw %0; ldmxcsr %1; vpxor %%ymm15,%%ymm15,%%ymm15"
        ::"m"(default_cw),"m"(default_mxcsr):"ymm15");
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
            __asm__ volatile("fninit; fldcw %6; fld1; ldmxcsr %5; vmovdqu %4,%%ymm15; ud2; vmovdqu %%ymm15,%0; stmxcsr %1; fnstcw %2; fstpl %3"
                :"=m"(observed),"=m"(output_mxcsr),"=m"(output_cw),"=m"(output_st)
                :"m"(seed),"m"(input_mxcsr),"m"(input_cw):"ymm15","memory");
        } else {
            register long nr __asm__("rax")=234;
            __asm__ volatile("fninit; fldcw %10; fld1; ldmxcsr %9; vmovdqu %5,%%ymm15; syscall; vmovdqu %%ymm15,%1; stmxcsr %2; fnstcw %3; fstpl %4"
                :"+a"(nr),"=m"(observed),"=m"(output_mxcsr),"=m"(output_cw),"=m"(output_st)
                :"m"(seed),"D"(pid),"S"(tid),"d"((long)SIGUSR1),"m"(input_mxcsr),"m"(input_cw)
                :"rcx","r11","ymm15","memory");
        }
        int restored=1;
        if(output_mxcsr!=(edit?0x5f80u:input_mxcsr) || output_cw!=(edit?0x0b7fu:input_cw) ||
           output_st!=(edit?2.0:1.0))restored=0;
        for(int n=0;n<8;++n)if(observed[n]!=seed[n]+(edit?10u:0u))restored=0;
        if(!delivered || !valid || !restored) {
            printf("{\"signal_fp_pass\":false,\"altstack\":%d,\"synchronous\":%d,\"edited\":%d,\"delivered\":%d,\"valid_image\":%d,\"restored\":%d}\n",alt,fault,edit,delivered,valid,restored);
            return 1;
        }
    }
    puts("{\"signal_fp_pass\":true,\"cases\":8,\"scope\":\"XMM/YMM export and restore, edited state, async/sync, normal/alternate stack\"}");
    return 0;
}
