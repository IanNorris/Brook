#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Reuse io-bench's retained 64+ MiB fixture; each process probes all files. */
static unsigned char expected(unsigned id, uint64_t off) {
    if ((off>=12*4096ULL-32 && off<12*4096ULL+65) ||
        (off>=(12+1024)*4096ULL-32 && off<(12+1024)*4096ULL+65)) return 0xa0+id;
    uint64_t key=(((uint64_t)id<<32)^(off>>12))*0x9e3779b97f4a7c15ULL+0xd1b54a32d192ed03ULL;
    return (key>>((off&7)*8))^((off&4095)*13);
}
static void check(const unsigned char *p, unsigned id, uint64_t off, const char *kind) {
    for (unsigned i=0;i<4096;i++) if(p[i]!=expected(id,off+i)) {
        fprintf(stderr,"CACHECHECK FAIL %s file=%u offset=%llu got=%u expected=%u\n",kind,id,(unsigned long long)(off+i),p[i],expected(id,off+i));
        exit(2);
    }
}
static void child(const char *root, unsigned id, unsigned workers, unsigned mib) {
    int fd[16]; char path[256]; unsigned char buffer[4096];
    for(unsigned i=0;i<workers;i++) {
        snprintf(path,sizeof(path),"%s/w%u/bulk",root,i);
        fd[i]=open(path,O_RDONLY); if(fd[i]<0){perror(path);exit(2);}
    }
    uint32_t random=0x12345678+id*7919;
    for(unsigned round=0;round<1024;round++) {
        random=random*1664525+1013904223;
        unsigned target=(round+id*3)%workers;
        uint64_t off=(uint64_t)((random>>8)%(mib*256))*4096;
        ssize_t n; do {n=pread(fd[target],buffer,4096,off);} while(n<0&&errno==EINTR);
        if(n!=4096){perror("cachecheck pread");exit(2);} check(buffer,target,off,"pread");
        random=random*1664525+1013904223;
        off=(uint64_t)((random>>8)%(mib*256))*4096;
        unsigned char *p=mmap(NULL,4096,PROT_READ,MAP_PRIVATE,fd[target],off);
        if(p==MAP_FAILED){perror("cachecheck mmap");exit(2);} check(p,target,off,"mmap");
        if(munmap(p,4096)){perror("cachecheck munmap");exit(2);}
    }
    for(unsigned i=0;i<workers;i++)close(fd[i]);
    exit(0);
}
int main(int argc,char **argv) {
    setbuf(stdout,NULL);
    if(argc!=4)return 2;
    unsigned workers=strtoul(argv[2],NULL,10),mib=strtoul(argv[3],NULL,10);
    if(workers<8||workers>16||mib<8||mib>128)return 2;
    struct timespec start,end;
    if(clock_gettime(CLOCK_MONOTONIC,&start))return 2;
    pid_t pids[16];
    for(unsigned i=0;i<workers;i++) {
        pids[i]=fork(); if(pids[i]<0){perror("cachecheck fork");return 2;}
        if(!pids[i])child(argv[1],i,workers,mib);
    }
    int failed=0;
    for(unsigned i=0;i<workers;i++) {
        int status;pid_t got;
        do{got=waitpid(pids[i],&status,0);}while(got<0&&errno==EINTR);
        if(got!=pids[i]||!WIFEXITED(status)||WEXITSTATUS(status))failed=1;
    }
    if(clock_gettime(CLOCK_MONOTONIC,&end))return 2;
    uint64_t elapsed=(uint64_t)(end.tv_sec-start.tv_sec)*1000000000ULL+end.tv_nsec-start.tv_nsec;
    printf("CACHECHECK_RESULT {\"processes\":%u,\"bytes\":%llu,\"elapsed_ns\":%llu}\n",workers,(unsigned long long)workers*1024*8192,(unsigned long long)elapsed);
    printf("CACHECHECK %s processes=%u pages=%u\n",failed?"FAIL":"PASS",workers,workers*2048);
    return failed?2:0;
}
