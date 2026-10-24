#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* Own-process data only; output never contains argv contents. */
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"CMDLINE_CHECK line=%d checks=%u errno=%d\n",__LINE__,checks,errno); exit(1); } } while (0)
static unsigned checks;
static char executable[1024];
static char expected[65536];
static size_t expected_size;

static size_t pack(char *out,int argc,char **argv) {
    size_t n=0;
    for(int i=0;i<argc;++i) { size_t bytes=strlen(argv[i])+1;CHECK(n+bytes<=65536);memcpy(out+n,argv[i],bytes);n+=bytes; }
    return n;
}
static void expect_fd(int fd,const char *want,size_t bytes) {
    char got[65536];size_t n=0;CHECK(lseek(fd,0,SEEK_SET)==0);
    for (;;) {
        /* Small reads exercise offsets through both strings and NUL bytes. */
        ssize_t r=read(fd,got+n,bytes-n<7?bytes-n+1:7);CHECK(r>=0);
        if(!r)break;
        n+=(size_t)r;CHECK(n<=bytes);
    }
    CHECK(n==bytes && !memcmp(got,want,bytes));++checks;
}
static int open_pid(pid_t pid) {
    char path[96];snprintf(path,sizeof(path),"/proc/%ld/cmdline",(long)pid);
    int fd=open(path,O_RDONLY);CHECK(fd>=0);return fd;
}
static void token_write(int fd,char token) { CHECK(write(fd,&token,1)==1); }
static void token_read(int fd,char token) { char got;CHECK(read(fd,&got,1)==1 && got==token); }
static void *thread_check(void *unused) {
    (void)unused;
    char path[128];snprintf(path,sizeof(path),"/proc/self/task/%ld/cmdline",syscall(SYS_gettid));
    int fd=open(path,O_RDONLY);CHECK(fd>=0);expect_fd(fd,expected,expected_size);close(fd);return NULL;
}
int main(int argc,char **argv) {
    if(argc==4 && !strcmp(argv[1],"--after-exec")) {
        int input=atoi(argv[2]),output=atoi(argv[3]);
        token_write(output,'X');token_read(input,'Q');return 0;
    }
    ssize_t path_size=readlink("/proc/self/exe",executable,sizeof(executable)-1);
    CHECK(path_size>0);executable[path_size]=0;
    if(argc!=5 || strcmp(argv[1],"--main")) {
        char *args[]={"forged argv zero","--main","original-argument","","tail",NULL};
        execv(executable,args);CHECK(0);
    }
    /* Linux exec strings are contiguous in argv order. */
    for(int i=1;i<argc;++i)CHECK(argv[i-1]+strlen(argv[i-1])+1==argv[i]);
    ++checks;
    expected_size=pack(expected,argc,argv);
    int own=open_pid(getpid());struct stat st;
    CHECK(fstat(own,&st)==0 && st.st_size==0);
    expect_fd(own,expected,expected_size);
    char saved=argv[2][0];argv[2][0]='E';expected_size=pack(expected,argc,argv);
    expect_fd(own,expected,expected_size); /* same open file, live bytes */
    uintptr_t page=(uintptr_t)argv[2]&~(uintptr_t)4095;
    CHECK(mprotect((void*)page,4096,PROT_NONE)==0);
    expect_fd(own,"",0); /* Linux does not force access to PROT_NONE arguments */
    CHECK(mprotect((void*)page,4096,PROT_READ|PROT_WRITE)==0);
    pthread_t worker;CHECK(pthread_create(&worker,NULL,thread_check,NULL)==0);
    CHECK(pthread_join(worker,NULL)==0);
    int to_child[2],from_child[2];CHECK(pipe(to_child)==0 && pipe(from_child)==0);
    pid_t child=fork();CHECK(child>=0);
    if(!child) {
        close(to_child[1]);close(from_child[0]);
        argv[2][0]='C';token_write(from_child[1],'C');token_read(to_child[0],'E');
        char input[24],output[24];snprintf(input,sizeof(input),"%d",to_child[0]);snprintf(output,sizeof(output),"%d",from_child[1]);
        char *args[]={"postexec-zero","--after-exec",input,output,NULL};
        execv(executable,args);_exit(90);
    }
    close(to_child[0]);close(from_child[1]);token_read(from_child[0],'C');
    int foreign=open_pid(child);char child_expected[65536];
    memcpy(child_expected,expected,expected_size);
    size_t argument_offset=strlen(argv[0])+1+strlen(argv[1])+1;
    child_expected[argument_offset]='C';expect_fd(foreign,child_expected,expected_size);
    expect_fd(own,expected,expected_size); /* child edit must remain CoW-private */
    token_write(to_child[1],'E');token_read(from_child[0],'X');
    char input[24],output[24];snprintf(input,sizeof(input),"%d",to_child[0]);snprintf(output,sizeof(output),"%d",from_child[1]);
    char *after[]={"postexec-zero","--after-exec",input,output};
    size_t after_size=pack(child_expected,4,after);
    expect_fd(foreign,child_expected,after_size); /* same FD follows exec's new AS */
    token_write(to_child[1],'Q');siginfo_t info;
    CHECK(waitid(P_PID,child,&info,WEXITED|WNOWAIT)==0);
    expect_fd(foreign,"",0); /* zombie */
    int status;CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
    CHECK(lseek(foreign,0,SEEK_SET)==0);char byte;errno=0;
    CHECK(read(foreign,&byte,1)==-1 && errno==ESRCH);++checks;
    close(foreign);close(to_child[1]);close(from_child[0]);
    argv[2][0]=saved;expected_size=pack(expected,argc,argv);expect_fd(own,expected,expected_size);close(own);
    printf("PROC_CMDLINE_PASS checks=%u live/empty/offsets/protection/thread/fork/COW/exec/zombie/departed\n",checks);
    return 0;
}
