#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* Brook-only syscall 519. Run in a disposable guest, never as a Linux test. */
static int failures;
#define CHECK(x) do { if (!(x)) { printf("CURSORCHECK FAIL line=%d errno=%d: %s\n",__LINE__,errno,#x); ++failures; } } while (0)
static long upload(const void* p, unsigned w, unsigned h, int x, int y)
{ return syscall(519,p,w,h,x,y,0); }
static void* race_page;
static unsigned mutation_failures;
static void* mutate(void* ignored)
{
    (void)ignored;
    for (int i=0;i<200;++i) {
        if (munmap(race_page,4096)) __atomic_fetch_add(&mutation_failures,1,__ATOMIC_RELAXED);
        sched_yield();
        if (mmap(race_page,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0)!=race_page)
            __atomic_fetch_add(&mutation_failures,1,__ATOMIC_RELAXED);
    }
    return NULL;
}
static int show(void)
{
    uint32_t p[24*24]={0};
    for (unsigned y=0;y<20;++y) for (unsigned x=0;x<8;++x)
        if (x<=y/2) p[y*24+x]=x==0||x==y/2?0xff102030:0xff00ddaa;
    CHECK(upload(p,24,24,3,5)==0);
    return failures!=0;
}
int main(int argc,char** argv)
{
    if (argc>1 && !strcmp(argv[1],"show")) return show();
    if (argc>1 && !strcmp(argv[1],"reset")) return upload(NULL,0,0,0,0)!=0;
    CHECK(upload(NULL,0,0,0,0)==0);
    errno=0;CHECK(upload(NULL,65,1,0,0)==-1&&errno==EINVAL);
    errno=0;CHECK(upload(NULL,8,8,0,0)==-1&&errno==EFAULT);
    void* denied=mmap(NULL,4096,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    CHECK(denied!=MAP_FAILED);
    if(denied!=MAP_FAILED){errno=0;CHECK(upload(denied,16,16,0,0)==-1&&errno==EFAULT);munmap(denied,4096);}
    unsigned char* pages=mmap(NULL,5*4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    CHECK(pages!=MAP_FAILED);
    if(pages!=MAP_FAILED){
        for(unsigned i=0;i<5;++i)CHECK(mmap(pages+i*4096,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0)==pages+i*4096);
        memset(pages+17,0,64*64*4);
        CHECK(upload(pages+17,64,64,0,0)==0);
        munmap(pages,5*4096);
    }
    race_page=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    CHECK(race_page!=MAP_FAILED);
    unsigned accepted=0,rejected=0;
    if(race_page!=MAP_FAILED){
        pthread_t worker;int created=pthread_create(&worker,NULL,mutate,NULL);
        CHECK(created==0);
        if(!created){
            for(int i=0;i<500;++i){errno=0;long r=upload(race_page,16,16,0,0);if(!r)++accepted;else if(errno==EFAULT)++rejected;else CHECK(0);}
            CHECK(pthread_join(worker,NULL)==0);CHECK(mutation_failures==0);
        }
        munmap(race_page,4096);
    }
    CHECK(upload(NULL,0,0,0,0)==0);
    printf("CURSORCHECK %s accepted=%u rejected=%u failures=%d\n",failures?"FAIL":"PASS",accepted,rejected,failures);
    return failures!=0;
}
