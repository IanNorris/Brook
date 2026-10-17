#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

/* Raw Linux x86-64 rt_sigaction layout, independent of libc's larger sigset. */
struct action { uint64_t handler,flags,restorer,mask; };
static volatile sig_atomic_t seen;
static struct action choices[2];
static int writer_bad;
static void caught(int sig) { seen=sig; }
static void alternate(int sig) { seen=sig+1; }
static long act(const struct action* input,struct action* output) {
    return syscall(SYS_rt_sigaction,SIGUSR2,input,output,8ul);
}
static void* writer(void* ignored) {
    (void)ignored;
    for(unsigned i=0;i<10000;++i)if(act(&choices[i&1],NULL)) {writer_bad=1;break;}
    return NULL;
}
static int control(unsigned which) {
    if(which==0) {
        struct sigaction sa={0};sa.sa_handler=caught;
        return sigaction(SIGUSR1,&sa,NULL)||kill(getpid(),SIGUSR1)||seen!=SIGUSR1;
    }
    if(which==7) {
        struct action a={.handler=1},old;
        for(unsigned i=0;i<2;++i) {
            int sig=i?SIGSTOP:SIGKILL;
            if(syscall(SYS_rt_sigaction,sig,NULL,&old,8ul))return 1;
            errno=0;
            if(syscall(SYS_rt_sigaction,sig,&a,NULL,8ul)!=-1||errno!=EINVAL)return 1;
        }
        return 0;
    }
    if(which==8) {
        struct action old,before;
        if(act(NULL,&before))return 1;
        errno=0;if(act((void*)1,NULL)!=-1||errno!=EFAULT)return 1;
        if(act(NULL,&old)||memcmp(&old,&before,sizeof old))return 1;
        errno=0;if(act(NULL,(void*)1)!=-1||errno!=EFAULT)return 1;
        void* page=mmap(NULL,4096,PROT_READ,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        if(page==MAP_FAILED)return 1;
        errno=0;int bad=act(NULL,page)!=-1||errno!=EFAULT;
        const struct action replacement={(uintptr_t)caught,SA_RESTART,0,1ul<<(SIGUSR1-1)};
        errno=0;
        if(act(&replacement,page)!=-1||errno!=EFAULT)bad=1;
        // Linux commits the disposition before copying the old action out.
        if(act(NULL,&old)||memcmp(&old,&replacement,sizeof old))bad=1;
        munmap(page,4096);
        return bad||act(NULL,&old);
    }
    if(which==9) {
        struct action a={.handler=(uintptr_t)caught,.mask=(1ul<<(SIGKILL-1))|(1ul<<(SIGSTOP-1))|(1ul<<(SIGUSR1-1))},old;
        return act(&a,NULL)||act(NULL,&old)||old.mask!=(1ul<<(SIGUSR1-1));
    }
    choices[0]=(struct action){(uintptr_t)caught,SA_RESTART,0,1ul<<(SIGUSR1-1)};
    choices[1]=(struct action){(uintptr_t)alternate,SA_SIGINFO,0,1ul<<(SIGTERM-1)};
    if(act(&choices[0],NULL))return 1;
    pthread_t thread;if(pthread_create(&thread,NULL,writer,NULL))return 1;
    int bad=0;
    for(unsigned i=0;i<10000;++i) {
        struct action old;
        if(act(NULL,&old)||(memcmp(&old,&choices[0],sizeof old)&&memcmp(&old,&choices[1],sizeof old))) {bad=1;break;}
    }
    if(pthread_join(thread,NULL))return 1;
    return bad||writer_bad;
}
int main(void) {
    alarm(60);setvbuf(stdout,NULL,_IONBF,0);
    static const char* names[]={"handled-return","async-low","async-noncanonical","async-kernel",
        "fault-low","fault-noncanonical","fault-kernel","uncatchable-signals",
        "checked-action-copies","unblockable-mask","coherent-action-snapshot"};
    const uintptr_t addresses[]={0x1234,0x1000000000000001ULL,0xffff800000000000ULL};
    unsigned passed=0;
    for(unsigned i=0;i<sizeof names/sizeof names[0];++i) {
        pid_t child=fork();if(child<0)return 2;
        if(!child) {
            alarm(10);
            if(i>=1&&i<=6) {
                struct sigaction sa={0};sa.sa_handler=(void(*)(int))addresses[(i-1)%3];
                const int sig=i<=3?SIGUSR1:SIGILL;
                if(sigaction(sig,&sa,NULL))_exit(90);
                if(i<=3)kill(getpid(),sig);
                else __asm__ volatile("ud2");
                _exit(91);
            }
            _exit(control(i)?92:0);
        }
        int status;if(waitpid(child,&status,0)!=child)return 3;
        const int ok=i>=1&&i<=6?(WIFSIGNALED(status)&&WTERMSIG(status)==SIGSEGV):(WIFEXITED(status)&&WEXITSTATUS(status)==0);
        passed+=ok;printf("SIGNAL_RETURN_CASE %s %s status=%d\n",names[i],ok?"PASS":"FAIL",status);
    }
    printf("SIGNAL_RETURN_RESULT %u/11\n",passed);
    return passed==11?0:1;
}
