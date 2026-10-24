#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

static char alternate[65536] __attribute__((aligned(64)));
static volatile sig_atomic_t handler_ok;
static int use_alt;
static int disabled(void) {
    stack_t ss;
    return sigaltstack(NULL,&ss)==0 && ss.ss_flags==SS_DISABLE;
}
static void* thread_main(void* unused) {
    (void)unused;
    return (void*)(uintptr_t)!disabled();
}
static void handler(int signo,siginfo_t* info,void* context) {
    (void)signo;(void)info;
    stack_t ss,off={.ss_flags=SS_DISABLE};
    char local;
    int on=(uintptr_t)&local>=(uintptr_t)alternate &&
           (uintptr_t)&local<(uintptr_t)alternate+sizeof(alternate);
    ucontext_t* uc=context;
    int queried=sigaltstack(NULL,&ss)==0 && ss.ss_flags==(use_alt?SS_ONSTACK:0);
    errno=0;
    int changed=sigaltstack(&off,NULL);
    handler_ok=on==use_alt && queried && uc->uc_stack.ss_flags==0 &&
               (use_alt?(changed==-1 && errno==EPERM):changed==0);
}
static int wait_ok(pid_t pid) {
    int status;
    return pid>0 && waitpid(pid,&status,0)==pid && WIFEXITED(status) && WEXITSTATUS(status)==0;
}
#define CHECK(test,name) do { if(!(test)) {printf("{\"sigaltstack_pass\":false,\"check\":\"%s\",\"errno\":%d}\n",name,errno);return 1;} } while(0)
int main(int argc,char** argv) {
    if(argc==2 && !strcmp(argv[1],"exec-child"))return !disabled();
    CHECK(disabled(),"fresh query/CoreCLR allocation precondition");
    stack_t ss={.ss_sp=alternate,.ss_size=sizeof(alternate)},old;
    CHECK(sigaltstack(&ss,&old)==0 && old.ss_flags==SS_DISABLE,"install");
    CHECK(sigaltstack(NULL,&old)==0 && old.ss_flags==0 &&
          old.ss_sp==alternate && old.ss_size==sizeof(alternate),"enabled query");
    pthread_t thread;void* result;
    CHECK(pthread_create(&thread,NULL,thread_main,NULL)==0,"thread create");
    CHECK(pthread_join(thread,&result)==0 && !result,"thread starts disabled");
    pid_t child=fork();
    if(child==0) {stack_t inherited;_exit(sigaltstack(NULL,&inherited)!=0 ||
        inherited.ss_sp!=alternate || inherited.ss_size!=sizeof(alternate) || inherited.ss_flags!=0);}
    CHECK(wait_ok(child),"fork inherits");
    child=fork();
    if(child==0) {execl(argv[0],argv[0],"exec-child",(char*)NULL);_exit(2);}
    CHECK(wait_ok(child),"exec disables");
    CHECK(execl("/definitely-missing-sigaltstack-probe","missing",(char*)NULL)==-1 &&
          sigaltstack(NULL,&old)==0 && old.ss_sp==alternate && old.ss_flags==0,"failed exec preserves");
    stack_t alias={.ss_sp=alternate+4096,.ss_size=sizeof(alternate)-4096};
    CHECK(syscall(SYS_sigaltstack,&alias,&alias)==0 && alias.ss_sp==alternate &&
          alias.ss_size==sizeof(alternate) && alias.ss_flags==0 &&
          sigaltstack(NULL,&old)==0 && old.ss_sp==alternate+4096 &&
          old.ss_size==sizeof(alternate)-4096,"input/output alias");
    CHECK(syscall(SYS_sigaltstack,(void*)1,NULL)==-1 && errno==EFAULT,
          "unreadable input");
    CHECK(syscall(SYS_sigaltstack,NULL,(void*)1)==-1 && errno==EFAULT,
          "unwritable output");
    for(use_alt=0;use_alt<2;++use_alt) {
        CHECK(sigaltstack(&ss,NULL)==0,"reinstall");
        struct sigaction sa={.sa_sigaction=handler,.sa_flags=SA_SIGINFO|(use_alt?SA_ONSTACK:0)};
        sigemptyset(&sa.sa_mask);
        CHECK(sigaction(SIGUSR1,&sa,NULL)==0,"sigaction");
        handler_ok=0;
        CHECK(raise(SIGUSR1)==0 && handler_ok,"handler stack status and modification");
    }
    ss.ss_flags=SS_DISABLE;
    CHECK(sigaltstack(&ss,&old)==0 && old.ss_flags==0 && disabled(),"disable and old state");
    ss.ss_flags=4;
    CHECK(sigaltstack(&ss,NULL)==-1 && errno==EINVAL,"invalid flags");
    puts("{\"sigaltstack_pass\":true,\"scope\":\"fresh/CoreCLR, query, thread, fork, exec, failed exec, handler stack range and EPERM, disable, flags\"}");
    return 0;
}
