// Linux control and Brook guest: real file-fault signal ABI and stale-EOF races.
// Build with MUSL_CC -static -O2 -pthread. Usage: file-faults DIR [--race-hooks]
// The optional races require Brook's root-only test-kernel mapping barriers.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int report_fd;
static volatile sig_atomic_t timed_out,active_child;
static void deadline(int number) {
    (void)number;timed_out=1;
    if(active_child>0)kill(active_child,SIGKILL);
}
static volatile unsigned char* expected_address;
struct report { int signal,code,address_matches; };
static void handler(int number,siginfo_t* info,void* context) {
    (void)context;
    struct report report={number,info->si_code,info->si_addr==(void*)expected_address};
    if(write(report_fd,&report,sizeof(report))!=(ssize_t)sizeof(report))_exit(98);
    _exit(0);
}
static void require(int condition) { if(!condition)_exit(97); }
static volatile unsigned char sink;
static volatile unsigned char* race_address;
static unsigned char race_value;
static void* racer(void* ignored) { (void)ignored;race_value=*race_address;return NULL; }
static long hook(unsigned action,void* address,unsigned phase) {
    return syscall(0xb30,action,address,8192ul,phase,1ul,0ul);
}
static void fault_child(int which,int fd,int output) {
    report_fd=output;
    if(which!=7) {
        struct sigaction action={0};action.sa_sigaction=handler;action.sa_flags=SA_SIGINFO;
        require(!sigaction(SIGBUS,&action,NULL) && !sigaction(SIGSEGV,&action,NULL));
    }
    int shared=which==3;
    int prot=which==3?PROT_READ:(PROT_READ|PROT_WRITE);
    off_t offset=which==4?8192:0;
    volatile unsigned char* memory=mmap(NULL,12288,prot,shared?MAP_SHARED:MAP_PRIVATE,fd,offset);
    require(memory!=MAP_FAILED);
    expected_address=memory+(which==0 || which==4?0:which==2?8192+37:4096);
    if(which==5) {
        require(memory[4096]==0x37 && memory[4097]==0 && memory[8191]==0);
        munmap((void*)memory,12288);_exit(0);
    }
    if(which==6)require(!mprotect((void*)memory,12288,PROT_NONE));
    if(which==8)require(!close(fd));
    if(which==9) {
        int pipes[2];char byte=1;require(!pipe(pipes));require(write(pipes[1],&byte,1)==1);
        errno=0;require(read(pipes[0],(void*)expected_address,1)==-1 && errno==EFAULT);
        errno=0;require(write(pipes[1],(void*)expected_address,1)==-1 && errno==EFAULT);
        // Force the syscall: libc's vDSO clock implementation is a user store
        // and correctly faults with SIGBUS instead of exercising uaccess.
        errno=0;require(syscall(SYS_clock_gettime,CLOCK_MONOTONIC,(struct timespec*)expected_address)==-1 && errno==EFAULT);
        close(pipes[0]);close(pipes[1]);munmap((void*)memory,12288);_exit(0);
    }
    if(which>=10) {
        require(hook(0,(void*)memory,2)==0);race_address=expected_address;
        pthread_t thread;require(!pthread_create(&thread,NULL,racer,NULL));
        unsigned wait;
        for(wait=0;wait<3000;++wait) {
            long state=hook(1,NULL,0);require(state>=0);
            if(((unsigned long)state>>16)&0xffff)break;
            usleep(1000);
        }
        require(wait<3000);
        if(which==10) { unsigned char byte=0x71;require(pwrite(fd,&byte,1,4096)==1); }
        if(which==11)require(mmap((void*)memory,12288,PROT_READ|PROT_WRITE,
                                  MAP_FIXED|MAP_PRIVATE|MAP_ANONYMOUS,-1,0)==(void*)memory);
        if(which==12)require(!mprotect((void*)memory,12288,PROT_NONE));
        require(hook(2,NULL,0)==0);require(!pthread_join(thread,NULL));
        require(race_value==(which==10?0x71:0));munmap((void*)memory,12288);_exit(0);
    }
    if(which==2)*expected_address=0x55;else sink=*expected_address;
    _exit(96); // A required synchronous fault did not happen.
}
int main(int argc,char** argv) {
    if(argc<2 || argc>3 || (argc==3 && strcmp(argv[2],"--race-hooks")))return 2;
    struct sigaction timeout_action={0};timeout_action.sa_handler=deadline;
    if(sigaction(SIGALRM,&timeout_action,NULL))return 2;
    const char* names[]={"empty-file","private-read-eof","private-write-eof","shared-read-eof",
        "offset-past-eof","valid-partial-tail","protection-stays-segv","default-sigbus",
        "closed-fd-mapping","checked-uaccess-eof","stale-eof-growth","stale-eof-remap","stale-eof-protection"};
    unsigned total=argc==3?13:10,passed=0;
    for(unsigned which=0;which<total;++which) {
        char path[512];snprintf(path,sizeof(path),"%s/brook-file-fault-XXXXXX",argv[1]);
        int fd=mkstemp(path),pipes[2];if(fd<0 || pipe(pipes))return 2;
        off_t size=which==0?0:which==5?4097:4096;
        if(ftruncate(fd,size))return 2;
        if(which==5) { char byte=0x37;if(pwrite(fd,&byte,1,4096)!=1)return 2; }
        pid_t child=fork();if(child<0)return 2;
        if(!child) {
            signal(SIGALRM,SIG_DFL);syscall(SYS_alarm,10);
            close(pipes[0]);fault_child(which,fd,pipes[1]);
        }
        active_child=child;timed_out=0;syscall(SYS_alarm,20);
        close(fd);close(pipes[1]);
        struct report report={0};ssize_t got=read(pipes[0],&report,sizeof(report));close(pipes[0]);
        int status=0;
        if(timed_out) { syscall(SYS_alarm,0);waitpid(child,&status,WNOHANG);return 124; }
        if(waitpid(child,&status,0)!=child)return timed_out?124:2;
        syscall(SYS_alarm,0);active_child=0;
        int wanted=(which==5 || which==9 || which==10 || which==11)?0:
                   (which==6 || which==12)?SIGSEGV:SIGBUS;
        int ok=which==7?(WIFSIGNALED(status) && WTERMSIG(status)==SIGBUS):
               WIFEXITED(status) && WEXITSTATUS(status)==0 &&
               (!wanted?got==0:got==sizeof(report) && report.signal==wanted && report.address_matches &&
                 (wanted!=SIGBUS || report.code==BUS_ADRERR));
        if(unlink(path))ok=0;
        printf("FILE_FAULT_CASE %s passed=%d status=%d signal=%d code=%d address=%d\n",
               names[which],ok,status,report.signal,report.code,report.address_matches);fflush(stdout);
        passed+=ok!=0;
    }
    printf("FILE_FAULT_RESULT %u/%u\n",passed,total);return passed==total?0:1;
}
