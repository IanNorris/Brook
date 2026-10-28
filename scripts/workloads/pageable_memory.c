// Restricted native paging integrity test. Compile as a static musl program.
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static long control(long op,long value) { return syscall(0xB40,op,value,0L,0L,0L,0L); }
static unsigned char pattern(unsigned page,unsigned at) { return (unsigned char)(page*31+at*73+(at>>8)); }
static void verify(unsigned char* p,unsigned page) {
    for(unsigned j=0;j<4096;++j)assert(p[j]==pattern(page,j));
}
static void expect_io_error(long value) { assert(value==-1 && errno==EIO); }
static void* should_not_start(void* ignored) { (void)ignored;_exit(97);return 0; }
static void* normal_thread(void* ignored) { return ignored; }
static volatile sig_atomic_t signal_seen,signal_on_altstack;
static uintptr_t alt_begin,alt_end;
static void on_alarm(int signal) {
    char local;(void)signal;uintptr_t address=(uintptr_t)&local;
    signal_on_altstack=address>=alt_begin && address<alt_end;signal_seen=1;
}
int main(int argc,char** argv) {
    enum { Pages=128 };const size_t bytes=Pages*4096;
    if(argc==2 && !strcmp(argv[1],"--disabled")) {
        assert(control(2,0)==-1 && errno==EOPNOTSUPP);
        assert(control(0,4096)==-1 && errno==EOPNOTSUPP);
        pthread_t t;void* result=0;
        assert(pthread_create(&t,0,normal_thread,(void*)123)==0);
        assert(pthread_join(t,&result)==0 && result==(void*)123);return 0;
    }
    if(argc==2 && !strcmp(argv[1],"--denied")) {
        assert(control(2,0)==-1 && errno==EPERM);return 0;
    }
    if(argc==2 && !strcmp(argv[1],"--ever-shared")) {
        pthread_t t;void* result=0;
        assert(pthread_create(&t,0,normal_thread,(void*)123)==0);
        assert(pthread_join(t,&result)==0 && result==(void*)123);
        assert(control(0,4096)==-1 && errno==EBUSY);return 0;
    }
    long baseline=control(2,0);assert(baseline>=0);
    if(argc==2 && !strcmp(argv[1],"--full-store")) {
        // This control uses the documented 4 MiB / 1024-slot backing fixture.
        assert(baseline==0);long base=control(0,1025*4096L);assert(base>0);
        unsigned char* memory=(unsigned char*)(uintptr_t)base;
        for(unsigned i=0;i<1025;++i)for(unsigned j=0;j<4096;++j)memory[i*4096+j]=pattern(i,j);
        for(unsigned i=0;i<1024;++i)assert(control(1,base+i*4096)==4096);
        assert(control(2,0)==1024);
        assert(control(1,base+1024*4096L)==-1 && errno==ENOSPC);
        verify(memory+1024*4096,1024);verify(memory,0);
        assert(control(2,0)==1023);assert(control(1,base+1024*4096L)==4096);
        assert(munmap(memory,1025*4096)==0);assert(control(2,0)==0);
        puts("PAGEABLE_FULL_STORE_PASS capacity=1024 ENOSPC_preserves_page refault_releases_slot teardown=clean");return 0;
    }
    long address=control(0,bytes);assert(address>0);
    unsigned char* p=(unsigned char*)(uintptr_t)address;
    for(unsigned i=0;i<Pages;++i)for(unsigned j=0;j<4096;++j)p[i*4096+j]=pattern(i,j);
    long before=control(3,0);
    for(unsigned i=0;i<Pages;++i)assert(control(1,address+i*4096)==4096);
    // PMM intentionally quarantines retired frames until its independent
    // all-CPU barrier worker runs. Wait in userspace without a prefault syscall.
    for(unsigned i=0;i<2000000;++i)__asm__ volatile("pause":::"memory");
    long after=control(3,0),stored=control(2,0);
    assert(stored==baseline+Pages);
    if(argc==2 && !strcmp(argv[1],"--exit-paged"))_exit(0);
    // No ordinary syscall intervenes: each read must take a real user #PF.
    for(unsigned i=0;i<Pages;++i)verify(p+i*4096,i);
    assert(control(2,0)==baseline);
    for(long failure=1;failure<=6;++failure) {
        if(failure==1 || failure==4 || failure==6) {
            assert(control(4,failure)==0);expect_io_error(control(1,address));
            verify(p,0);assert(control(2,0)==baseline);
        } else {
            assert(control(1,address)==4096);assert(control(4,failure)==0);
            expect_io_error(syscall(SYS_getpid));
            assert(control(2,0)==baseline+1);
            assert(syscall(SYS_getpid)>0);verify(p,0);assert(control(2,0)==baseline);
        }
    }
    // Numeric-only operations reject unsupported splits/permissions/remaps
    // without restoring evicted pages or leaking their slots.
    assert(control(1,address)==4096);
    assert(munmap(p,4096)==-1 && errno==EOPNOTSUPP);
    assert(mprotect(p,bytes,PROT_READ)==-1 && errno==EOPNOTSUPP);
    assert(syscall(SYS_mremap,address,bytes,bytes*2,1L,0L)==-1 && errno==EOPNOTSUPP);
    assert(control(2,0)==baseline+1);
    // Raw syscall output must be populated at safe syscall entry, not in #PF
    // under an unknown handler lock. clock_gettime writes into this page.
    assert(syscall(SYS_clock_gettime,1,p)==0);assert(control(2,0)==baseline);
    for(unsigned j=0;j<4096;++j)p[j]=pattern(0,j);
    // Fork is rejected for the entire opted-in AS (until exec).
    assert(syscall(SYS_fork)==-1);
    pthread_t thread;assert(pthread_create(&thread,0,should_not_start,0)!=0);
    assert(control(1,address)==4096);
    assert(munmap(p,bytes)==0);assert(control(2,0)==baseline);
    long alternate=control(0,16384);assert(alternate>0);
    alt_begin=(uintptr_t)alternate;alt_end=alt_begin+16384;
    stack_t stack={.ss_sp=(void*)alt_begin,.ss_size=16384,.ss_flags=0};assert(sigaltstack(&stack,0)==0);
    struct sigaction action;memset(&action,0,sizeof(action));action.sa_handler=on_alarm;action.sa_flags=SA_ONSTACK;
    sigemptyset(&action.sa_mask);assert(sigaction(SIGALRM,&action,0)==0);
    const pid_t self=getpid();
    for(unsigned i=0;i<4;++i)assert(control(1,alternate+i*4096)==4096);
    assert(control(2,0)==baseline+4);
    // Signal syscalls carry only numeric IDs and are safe barrier exemptions.
    // This queues delivery with the alternate stack still entirely OnDisk.
    assert(syscall(SYS_tgkill,self,self,SIGALRM)==0);
    if(!signal_seen || !signal_on_altstack)fprintf(stderr,"signal_seen=%d on_altstack=%d\n",signal_seen,signal_on_altstack);
    assert(signal_seen && signal_on_altstack);
    stack.ss_flags=SS_DISABLE;assert(sigaltstack(&stack,0)==0);
    assert(munmap((void*)alt_begin,16384)==0);assert(control(2,0)==baseline);
    printf("PAGEABLE_MEMORY_PASS pages=%d stored=%ld free_before=%ld free_after_pageout=%ld delta=%ld failures=6 teardown=clean\n",
           Pages,stored,before,after,after-before);
    return 0;
}
