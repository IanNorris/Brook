#define _GNU_SOURCE
#include <alloca.h>
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
static volatile sig_atomic_t handled;
// Read genuinely untouched stack pages without asking C's optimizer to assign
// semantics to an indeterminate automatic object.
static unsigned char hardware_byte(const volatile unsigned char* p){
    unsigned char value;__asm__ volatile("movb (%1), %0":"=q"(value):"r"(p):"memory");return value;
}
static void handler(int signal){(void)signal;handled=1;}
static void exited(pid_t pid){int status;assert(waitpid(pid,&status,0)==pid && WIFEXITED(status) && WEXITSTATUS(status)==0);}
__attribute__((noinline)) static void growth(void){
    enum { Bytes=7*1024*1024 };volatile unsigned char* p=alloca(Bytes);
    assert(hardware_byte(p)==0 && hardware_byte(p+4*1024*1024)==0);
    int pipefd[2];assert(pipe(pipefd)==0 && write(pipefd[1],"xyz",3)==3);
    uintptr_t boundary=((uintptr_t)p+2*1024*1024+4095)&~(uintptr_t)4095;
    unsigned crossing=(unsigned)(boundary-1-(uintptr_t)p);
    assert(((uintptr_t)(p+crossing)&4095)==4095);
    assert(read(pipefd[0],(void*)(p+crossing),3)==3);
    assert(p[crossing]=='x' && p[crossing+1]=='y' && p[crossing+2]=='z');
    assert(close(pipefd[0])==0 && close(pipefd[1])==0);
    for(unsigned at=0;at<Bytes;at+=4096)p[at]=(unsigned char)(0x3c+(at>>12));
    assert(raise(SIGUSR1)==0 && handled);
    pid_t pid=fork();assert(pid>=0);
    if(!pid){p[0]^=0x7b;assert(p[0]==(0x3c^0x7b));_exit(0);}
    exited(pid);
    for(unsigned at=0;at<Bytes;at+=4096)assert(p[at]==(unsigned char)(0x3c+(at>>12)));
}
int main(int argc,char** argv){
    struct sigaction action={0};action.sa_handler=handler;sigemptyset(&action.sa_mask);assert(sigaction(SIGUSR1,&action,NULL)==0);
    if(argc==512 && !strcmp(argv[511],"--max-child")){
        unsigned args=0,env=0,count=0;for(unsigned i=0;i<512;++i)args+=strlen(argv[i])+1;
        for(;environ[count];++count)env+=strlen(environ[count])+1;
        assert(args==65536 && count==256 && env==32768);
        assert(getauxval(AT_PAGESZ)==4096 && !strcmp((char*)getauxval(AT_PLATFORM),"x86_64"));
        assert(getauxval(AT_EXECFN) && strlen((char*)getauxval(AT_EXECFN))<65536);
        growth();puts("STACK_MAX_VECTORS_PASS argc=512 envc=256 arg_bytes=65536 env_bytes=32768 growth=7MiB");return 0;
    }
    assert(argc==1);growth();
    pid_t pid=fork();assert(pid>=0);
    if(!pid){*(volatile unsigned char*)(uintptr_t)(0x7fffffffe000ULL-8*1024*1024-4096)=1;_exit(99);}
    int status;assert(waitpid(pid,&status,0)==pid && WIFSIGNALED(status) && WTERMSIG(status)==SIGSEGV);
    char* args[514]={0};char* env[257]={0};for(unsigned i=0;i<512;++i)args[i]="";args[511]="--max-child";
    size_t first=65536-510-(strlen(args[511])+1);args[0]=malloc(first);assert(args[0]);memset(args[0],'a',first-1);args[0][first-1]=0;
    size_t firstEnv=32768-255*4;env[0]=malloc(firstEnv);assert(env[0]);memset(env[0],'z',firstEnv-1);memcpy(env[0],"X=",2);env[0][firstEnv-1]=0;for(unsigned i=1;i<256;++i)env[i]="Y=z";
    pid=fork();assert(pid>=0);if(!pid){execve(argv[0],args,env);perror("max execve");_exit(98);}exited(pid);
    // Oversized count is rejected without replacing the caller's old image.
    for(unsigned i=0;i<513;++i)args[i]="";
    pid=fork();assert(pid>=0);if(!pid){execve(argv[0],args,env);assert(errno==E2BIG);_exit(0);}exited(pid);
    puts("STACK_ACCEPT_PASS zero_growth kernel_copy_cross_page signal_return fork_COW guard max_vectors E2BIG");
}
