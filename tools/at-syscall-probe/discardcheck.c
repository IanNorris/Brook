#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

enum { PAGE=4096, PAGES=4, WORKERS=8, ROUNDS=32 };
static unsigned failures, checks;
static atomic_uint reader_errors;
static unsigned char* shared;
static pthread_barrier_t phase;
#define CHECK(test) do { ++checks; if (!(test)) { ++failures; printf("FAIL line=%d errno=%d\n", __LINE__, errno); } } while (0)
static int filled(const unsigned char* p, size_t size, unsigned char value) {
    for (size_t i=0;i<size;++i) if (p[i]!=value) return 0;
    return 1;
}
static void* reader(void* unused) {
    (void)unused;
    for (unsigned round=0;round<ROUNDS;++round) {
        pthread_barrier_wait(&phase);
        if (!filled(shared,PAGES*PAGE,0x5a)) atomic_fetch_add(&reader_errors,1);
        pthread_barrier_wait(&phase);
        pthread_barrier_wait(&phase);
        if (!filled(shared,PAGES*PAGE,0)) atomic_fetch_add(&reader_errors,1);
        pthread_barrier_wait(&phase);
    }
    return NULL;
}
int main(void) {
    setvbuf(stdout,NULL,_IONBF,0);
    syscall(SYS_alarm,60);
    unsigned char* p=mmap(NULL,PAGES*PAGE,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if (p==MAP_FAILED) return 1;
    memset(p,0x5a,PAGES*PAGE);
    CHECK(madvise(p+PAGE,PAGE+1,MADV_DONTNEED)==0);
    CHECK(filled(p,PAGE,0x5a) && filled(p+PAGE,2*PAGE,0) && filled(p+3*PAGE,PAGE,0x5a));
    memset(p,0x5a,PAGES*PAGE);
    CHECK(mprotect(p,PAGES*PAGE,PROT_READ)==0);
    CHECK(madvise(p,PAGES*PAGE,MADV_DONTNEED)==0 && filled(p,PAGES*PAGE,0));
    CHECK(mprotect(p,PAGES*PAGE,PROT_READ|PROT_WRITE)==0);
    memset(p,0x5a,PAGES*PAGE);
    CHECK(mprotect(p,PAGES*PAGE,PROT_NONE)==0);
    CHECK(madvise(p,PAGES*PAGE,MADV_DONTNEED)==0);
    CHECK(mprotect(p,PAGES*PAGE,PROT_READ|PROT_WRITE)==0 && filled(p,PAGES*PAGE,0));
    memset(p,0x5a,PAGES*PAGE);
    int wake[2]; if (pipe(wake)) return 1;
    pid_t child=fork();
    if (!child) {
        close(wake[1]); char byte;
        _exit(read(wake[0],&byte,1)==1 && filled(p,PAGES*PAGE,0x5a)?0:1);
    }
    if (child<0) return 1;
    close(wake[0]);
    CHECK(madvise(p,PAGES*PAGE,MADV_DONTNEED)==0 && filled(p,PAGES*PAGE,0));
    CHECK(write(wake[1],"x",1)==1); close(wake[1]);
    int status=0;
    CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
    errno=0;CHECK(madvise(p+1,PAGE,MADV_DONTNEED)==-1 && errno==EINVAL);
    errno=0;CHECK(madvise(p,PAGE,0x7fffffff)==-1 && errno==EINVAL);
    CHECK(madvise(p,0,MADV_DONTNEED)==0);
    void* empty=mmap(NULL,1UL<<30,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    CHECK(empty!=MAP_FAILED);
    if (empty!=MAP_FAILED) {
        CHECK(madvise(empty,1UL<<30,MADV_DONTNEED)==0);
        CHECK(munmap(empty,1UL<<30)==0);
    }
    shared=p;
    pthread_t threads[WORKERS];
    if (pthread_barrier_init(&phase,NULL,WORKERS+1)) return 1;
    for (unsigned i=0;i<WORKERS;++i) if (pthread_create(&threads[i],NULL,reader,NULL)) return 1;
    for (unsigned round=0;round<ROUNDS;++round) {
        memset(shared,0x5a,PAGES*PAGE);
        pthread_barrier_wait(&phase);
        pthread_barrier_wait(&phase);
        CHECK(madvise(shared,PAGES*PAGE,MADV_DONTNEED)==0);
        pthread_barrier_wait(&phase);
        pthread_barrier_wait(&phase);
    }
    for (unsigned i=0;i<WORKERS;++i) CHECK(pthread_join(threads[i],NULL)==0);
    CHECK(atomic_load(&reader_errors)==0);
    CHECK(pthread_barrier_destroy(&phase)==0);
    CHECK(munmap(p,PAGES*PAGE)==0);
    printf("DISCARDCHECK %s checks=%u failures=%u reader_errors=%u rounds=%u\n",
        failures?"FAIL":"PASS",checks,failures,atomic_load(&reader_errors),ROUNDS);
    return failures?1:0;
}
