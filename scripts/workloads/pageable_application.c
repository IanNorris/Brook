#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
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
static void clock_test(void){
    enum {Pages=24};
    int fd=open("/tmp/application-clock",O_CREAT|O_TRUNC|O_RDWR,0600);assert(fd>=0 && unlink("/tmp/application-clock")==0);
    unsigned char* p=mmap(0,Pages*4096,3,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(p!=MAP_FAILED);
    for(unsigned i=0;i<Pages;++i)p[i*4096]=(unsigned char)(i+1);
    for(unsigned i=0;i<Pages;++i)assert(ctl(24,(long)(uintptr_t)(p+i*4096))==1);
    for(unsigned i=0;i<Pages;++i)assert(ctl(24,(long)(uintptr_t)(p+i*4096))==0);
    assert(((volatile unsigned char*)p)[7*4096]==8);
    assert(ctl(10,0)==4096 && ctl(13,2)==1);
    assert(((volatile unsigned char*)p)[0]==1 && ctl(13,2)==0);
    assert(ctl(24,(long)(uintptr_t)p)==1 && ctl(24,(long)(uintptr_t)(p+7*4096))==1);
    // A pending clear must still be acknowledged when PageOut fails before
    // its detach transaction (and therefore supplies no acknowledgement).
    for(unsigned i=0;i<Pages;++i){long old=ctl(24,(long)(uintptr_t)(p+i*4096));assert(old==0 || old==1);}
    assert(((volatile unsigned char*)p)[4096]==2);
    long flushes=ctl(22,15);assert(ctl(4,8)==0);
    errno=0;assert(ctl(10,0)==-1 && errno==ENOMEM && ctl(22,15)==flushes+1);
    assert(ctl(24,(long)(uintptr_t)(p+4096))==0);
    assert(((volatile unsigned char*)p)[4096]==2 && ctl(24,(long)(uintptr_t)(p+4096))==1);
    for(unsigned i=0;i<Pages;++i)assert(((volatile unsigned char*)p)[i*4096]==i+1);
    long passes=ctl(22,16);
    assert(ctl(10,0)==4096 && ctl(22,16)==passes+1 && ctl(13,2)==1);
    // Incompressible data forces device I/O in both backing configurations.
    uint32_t random=0x43b3291u;
    for(unsigned i=0;i<Pages*4096;++i){random^=random<<13;random^=random>>17;random^=random<<5;p[i]=(unsigned char)random;}
    assert(ctl(4,1)==0);errno=0;assert(ctl(10,0)==-1 && errno==EIO && ctl(13,2)==0);
    random=0x43b3291u;
    for(unsigned i=0;i<Pages*4096;++i){random^=random<<13;random^=random>>17;random^=random<<5;assert(p[i]==(unsigned char)random);}
    for(unsigned i=0;i<Pages;++i)assert(ctl(1,(long)(uintptr_t)(p+i*4096))==4096);
    for(unsigned i=0;i<8;++i){assert(ctl(8,1)==0);(void)((volatile unsigned char*)p)[i*4096];}
    assert(ctl(7,0)==0 && ctl(13,2)==Pages-8);
    assert(ctl(8,1)==0);errno=0;assert(write(fd,p,Pages*4096)==-1 && errno==EFAULT);
    assert(ctl(6,0)==0 && ctl(13,2)==Pages-8 && lseek(fd,0,SEEK_CUR)==0);
    assert(munmap(p,Pages*4096)==0 && ctl(2,0)==0 && ctl(6,0)==0 && close(fd)==0);
    puts("APPLICATION_CLOCK_PASS access_reassertion cold_victim hot_second_chance two_pass_progress early_failure_ACK write_failure all_leased cleanup");
}
int main(int argc,char** argv){
    if(argc==2 && !strcmp(argv[1],"--reset")){
        errno=0;assert(ctl(9,1)==-1 && errno==EOPNOTSUPP);
        puts("APPLICATION_EXEC_RESET_PASS");return 0;
    }
    if(argc==2 && !strcmp(argv[1],"--clock")){clock_test();return 0;}
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
