#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
static long ctl(long op,long value){return syscall(0xB40,op,value,0L,0L,0L,0L);}
static volatile sig_atomic_t interrupted;
static void handler(int signal){(void)signal;interrupted=1;}
static int scratch(void) {
    char name[]="/tmp/brook-page-lease-XXXXXX";int fd=mkstemp(name);assert(fd>=0);assert(unlink(name)==0);return fd;
}
static void writable_and_cow(void) {
    int fd=scratch();unsigned char data[4096];memset(data,0x7b,sizeof(data));
    assert(write(fd,data,sizeof(data))==sizeof(data));assert(lseek(fd,0,SEEK_SET)==0);
    unsigned char* p=mmap(0,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(p!=MAP_FAILED);
    memset(p,0x33,4096);assert(mprotect(p,4096,PROT_READ)==0);
    assert(read(fd,p,64)==-1 && errno==EFAULT);assert(lseek(fd,0,SEEK_CUR)==0);
    assert(mprotect(p,4096,PROT_READ|PROT_WRITE)==0);
    int pipefd[2];assert(pipe(pipefd)==0);pid_t child=fork();assert(child>=0);
    if(!child) {
        struct pollfd ready={.fd=pipefd[0],.events=POLLIN};
        if(poll(&ready,1,5000)!=1)_exit(91);
        char c;if(read(pipefd[0],&c,1)!=1)_exit(92);
        for(unsigned i=0;i<4096;++i)if(p[i]!=0x33)_exit(93);
        _exit(0);
    }
    assert(read(fd,p,4096)==4096);assert(!memcmp(p,data,4096));
    assert(write(pipefd[1],"x",1)==1);int status;assert(waitpid(child,&status,0)==child && status==0);
    assert(read(fd,0,0)==0);
    assert(munmap(p,4096)==0);close(pipefd[0]);close(pipefd[1]);close(fd);
}
int main(int argc,char** argv) {
    writable_and_cow();
    if(argc==2 && !strcmp(argv[1],"--linux-access")){puts("READ_WRITABLE_COW_PASS");return 0;}
    // All unaudited setup syscalls precede admission/pageout.
    int fd=scratch(),nonblock[2],blocked[2],trigger[2];
    unsigned char data[4096];memset(data,0x61,sizeof(data));
    assert(write(fd,data,sizeof(data))==sizeof(data));assert(lseek(fd,0,SEEK_SET)==0);
    assert(pipe2(nonblock,O_NONBLOCK)==0);assert(pipe(blocked)==0);assert(pipe(trigger)==0);
    struct sigaction action;memset(&action,0,sizeof(action));action.sa_handler=handler;
    sigemptyset(&action.sa_mask);assert(sigaction(SIGUSR1,&action,0)==0);
    const pid_t parent=getpid();pid_t helper=fork();assert(helper>=0);
    if(!helper) {
        struct pollfd ready={.fd=trigger[0],.events=POLLIN};
        if(poll(&ready,1,5000)!=1)_exit(94);
        char c;if(read(trigger[0],&c,1)!=1)_exit(95);
        struct timespec delay={.tv_sec=0,.tv_nsec=50000000};nanosleep(&delay,0);
        if(kill(parent,SIGUSR1))_exit(96);
        _exit(0);
    }
    const long baseline=ctl(2,0);assert(baseline==0);
    long address=ctl(0,16*4096);assert(address>0);unsigned char* p=(unsigned char*)(uintptr_t)address;
    memset(p,0x53,16*4096);
    for(unsigned i=0;i<16;++i)assert(ctl(1,address+i*4096)==4096);
    assert(ctl(2,0)==16);
    // Stack-only output must not restore any unrelated cold page.
    assert(write(STDOUT_FILENO,"leases: cold set retained\n",26)==26);assert(ctl(2,0)==16);
    assert(read(fd,p,4096)==4096);assert(!memcmp(p,data,4096));assert(ctl(2,0)==15);
    assert(write(fd,p+4096,4096)==4096);assert(ctl(2,0)==14);
    assert(read(-1,p,1)==-1 && errno==EBADF);assert(ctl(6,0)==0);
    assert(syscall(SYS_write,fd,1L,1L)==-1 && errno==EFAULT);assert(ctl(6,0)==0);
    assert(read(nonblock[0],p+8192,1)==-1 && errno==EAGAIN);assert(ctl(6,0)==0);
    assert(ctl(5,address)==-1 && errno==EBUSY);assert(ctl(6,0)==0);
    assert(ctl(1,address)==4096); // the checked lease really left on unwind
    assert(ctl(5,1)==-1 && errno==EFAULT);assert(ctl(6,0)==0);
    assert(write(trigger[1],"x",1)==1);
    assert(read(blocked[0],p+12288,1)==-1 && errno==EINTR);
    assert(interrupted && ctl(6,0)==0);assert(ctl(1,address+12288)==4096);
    // Waiting/closing are still unaudited and may conservatively restore peers.
    int status;assert(waitpid(helper,&status,0)==helper && status==0);
    assert(munmap(p,16*4096)==0);assert(ctl(2,0)==0 && ctl(6,0)==0);
    close(fd);for(unsigned i=0;i<2;++i){close(nonblock[i]);close(blocked[i]);close(trigger[i]);}
    puts("PAGEABLE_IO_LEASES_PASS targeted_buffers cold_peers failure_unwind blocked_EINTR writable_COW");
}
