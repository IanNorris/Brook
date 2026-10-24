/* Known-data microbenchmark for Steam-sized pools; no application data. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
static char owned[3][160];
static int have_path[3];
static void cleanup(void){for(unsigned i=0;i<3;++i)if(have_path[i])unlink(owned[i]);}
static void expired(int sig){(void)sig;for(unsigned i=0;i<3;++i)if(have_path[i])unlink(owned[i]);_exit(124);}
#define CHECK(x) do{if(!(x)){fprintf(stderr,"POOL_BENCH_FAIL line=%d errno=%d\n",__LINE__,errno);exit(1);}}while(0)
static uint64_t stamp(void){struct timespec t;CHECK(!clock_gettime(CLOCK_MONOTONIC,&t));return (uint64_t)t.tv_sec*1000000000ULL+t.tv_nsec;}
static void measure(size_t bytes,int shared,unsigned round){
    int fd=-1;uint64_t begin=stamp();
    if(shared){snprintf(owned[0],sizeof(owned[0]),"/dev/shm/brook-pool-bench-%ld-%u-%zu",(long)getpid(),round,bytes);fd=open(owned[0],O_CREAT|O_EXCL|O_RDWR,0600);CHECK(fd>=0);have_path[0]=1;CHECK(!ftruncate(fd,bytes));}
    uint64_t sized=stamp();void*p=mmap(NULL,bytes,PROT_READ|PROT_WRITE,shared?MAP_SHARED:MAP_PRIVATE|MAP_ANONYMOUS,fd,0);CHECK(p!=MAP_FAILED);uint64_t mapped=stamp();
    memset(p,0,bytes);uint64_t cold=stamp();
    /* Volatile stores and explicit last-byte checks retain both timed writes. */
    CHECK(*(volatile unsigned char*)p==0&&*((volatile unsigned char*)p+bytes-1)==0);
    memset(p,0x5a,bytes);uint64_t hot=stamp();CHECK(*(volatile unsigned char*)p==0x5a&&*((volatile unsigned char*)p+bytes-1)==0x5a);
    if(shared){unsigned char last;CHECK(pread(fd,&last,1,(off_t)bytes-1)==1&&last==0x5a);}
    CHECK(!munmap(p,bytes));if(shared){CHECK(!close(fd)&&!unlink(owned[0]));have_path[0]=0;}
    uint64_t done=stamp();
    printf("{\"pool_benchmark\":true,\"round\":%u,\"shared_file\":%s,\"bytes\":%zu,\"size_ns\":%llu,\"map_ns\":%llu,\"cold_write_ns\":%llu,\"hot_write_ns\":%llu,\"cleanup_ns\":%llu}\n",round,shared?"true":"false",bytes,(unsigned long long)(sized-begin),(unsigned long long)(mapped-sized),(unsigned long long)(cold-mapped),(unsigned long long)(hot-cold),(unsigned long long)(done-hot));
}
static void pair(unsigned round){
    int fd[2];void*p[2];uint64_t ns[2][4];const size_t n=26214416;
    for(unsigned i=0;i<2;++i){unsigned slot=i+1;uint64_t t=stamp();snprintf(owned[slot],sizeof(owned[slot]),"/dev/shm/brook-pool-pair-%ld-%u-%u",(long)getpid(),round,i);fd[i]=open(owned[slot],O_CREAT|O_EXCL|O_RDWR,0600);CHECK(fd[i]>=0);have_path[slot]=1;CHECK(!ftruncate(fd[i],n));uint64_t sized=stamp();
        p[i]=mmap(NULL,n,PROT_READ|PROT_WRITE,MAP_SHARED,fd[i],0);CHECK(p[i]!=MAP_FAILED);uint64_t mapped=stamp();memset(p[i],0,n);uint64_t cold=stamp();CHECK(*((volatile unsigned char*)p[i]+n-1)==0);ns[i][0]=sized-t;ns[i][1]=mapped-sized;ns[i][2]=cold-mapped;}
    for(unsigned i=0;i<2;++i){uint64_t t=stamp();memset(p[i],0x6b,n);ns[i][3]=stamp()-t;unsigned char last;CHECK(pread(fd[i],&last,1,n-1)==1&&last==0x6b);CHECK(!munmap(p[i],n)&&!close(fd[i])&&!unlink(owned[i+1]));have_path[i+1]=0;}
    for(unsigned i=0;i<2;++i)printf("{\"pool_pair_benchmark\":true,\"round\":%u,\"pool\":%u,\"bytes\":%zu,\"size_ns\":%llu,\"map_ns\":%llu,\"cold_write_ns\":%llu,\"hot_write_ns\":%llu}\n",round,i,n,(unsigned long long)ns[i][0],(unsigned long long)ns[i][1],(unsigned long long)ns[i][2],(unsigned long long)ns[i][3]);
}
int main(int argc,char**argv){
    CHECK(argc==1||(argc==2&&!strcmp(argv[1],"--repeat")));CHECK(!atexit(cleanup));struct sigaction a={0};a.sa_handler=expired;CHECK(!sigaction(SIGALRM,&a,NULL));alarm(30);setbuf(stdout,NULL);
    for(unsigned r=0;r<(argc==2?4u:1u);++r){measure(8208,1,r);measure(8*1024*1024,1,r);measure(26214416,1,r);measure(26214416,0,r);pair(r);}
    alarm(0);return 0;
}
