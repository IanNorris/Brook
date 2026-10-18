/* Owned bounded transport control for the Nix download/decompress/unpack
 * pipeline. No files, network, external programs, or host/guest-specific APIs.
 * Timings include verification and process lifecycle, not just pipe copying. */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

struct counts { uint64_t reads,writes,bytes; };
static unsigned char pattern[65536];
static uint64_t now_ns(void) {
    struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t)) {perror("clock_gettime");exit(1);}
    return (uint64_t)t.tv_sec*1000000000ULL+(uint64_t)t.tv_nsec;
}
static int write_all(int fd,const void* data,size_t size,struct counts* c) {
    size_t done=0;
    while(done<size) {
        ssize_t n=write(fd,(const char*)data+done,size-done);
        if(c)++c->writes;
        if(n<0&&errno==EINTR)continue;
        if(n<=0)return -1;
        done+=(size_t)n;
    }
    return 0;
}
static void produce(int out,int reports,uint64_t size,size_t chunk) {
    struct counts c={0};
    while(c.bytes<size) {
        size_t n=size-c.bytes<chunk?(size_t)(size-c.bytes):chunk;
        if(write_all(out,pattern,n,&c))_exit(2);
        c.bytes+=n;
    }
    close(out);
    int error=write_all(reports,&c,sizeof(c),NULL);close(reports);_exit(error?3:0);
}
static void relay(int in,int out,int reports,size_t chunk) {
    struct counts c={0};unsigned char data[65536];
    for(;;) {
        ssize_t n=read(in,data,chunk);++c.reads;
        if(n<0&&errno==EINTR)continue;
        if(n<0)_exit(4);
        if(!n)break;
        if(write_all(out,data,(size_t)n,&c))_exit(5);
        c.bytes+=(uint64_t)n;
    }
    close(in);close(out);
    int error=write_all(reports,&c,sizeof(c),NULL);close(reports);_exit(error?6:0);
}
static int run(uint64_t size,size_t chunk,int stages) {
    int first[2],second[2]={-1,-1},reports[2];
    if(pipe(first)||pipe(reports)||(stages==2&&pipe(second))){perror("pipe");return 1;}
    uint64_t begin=now_ns();pid_t producer=fork();
    if(producer<0){perror("fork producer");return 1;}
    if(!producer){close(first[0]);close(reports[0]);if(stages==2){close(second[0]);close(second[1]);}produce(first[1],reports[1],size,chunk);}
    pid_t forwarder=-1;
    if(stages==2) {
        forwarder=fork();if(forwarder<0){perror("fork relay");return 1;}
        if(!forwarder){close(first[1]);close(second[0]);close(reports[0]);relay(first[0],second[1],reports[1],chunk);}
    }
    close(first[1]);close(reports[1]);
    int input=first[0];if(stages==2){close(first[0]);close(second[1]);input=second[0];}
    uint64_t bytes=0,reads=0;int bad=0;unsigned char data[65536];
    for(;;) {
        ssize_t n=read(input,data,chunk);++reads;
        if(n<0&&errno==EINTR)continue;
        if(n<0){perror("read");bad=1;break;}
        if(!n)break;
        for(ssize_t i=0;i<n;++i)if(data[i]!=(unsigned char)((bytes+(uint64_t)i)*13+17))bad=1;
        bytes+=(uint64_t)n;
    }
    close(input);
    struct counts result[2]={{0}};size_t count=0;
    while(count<(size_t)stages*sizeof(struct counts)) {
        ssize_t n=read(reports[0],(char*)result+count,(size_t)stages*sizeof(struct counts)-count);
        if(n<0&&errno==EINTR)continue;
        if(n<=0)break;
        count+=(size_t)n;
    }
    close(reports[0]);int status=0;
    if(waitpid(producer,&status,0)!=producer||status)bad=1;
    if(stages==2&&(waitpid(forwarder,&status,0)!=forwarder||status))bad=1;
    if(bytes!=size||count!=(size_t)stages*sizeof(struct counts))bad=1;
    uint64_t writes=0,relay_reads=0;
    for(int i=0;i<stages;++i){if(result[i].bytes!=size)bad=1;writes+=result[i].writes;relay_reads+=result[i].reads;}
    uint64_t elapsed=now_ns()-begin;
    printf("PIPEPERF_RESULT {\"stages\":%d,\"chunk\":%zu,\"bytes\":%llu,\"elapsed_ns\":%llu,\"sink_reads\":%llu,\"relay_reads\":%llu,\"total_writes\":%llu,\"verified\":%s}\n",stages,chunk,(unsigned long long)bytes,(unsigned long long)elapsed,(unsigned long long)reads,(unsigned long long)relay_reads,(unsigned long long)writes,bad?"false":"true");
    fflush(stdout);return bad;
}
int main(int argc,char** argv) {
    unsigned long mib=8;
    if(argc==2){char* end=NULL;errno=0;mib=strtoul(argv[1],&end,10);if(errno||!end||*end||mib<1||mib>256){fputs("MiB must be 1..256\n",stderr);return 2;}}
    else if(argc!=1){fputs("Usage: pipecheck [MiB]\n",stderr);return 2;}
    for(size_t i=0;i<sizeof(pattern);++i)pattern[i]=(unsigned char)(i*13+17);
    for(int stages=1;stages<=2;++stages)for(size_t chunk=4096;chunk<=65536;chunk*=16)
        if(run((uint64_t)mib*1024*1024,chunk,stages))return 1;
    puts("PIPEPERF_PASS");return 0;
}
