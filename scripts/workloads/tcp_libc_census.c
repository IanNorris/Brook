/* Optional LD_PRELOAD census for one bounded diagnostic workload.
 * Records counts and successful byte totals by libc wrapper and descriptor;
 * it does not inspect buffers, paths, endpoints, or vector contents.
 * Calls that bypass interposition are invisible. Not for production use.
 */
#define _GNU_SOURCE
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

enum { FD_BUCKETS=64, METHODS=5 };
struct counter { _Atomic uint64_t calls, bytes, errors; };
static struct counter counters[METHODS][FD_BUCKETS+1];
static const char *const names[METHODS]={"write","writev","send","sendto","sendmsg"};

static void observe(unsigned method,int fd,ssize_t result) {
    unsigned bucket=fd>=0 && fd<FD_BUCKETS ? (unsigned)fd : FD_BUCKETS;
    struct counter *c=&counters[method][bucket];
    atomic_fetch_add_explicit(&c->calls,1,memory_order_relaxed);
    if(result>0) atomic_fetch_add_explicit(&c->bytes,(uint64_t)result,memory_order_relaxed);
    else if(result<0) atomic_fetch_add_explicit(&c->errors,1,memory_order_relaxed);
}
ssize_t write(int fd,const void *buf,size_t size) {
    ssize_t r=syscall(SYS_write,fd,buf,size);observe(0,fd,r);return r;
}
ssize_t writev(int fd,const struct iovec *iov,int count) {
    ssize_t r=syscall(SYS_writev,fd,iov,count);observe(1,fd,r);return r;
}
ssize_t send(int fd,const void *buf,size_t size,int flags) {
    ssize_t r=syscall(SYS_sendto,fd,buf,size,flags,NULL,0);observe(2,fd,r);return r;
}
ssize_t sendto(int fd,const void *buf,size_t size,int flags,const struct sockaddr *addr,socklen_t len) {
    ssize_t r=syscall(SYS_sendto,fd,buf,size,flags,addr,len);observe(3,fd,r);return r;
}
ssize_t sendmsg(int fd,const struct msghdr *msg,int flags) {
    ssize_t r=syscall(SYS_sendmsg,fd,msg,flags);observe(4,fd,r);return r;
}
__attribute__((destructor)) static void report(void) {
    for(unsigned method=0;method<METHODS;++method) for(unsigned fd=0;fd<=FD_BUCKETS;++fd) {
        struct counter *c=&counters[method][fd];uint64_t calls=atomic_load_explicit(&c->calls,memory_order_relaxed);
        if(!calls)continue;
        char line[160];int n=snprintf(line,sizeof(line),"TCP_LIBC_CENSUS method=%s fd=%u calls=%llu bytes=%llu errors=%llu\n",
            names[method],fd,(unsigned long long)calls,
            (unsigned long long)atomic_load_explicit(&c->bytes,memory_order_relaxed),
            (unsigned long long)atomic_load_explicit(&c->errors,memory_order_relaxed));
        if(n>0 && (size_t)n<sizeof(line)) (void)syscall(SYS_write,2,line,(size_t)n);
    }
}
