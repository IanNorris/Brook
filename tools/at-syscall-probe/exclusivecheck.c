#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures, winners, losers;
static pthread_barrier_t barrier;
static char race[512];
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"EXCLUSIVECHECK FAIL line=%d %s errno=%d\n",__LINE__,#c,errno); ++failures; } } while(0)
static void* creator(void* unused) {
    (void)unused; pthread_barrier_wait(&barrier);
    int fd=open(race,O_WRONLY|O_CREAT|O_EXCL|O_TRUNC,0600);
    if(fd>=0) {
        __atomic_add_fetch(&winners,1,__ATOMIC_RELAXED);
        if(write(fd,"winner",6)!=6)__atomic_add_fetch(&failures,1,__ATOMIC_RELAXED);
        close(fd);
    } else if(errno==EEXIST)__atomic_add_fetch(&losers,1,__ATOMIC_RELAXED);
    else __atomic_add_fetch(&failures,1,__ATOMIC_RELAXED);
    return NULL;
}
static void rejected(const char* path) {
    errno=0; int fd=open(path,O_WRONLY|O_CREAT|O_EXCL|O_TRUNC,0600);
    CHECK(fd<0&&errno==EEXIST); if(fd>=0)close(fd);
}
int main(int argc,char**argv) {
    if(argc<2) return 2;
    char dir[512],path[512],link[512],absent[512],parent[512],via[512];
    if(snprintf(dir,sizeof(dir),"%s/exclusive-XXXXXX",argv[1])>=(int)sizeof(dir))return 2;
    if(!mkdtemp(dir)){perror("mkdtemp");return 1;}
#define NAME(out,suffix) CHECK(snprintf(out,sizeof(out),"%s/%s",dir,suffix)<(int)sizeof(out))
    NAME(path,"file");NAME(race,"race");NAME(link,"link");NAME(absent,"absent");NAME(parent,"parent");NAME(via,"parent/new");
    int fd=open(path,O_RDWR|O_CREAT|O_EXCL,0600);CHECK(fd>=0);
    if(fd>=0){CHECK(write(fd,"original",8)==8);close(fd);}
    rejected(path); rejected(dir);
    if (!strcmp(argv[1],"/boot")) rejected(path+5); // Legacy boot alias preserves EEXIST.
    char data[16]={0};fd=open(path,O_RDONLY|O_EXCL);CHECK(fd>=0);
    if(fd>=0){CHECK(read(fd,data,sizeof(data))==8&&!memcmp(data,"original",8));close(fd);}
    if(argc<3||strcmp(argv[2],"--no-symlink")) {
        CHECK(symlink(absent,link)==0);rejected(link);
        struct stat st;CHECK(lstat(absent,&st)<0&&errno==ENOENT);
        CHECK(unlink(link)==0);CHECK(symlink(path,link)==0);rejected(link);CHECK(unlink(link)==0);
        CHECK(symlink(dir,parent)==0);
        fd=open(via,O_WRONLY|O_CREAT|O_EXCL,0600);CHECK(fd>=0);if(fd>=0)close(fd);rejected(via);
    }
    for(unsigned round=0;round<16;++round) {
        unlink(race);winners=losers=0;pthread_t threads[8];
        CHECK(!pthread_barrier_init(&barrier,NULL,8));
        for(unsigned i=0;i<8;++i)if(pthread_create(&threads[i],NULL,creator,NULL))exit(2);
        for(unsigned i=0;i<8;++i)CHECK(!pthread_join(threads[i],NULL));
        CHECK(!pthread_barrier_destroy(&barrier));CHECK(winners==1&&losers==7);
        fd=open(race,O_RDONLY);CHECK(fd>=0);
        if(fd>=0){CHECK(read(fd,data,sizeof(data))==6&&!memcmp(data,"winner",6));close(fd);}
    }
    printf("EXCLUSIVECHECK %s rounds=16 threads=8 failures=%d path=%s\n",failures?"FAIL":"PASS",failures,dir);
    return failures!=0;
}
