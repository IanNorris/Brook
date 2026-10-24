#define _GNU_SOURCE
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <ucontext.h>
#include <unistd.h>
static int mode;
static unsigned char legacy[512] __attribute__((aligned(64)));
static uint32_t seed[8] __attribute__((aligned(32)))={1,2,3,4,5,6,7,8},out[8];
static uint32_t mx=0x3f80,observed_mx;
static uint16_t cw=0x077f,observed_cw;
static unsigned char* unreadable;
static void handler(int sig,siginfo_t* info,void* opaque) {
    (void)sig;(void)info;
    ucontext_t* uc=opaque;unsigned char* fp=(void*)uc->uc_mcontext.fpregs;
    if(!fp)_exit(5);
    if(mode==0) {
        memcpy(legacy,fp,512);memset(legacy+464,0,48);
        uc->uc_mcontext.fpregs=(void*)legacy;
    } else if(mode==1)uc->uc_mcontext.fpregs=NULL;
    else if(mode==2) { uint32_t bad=0x10000;memcpy(fp+24,&bad,4); }
    else if(mode==3) { uint64_t bad=1ULL<<63;memcpy(fp+520,&bad,8); }
    else if(mode==4) { uint64_t bad=1ULL<<63;memcpy(fp+512,&bad,8); }
    else if(mode==5)uc->uc_mcontext.fpregs=(void*)(uintptr_t)0x10000;
    else if(mode==6 || mode==9) {
        uint32_t size;memcpy(&size,fp+468,4);
        if(size<836 || size>16384)_exit(6);
        fp[size-4]^=1;
        if(mode==9) { uint32_t bad=0x10000;memcpy(fp+24,&bad,4); }
    }
    else if(mode==7) { uint32_t bad=0;memcpy(fp+480,&bad,4); }
    else if(mode==8) { memcpy(unreadable,fp,512);uc->uc_mcontext.fpregs=(void*)unreadable; }
    __asm__ volatile("vpxor %%ymm15,%%ymm15,%%ymm15" ::: "ymm15");
}
static void child(void) {
    if(mode==8) {
        unsigned char* p=mmap(NULL,8192,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        if(p==MAP_FAILED || mprotect(p+4096,4096,PROT_NONE))_exit(2);
        unreadable=p+4096-512;
    }
    struct sigaction sa={.sa_sigaction=handler,.sa_flags=SA_SIGINFO};
    sigemptyset(&sa.sa_mask);if(sigaction(SIGUSR1,&sa,NULL))_exit(2);
    long pid=getpid(),tid=syscall(SYS_gettid);
    register long nr __asm__("rax")=234;
    __asm__ volatile("fninit; fldcw %9; ldmxcsr %8; vmovdqu %4,%%ymm15; syscall; vmovdqu %%ymm15,%1; stmxcsr %2; fnstcw %3"
        :"+a"(nr),"=m"(out),"=m"(observed_mx),"=m"(observed_cw)
        :"m"(seed),"D"(pid),"S"(tid),"d"((long)SIGUSR1),"m"(mx),"m"(cw)
        :"rcx","r11","ymm15","memory");
    if(mode>=2 && mode!=6 && mode!=7)_exit(3);
    // A missing extension trailer has Linux's legacy fallback semantics.
    for(int i=0;i<8;++i)if(out[i]!=((mode==0 || mode==6 || mode==7) && i<4?seed[i]:0))_exit(4);
    if(observed_mx!=(mode==1?0x1f80u:mx) || observed_cw!=(mode==1?0x037fu:cw))_exit(4);
    _exit(0);
}
int main(void) {
    struct rlimit limit={0,0};setrlimit(RLIMIT_CORE,&limit);
    for(mode=0;mode<10;++mode) {
        pid_t p=fork();if(p<0)return 2;if(!p)child();
        int status;if(waitpid(p,&status,0)!=p)return 2;
        int ok=(mode<2 || mode==6 || mode==7) ? WIFEXITED(status)&&WEXITSTATUS(status)==0
                     : (WIFSIGNALED(status)&&WTERMSIG(status)==SIGSEGV);
        if(!ok) { printf("{\"signal_fp_return_pass\":false,\"mode\":%d,\"wait_status\":%d}\n",mode,status);return 1; }
    }
    puts("{\"signal_fp_return_pass\":true,\"cases\":10,\"scope\":\"legacy/null/discovery fallback; invalid MXCSR, XCOMP, XSTATE, pointer and unreadable trailer\"}");
    return 0;
}
