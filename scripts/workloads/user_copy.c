// Static Linux control/Brook probe. Optional races need a root test kernel.
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
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static void require(int condition) { if(!condition)_exit(97); }
static unsigned char pattern(unsigned i) { return (unsigned char)((i/4096)*0x31+i%251); }
static unsigned char* memory;
static int file,operation;
static ssize_t result;
static int saved_errno;
static long hook(unsigned action,void* address) { return syscall(0xb30,action,address,8192ul,4ul,1ul,0ul); }
static void* worker(void* unused) {
    (void)unused;errno=0;
    if(operation==11)result=syscall(SYS_clock_gettime,CLOCK_MONOTONIC,memory);
    else if(operation==6 || operation==9 || operation==12)result=read(file,memory,8192);
    else result=write(file,memory,8192);
    saved_errno=errno;return NULL;
}
static void one_case(unsigned which,const char* directory) {
    signal(SIGALRM,SIG_DFL);syscall(SYS_alarm,15);
    char path[512];snprintf(path,sizeof(path),"%s/brook-user-copy-XXXXXX",directory);
    file=mkstemp(path);require(file>=0);
    memory=mmap(NULL,8192,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);require(memory!=MAP_FAILED);
    unsigned char data[8192];for(unsigned i=0;i<8192;++i)memory[i]=data[i]=pattern(i);
    if(which==0 || which==1 || which==3 || which==6 || which==9 || which==12) {
        require(write(file,data,sizeof(data))==(ssize_t)sizeof(data));require(lseek(file,0,SEEK_SET)==0);
    }
    if(which==0) {
        memset(memory,0xe7,8192);require(read(file,memory,8192)==8192&&!memcmp(memory,data,8192));
        require(lseek(file,0,SEEK_CUR)==8192);
    } else if(which==1 || which==2 || which==3) {
        require(!mprotect(memory,8192,which==3?PROT_READ:PROT_NONE));errno=0;
        ssize_t got=which==2?write(file,memory,32):read(file,memory,32);
        require(got==-1&&errno==EFAULT&&lseek(file,0,SEEK_CUR)==0);
    } else if(which==4) {
        require(read(file,(void*)1,0)==0&&write(file,(void*)1,0)==0&&lseek(file,0,SEEK_CUR)==0);
    } else if(which==5) {
        size_t size=strlen(path)+1;require(size<4096);char* end=(char*)memory+4096-size;
        memcpy(end,path,size);require(!mprotect(memory+4096,4096,PROT_NONE));
        int opened=open(end,O_RDONLY);require(opened>=0);close(opened);
    } else {
        if(which==8)require(!fcntl(file,F_SETFL,O_APPEND));
        if(which==6 || which==9 || which==12)memset(memory,0xe7,8192);
        require(!hook(0,memory));operation=(int)which;
        pthread_t thread;require(!pthread_create(&thread,NULL,worker,NULL));
        unsigned wait;
        for(wait=0;wait<3000;++wait) {
            long state=hook(1,NULL);require(state>=0);
            if(((unsigned long)state>>16)&0xffff)break;
            usleep(1000);
        }
        require(wait<3000);
        pid_t cow_child=-1;int cow_pipe[2]={-1,-1};
        if(which==12) {
            require(!pipe(cow_pipe));cow_child=fork();require(cow_child>=0);
            if(!cow_child) {
                syscall(SYS_alarm,10);close(cow_pipe[1]);char byte;
                require(read(cow_pipe[0],&byte,1)==1);
                for(unsigned i=0;i<8192;++i)require(memory[i]==0xe7);
                close(cow_pipe[0]);_exit(0);
            }
            close(cow_pipe[0]);
        } else if(which==9 || which==10)require(!munmap(memory,4096));
        else require(!mprotect(memory+(which==11?0:4096),4096,PROT_NONE));
        require(!hook(2,NULL)&&!pthread_join(thread,NULL));
        if(which==12) {
            require(result==8192&&!memcmp(memory,data,8192)&&lseek(file,0,SEEK_CUR)==8192);
            char byte=1;require(write(cow_pipe[1],&byte,1)==1);close(cow_pipe[1]);
            int status=0;require(waitpid(cow_child,&status,0)==cow_child&&WIFEXITED(status)&&!WEXITSTATUS(status));
        } else if(which>=9) {
            require(result==-1&&saved_errno==EFAULT);
            if(which!=11)require(lseek(file,0,SEEK_CUR)==0);
            if(which==10) { struct stat st;require(!fstat(file,&st)&&st.st_size==0); }
        } else {
            require(result==4096&&lseek(file,0,SEEK_CUR)==4096);
            if(which==6)require(!memcmp(memory,data,4096));
            else {
                struct stat st;require(!fstat(file,&st)&&st.st_size==4096);
                unsigned char actual[4096];require(pread(file,actual,4096,0)==4096&&!memcmp(actual,data,4096));
            }
            require(!mprotect(memory+4096,4096,PROT_READ|PROT_WRITE));
            if(which==6)require(read(file,memory+4096,4096)==4096&&!memcmp(memory,data,8192));
            else require(write(file,memory+4096,4096)==4096);
            require(lseek(file,0,SEEK_CUR)==8192);
            if(which!=6) {
                unsigned char actual[8192];require(pread(file,actual,8192,0)==8192&&!memcmp(actual,data,8192));
            }
        }
    }
    if(which==9 || which==10)require(!munmap(memory+4096,4096));else require(!munmap(memory,8192));
    require(!close(file)&&!unlink(path));_exit(0);
}
int main(int argc,char** argv) {
    if(argc<2 || argc>3 || (argc==3&&strcmp(argv[2],"--race-hooks")))return 2;
    const char* names[]={"roundtrip","invalid-copyout","invalid-copyin","readonly-copyout","zero-length",
        "cstring-page-end","read-partial-offset","write-partial-offset","append-partial-offset",
        "read-unmap","write-unmap","clock-protection","read-fork-cow"};
    unsigned total=argc==3?13:6,passed=0;
    for(unsigned which=0;which<total;++which) {
        pid_t child=fork();if(child<0)return 2;if(!child)one_case(which,argv[1]);
        int status=0;if(waitpid(child,&status,0)!=child)return 2;
        int ok=WIFEXITED(status)&&!WEXITSTATUS(status);passed+=ok!=0;
        printf("USER_COPY_CASE %s passed=%d status=%d\n",names[which],ok,status);fflush(stdout);
    }
    printf("USER_COPY_RESULT %u/%u\n",passed,total);return passed==total?0:1;
}
