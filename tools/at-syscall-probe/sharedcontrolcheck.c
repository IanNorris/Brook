/* Public synthetic regression: independently exec'd processes exchange known
 * data through an 8,208-byte named shared object, including its partial page.
 * Output contains fixed stages/counts only; no application memory is read. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/futex.h>
#include <signal.h>
#include <stdatomic.h>
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

enum { bytes=8208, header=16, pipe_rounds=8, atomic_rounds=64 };
struct control { _Atomic uint32_t send,ack;uint32_t magic,guard;unsigned char data[bytes-header]; };
_Static_assert(sizeof(struct control)==bytes,"control size");
_Static_assert(ATOMIC_INT_LOCK_FREE==2,"process-shared atomic words must be lock free");
static pid_t child_pid;
static char owned_path[160];
static int path_owned;
static void cleanup(void) {
    if(child_pid>0){kill(child_pid,SIGKILL);while(waitpid(child_pid,NULL,0)<0&&errno==EINTR){}}
    if(path_owned)unlink(owned_path);
}
static void failure(int line) { fprintf(stderr,"SHAREDCONTROLCHECK FAIL line=%d errno=%d\n",line,errno);exit(1); }
#define CHECK(x) do{if(!(x))failure(__LINE__);}while(0)
static void expired(int sig) { (void)sig;if(child_pid>0)kill(child_pid,SIGKILL);if(path_owned)unlink(owned_path);_exit(124); }
static void watchdog(void) {
    struct sigaction a={0};a.sa_handler=expired;CHECK(!sigaction(SIGALRM,&a,NULL));alarm(30);
}
static void byte_write(int fd,char value) { CHECK(write(fd,&value,1)==1); }
static void byte_read(int fd,char value) { char got;CHECK(read(fd,&got,1)==1&&got==value); }
static unsigned char pattern(unsigned round,unsigned index) { return (unsigned char)(round*37+index*13+11); }
static void fill(struct control*c,unsigned round) {
    for(unsigned i=0;i<sizeof(c->data);++i)c->data[i]=pattern(round,i);
}
static void verify(const struct control*c,const unsigned char*tail,unsigned round) {
    CHECK(c->magic==0x42524f4f&&c->guard==0x4b43544c);
    for(unsigned i=0;i<sizeof(c->data);++i)CHECK(c->data[i]==pattern(round,i));
    for(unsigned i=0;i<16;++i)CHECK(tail[i]==pattern(round,8192-header+i));
}
static void wake(_Atomic uint32_t*word) { CHECK(syscall(SYS_futex,word,FUTEX_WAKE,1,NULL,NULL,0)>=0); }
static void wait_word(_Atomic uint32_t*word,uint32_t want) {
    for(;;){uint32_t got=atomic_load_explicit(word,memory_order_acquire);if(got==want)return;
        struct timespec timeout={2,0};long r=syscall(SYS_futex,word,FUTEX_WAIT,got,&timeout,NULL,0);
        CHECK(r==0||(r<0&&(errno==EAGAIN||errno==EINTR)));
    }
}
static struct control* mapped(int fd) {
    struct control*c=mmap(NULL,bytes,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);CHECK(c!=MAP_FAILED);return c;
}
static const unsigned char* tail_map(int fd) {
    void*p=mmap(NULL,16,PROT_READ,MAP_SHARED,fd,8192);CHECK(p!=MAP_FAILED);return p;
}
static int helper(int argc,char**argv) {
    CHECK(argc==7);watchdog();int input=atoi(argv[3]),output=atoi(argv[4]);
    int duplicate=open(argv[2],O_CREAT|O_EXCL|O_RDWR,0600);CHECK(duplicate<0&&errno==EEXIST);
    int fd=open(argv[2],O_RDWR);CHECK(fd>=0);struct stat st;CHECK(!fstat(fd,&st));
    CHECK((unsigned long long)st.st_dev==strtoull(argv[5],NULL,10));
    CHECK((unsigned long long)st.st_ino==strtoull(argv[6],NULL,10)&&st.st_size==bytes);
    struct control*c=mapped(fd);CHECK(!close(fd));
    int ro=open(argv[2],O_RDONLY);CHECK(ro>=0);const struct control*alias=mmap(NULL,bytes,PROT_READ,MAP_SHARED,ro,0);CHECK(alias!=MAP_FAILED);
    const unsigned char*tail=tail_map(ro);CHECK(!close(ro));byte_write(output,'R');
    for(unsigned r=1;r<=pipe_rounds;++r){byte_read(input,'P');verify(alias,tail,r);fill(c,r+1000);byte_write(output,'A');}
    byte_read(input,'U'); /* parent has unlinked/recreated the name; old aliases must survive */
    for(unsigned r=1;r<=atomic_rounds;++r){wait_word(&c->send,r);verify(alias,tail,r+2000);fill(c,r+3000);atomic_store_explicit(&c->ack,r,memory_order_release);wake(&c->ack);}
    CHECK(!munmap((void*)tail,16)&&!munmap((void*)alias,bytes)&&!munmap(c,bytes));close(input);close(output);return 0;
}
int main(int argc,char**argv) {
    setbuf(stdout,NULL);if(argc>1&&!strcmp(argv[1],"--helper"))return helper(argc,argv);
    CHECK(argc==1);CHECK(!atexit(cleanup));watchdog();
    struct timespec stamp;CHECK(!clock_gettime(CLOCK_MONOTONIC,&stamp));snprintf(owned_path,sizeof(owned_path),"/dev/shm/brook-control-%ld-%ld-%ld",(long)getpid(),stamp.tv_sec,stamp.tv_nsec);
    int fd=open(owned_path,O_CREAT|O_EXCL|O_RDWR,0600);CHECK(fd>=0);path_owned=1;CHECK(!ftruncate(fd,bytes));
    struct stat st;CHECK(!fstat(fd,&st));struct control*c=mapped(fd);const unsigned char*tail=tail_map(fd);
    memset(c,0,bytes);c->magic=0x42524f4f;c->guard=0x4b43544c;CHECK(!close(fd));
    int input[2],output[2];CHECK(!pipe(input)&&!pipe(output));char exe[1024];ssize_t n=readlink("/proc/self/exe",exe,sizeof(exe)-1);CHECK(n>0);exe[n]=0;
    child_pid=fork();CHECK(child_pid>=0);
    if(!child_pid){close(input[1]);close(output[0]);char in[24],out[24],dev[32],ino[32];snprintf(in,sizeof(in),"%d",input[0]);snprintf(out,sizeof(out),"%d",output[1]);snprintf(dev,sizeof(dev),"%llu",(unsigned long long)st.st_dev);snprintf(ino,sizeof(ino),"%llu",(unsigned long long)st.st_ino);char*args[]={exe,"--helper",owned_path,in,out,dev,ino,NULL};execv(exe,args);_exit(125);}
    close(input[0]);close(output[1]);byte_read(output[0],'R');
    for(unsigned r=1;r<=pipe_rounds;++r){fill(c,r);byte_write(input[1],'P');byte_read(output[0],'A');verify(c,tail,r+1000);}
    puts("SHAREDCONTROLCHECK pipe-coherence PASS rounds=8 partial-page=16");
    CHECK(!unlink(owned_path));path_owned=0;int fresh=open(owned_path,O_CREAT|O_EXCL|O_RDWR,0600);CHECK(fresh>=0);path_owned=1;CHECK(!ftruncate(fresh,bytes));
    struct stat replacement;CHECK(!fstat(fresh,&replacement)&&replacement.st_ino!=st.st_ino);struct control*new_map=mapped(fresh);CHECK(new_map->magic==0&&new_map->guard==0);CHECK(!close(fresh));byte_write(input[1],'U');
    for(unsigned r=1;r<=atomic_rounds;++r){fill(c,r+2000);atomic_store_explicit(&c->send,r,memory_order_release);wake(&c->send);wait_word(&c->ack,r);verify(c,tail,r+3000);CHECK(new_map->magic==0&&new_map->guard==0);}
    close(input[1]);close(output[0]);int status;CHECK(waitpid(child_pid,&status,0)==child_pid);child_pid=0;CHECK(WIFEXITED(status)&&WEXITSTATUS(status)==0);
    CHECK(!munmap((void*)tail,16)&&!munmap(c,bytes)&&!munmap(new_map,bytes));CHECK(!unlink(owned_path));path_owned=0;alarm(0);
    puts("SHAREDCONTROLCHECK PASS independent-exec/separate-opens/readonly-alias/fd-close/partial-page/unlink-recreate/shared-futex rounds=64");return 0;
}
