#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
static long ctl(long op,long value){return syscall(0xB40,op,value,0L,0L,0L,0L);}
static void* thread(void* arg){return arg;}
static void reap(pid_t child){int status;assert(waitpid(child,&status,0)==child && status==0);}
int main(int argc,char** argv){
    if(argc==2 && !strcmp(argv[1],"--reset")){
        errno=0;assert(ctl(9,1)==-1 && errno==EOPNOTSUPP);
        puts("APPLICATION_EXEC_RESET_PASS");return 0;
    }
    long before=ctl(22,0);assert(before>=0);
    if(argc==2 && !strcmp(argv[1],"--shared")){
        pthread_t worker;void* result;assert(pthread_create(&worker,0,thread,(void*)42)==0);
        assert(pthread_join(worker,&result)==0 && result==(void*)42);
        unsigned char* p=mmap(0,65536,3,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(p!=MAP_FAILED);
        p[0]=7;assert(ctl(22,0)==before && ctl(13,0)==0 && ctl(13,1)==0);
        assert(munmap(p,65536)==0);puts("APPLICATION_EVER_SHARED_FALLBACK_PASS");return 0;
    }
    assert(argc==1);
    void* ordinary=mmap(0,4096,3,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(ordinary!=MAP_FAILED);
    assert(ctl(22,0)==before && munmap(ordinary,4096)==0);
    void* hint=(void*)0x2800000000ULL;
    unsigned char* p=mmap(hint,65536,3,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(p==hint);
    assert(ctl(22,0)==before+1 && ctl(13,1)==16 && ctl(13,0)==0);
    static const unsigned char code[]={0xb8,42,0,0,0,0xc3};memcpy(p,code,sizeof(code));
    assert(ctl(1,(long)(uintptr_t)p)==4096);
    assert(mprotect(p,65536,PROT_READ|PROT_EXEC)==0);
    int (*call)(void)=(void*)p;assert(call()==42);
    pid_t child=fork();assert(child>=0);
    if(!child){
        assert(mprotect(p,65536,3)==0);p[1]=43;
        assert(mprotect(p,65536,PROT_READ|PROT_EXEC)==0);
        assert(ctl(1,(long)(uintptr_t)p)==4096 && call()==43);
        assert(munmap(p,65536)==0);
        execl(argv[0],argv[0],"--reset",(char*)0);_exit(99);
    }
    reap(child);assert(call()==42);
    assert(mprotect(p,65536,PROT_NONE)==0 && mprotect(p,65536,PROT_READ|PROT_EXEC)==0 && call()==42);
    assert(munmap(p,65536)==0 && ctl(6,0)==0);
    puts("APPLICATION_ENROLLMENT_PASS hint lazy_zero maximum_RX stored_instruction_fault fork_COW isolation exec_reset");
}
