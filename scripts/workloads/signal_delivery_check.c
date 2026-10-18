/* Owned x86-64 Linux-ABI signal-stack regression fixture.
 * Build statically with a musl C compiler. No arguments: 18 stack cases.
 * --pressure / --pressure-cow: ONLY inside a disposable, memory-bounded VM;
 * these deliberately touch up to 8 GiB. A passing child status alone does not
 * prove OOM containment: also require serial PMM exhaustion, inaccessible signal
 * frame, a surviving parent/kernel, checked shutdown and read-only disk checks.
 */
#define _GNU_SOURCE
#include <sys/mman.h>
#include <sys/wait.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ucontext.h>
#include <errno.h>

static uintptr_t stack_base,stack_end;
static volatile sig_atomic_t seen,require_alt,pressure;
static void handler(int signo,siginfo_t* info,void* context) {
    volatile unsigned char local;
    uintptr_t here=(uintptr_t)&local;
    if(!info || info->si_signo!=signo || !context ||
       (require_alt && (here<stack_base || here>=stack_end)))_exit(91);
    if(signo==SIGILL) ((ucontext_t*)context)->uc_mcontext.gregs[REG_RIP]+=2;
    if(pressure && signo==SIGSEGV)_exit(73);
    seen=1;
}
__attribute__((naked,noreturn)) static void bad_async(void* stack,long pid) {
    __asm__ volatile("mov %rdi,%rsp; mov %rsi,%rdi; mov $10,%esi; mov $62,%eax; syscall; ud2");
}
__attribute__((naked,noreturn)) static void bad_sync(void* stack) {
    __asm__ volatile("mov %rdi,%rsp; ud2");
}
static void fail(const char* message) {perror(message);_exit(92);}
static int one(int sync,int mode) {
    const size_t page=4096,size=65536;
    unsigned char* memory=mmap(NULL,size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(memory==MAP_FAILED)fail("mmap");
    // Mode1 exercises a real inherited COW mapping rather than an untouched
    // lazy altstack. Every other case starts with demand-zero pages.
    if(mode==1 || mode==10)memset(memory,0x5a,size);
    pid_t child=fork();if(child<0)fail("fork");
    if(!child) {
        stack_base=(uintptr_t)memory;stack_end=stack_base+size;seen=0;require_alt=mode<7 || mode>=9;pressure=mode>=9;
        stack_t alt={.ss_sp=memory,.ss_size=mode==5?2*page+128:size,.ss_flags=0};
        if(sigaltstack(&alt,NULL))fail("sigaltstack");
        struct sigaction action={0};action.sa_sigaction=handler;action.sa_flags=SA_SIGINFO|(require_alt?SA_ONSTACK:0);
        sigemptyset(&action.sa_mask);
        if(sigaction(sync?SIGILL:SIGUSR1,&action,NULL))fail("sigaction");
        if(pressure && sigaction(SIGSEGV,&action,NULL))fail("pressure sigaction");
        if(pressure) {
            const size_t bytes=8ULL*1024*1024*1024;
            volatile unsigned char* pages=mmap(NULL,bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
            if(pages==MAP_FAILED)fail("pressure mmap");
            for(size_t offset=0;offset<bytes;offset+=page) {
                pages[offset]=1;
                if(offset%(256ULL*1024*1024)==0) {
                    static const char mark[]="pressure: another 256 MiB reached\n";
                    (void)write(STDOUT_FILENO,mark,sizeof(mark)-1);
                }
            }
            _exit(94); // did not actually reach the guest memory limit
        }
        if(mode==2 && munmap(memory,size))fail("munmap");
        if(mode==3 && mprotect(memory,size,PROT_READ))fail("read-only");
        if(mode==4 && mprotect(memory,size,PROT_NONE))fail("none");
        if(mode==5 && mprotect(memory+2*page,page,PROT_NONE))fail("cross-page");
        if(mode==6) { // overwrite registered range with a fresh inaccessible mapping
            if(munmap(memory,size))fail("unmap");
            if(mmap(memory,size,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0)!=memory)fail("replace");
        }
        if(mode==8) {
            if(mprotect(memory,size,PROT_NONE))fail("bad ordinary stack");
            if(sync)bad_sync(memory+size/2);
            else bad_async(memory+size/2,getpid());
        }
        if(sync)__asm__ volatile("ud2");
        else if(kill(getpid(),SIGUSR1))fail("kill");
        _exit(seen?0:93);
    }
    int status=0;pid_t got;
    do {got=waitpid(child,&status,0);}while(got<0 && errno==EINTR);
    if(got!=child)fail("waitpid");
    munmap(memory,size);
    int success=mode==0 || mode==1 || mode==7;
    int passed=success?(WIFEXITED(status)&&WEXITSTATUS(status)==0):
                       ((WIFSIGNALED(status)&&WTERMSIG(status)==SIGSEGV) ||
                        (mode>=9 && WIFEXITED(status) && WEXITSTATUS(status)==73));
    printf("signal_delivery sync=%d mode=%d status=%x expected=%s %s\n",sync,mode,status,success?"return":"SIGSEGV",passed?"PASS":"FAIL");
    fflush(stdout);return passed?0:1;
}
int main(int argc,char** argv) {
    alarm(120);
    if(argc==2 && (!strcmp(argv[1],"--pressure") || !strcmp(argv[1],"--pressure-cow"))) {
        int result=one(1,!strcmp(argv[1],"--pressure-cow")?10:9);
        puts(result?"SIGNAL_PRESSURE_FAIL":"SIGNAL_PRESSURE_PASS");
        return result;
    }
    if(argc!=1)return 2;
    int failures=0;
    for(int sync=0;sync<2;++sync)for(int mode=0;mode<=8;++mode)failures+=one(sync,mode);
    printf("SIGNAL_DELIVERY_%s cases=18 failures=%d uid=%ld\n",failures?"FAIL":"PASS",failures,(long)geteuid());
    return failures?1:0;
}
