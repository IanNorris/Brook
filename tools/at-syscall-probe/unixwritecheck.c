// Stream integrity and requested/accepted-byte cost for large short writev retries.
#define _POSIX_C_SOURCE 200809L
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint64_t now_ns(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC,&t)) { perror("clock_gettime"); exit(2); }
    return (uint64_t)t.tv_sec*1000000000u+(uint64_t)t.tv_nsec;
}
static unsigned char pattern(size_t n) { return (unsigned char)((n*17u+(n>>12))&255u); }
static int run(size_t quantum) {
    const size_t bytes=8u*1024u*1024u;
    unsigned char *buffer=malloc(bytes);
    if(!buffer) return 2;
    for(size_t i=0;i<bytes;++i) buffer[i]=pattern(i); // Prefault before timing.
    int pair[2];
    if(socketpair(AF_UNIX,SOCK_STREAM,0,pair)) { perror("socketpair"); free(buffer); return 2; }
    pid_t child=fork();
    if(child<0) { perror("fork"); close(pair[0]); close(pair[1]); free(buffer); return 2; }
    if(!child) {
        close(pair[0]); unsigned char part[65536]; size_t received=0;
        while(received<bytes) {
            ssize_t n=read(pair[1],part,sizeof(part));
            if(n<0 && errno==EINTR) continue;
            if(n<=0 || (size_t)n>bytes-received) _exit(3);
            for(ssize_t i=0;i<n;++i) if(part[i]!=pattern(received+(size_t)i)) _exit(4);
            received+=(size_t)n;
        }
        close(pair[1]); _exit(0);
    }
    close(pair[1]);
    int flags=fcntl(pair[0],F_GETFL);
    if(flags<0 || fcntl(pair[0],F_SETFL,flags|O_NONBLOCK)<0) { perror("fcntl"); close(pair[0]); waitpid(child,NULL,0); free(buffer); return 2; }
    size_t sent=0,max_returned=0; uint64_t calls=0,requested=0,eagain=0,write_ns=0;
    int error=0; uint64_t started=now_ns();
    while(sent<bytes) {
        size_t count=bytes-sent;
        if(count>quantum) count=quantum;
        size_t first=count<16?count:16;
        struct iovec vec[2]={{buffer+sent,first},{buffer+sent+first,count-first}};
        uint64_t begin=now_ns(); ssize_t n=writev(pair[0],vec,2); int saved=errno;
        write_ns+=now_ns()-begin; ++calls; requested+=count;
        if(n>0) {
            if((size_t)n>count) { error=5; break; }
            if((size_t)n>max_returned) max_returned=(size_t)n;
            sent+=(size_t)n; continue;
        }
        if(n<0 && saved==EINTR) continue;
        if(n<0 && (saved==EAGAIN || saved==EWOULDBLOCK)) {
            ++eagain; struct pollfd p={pair[0],POLLOUT,0}; int r;
            do { r=poll(&p,1,5000); } while(r<0 && errno==EINTR);
            if(r>0 && (p.revents&POLLOUT)) continue;
            error=6; break;
        }
        error=7; break;
    }
    uint64_t elapsed=now_ns()-started;
    close(pair[0]); int status=0; pid_t reaped;
    do { reaped=waitpid(child,&status,0); } while(reaped<0 && errno==EINTR);
    int receiver_ok=reaped==child && WIFEXITED(status) && WEXITSTATUS(status)==0;
    printf("UNIX_WRITE quantum=%zu bytes=%zu calls=%llu requested=%llu accepted=%zu max_returned=%zu eagain=%llu write_ns=%llu elapsed_ns=%llu receiver_ok=%d error=%d\n",
        quantum,bytes,(unsigned long long)calls,(unsigned long long)requested,sent,max_returned,
        (unsigned long long)eagain,(unsigned long long)write_ns,(unsigned long long)elapsed,receiver_ok,error);
    free(buffer); return error || sent!=bytes || !receiver_ok;
}
int main(int argc,char **argv) {
    alarm(30); setvbuf(stdout,NULL,_IONBF,0);
    if(argc!=2 || (strcmp(argv[1],"large") && strcmp(argv[1],"chunk32k"))) return 2;
    int code=run(!strcmp(argv[1],"large")?8u*1024u*1024u:32768u);
    if(!code) puts("UNIX_WRITE_PASS");
    return code;
}
