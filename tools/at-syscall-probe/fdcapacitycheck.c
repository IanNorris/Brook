#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

enum { COUNT=512, HIGH_FD=900 };
#define CHECK(test) do { if (!(test)) { fprintf(stderr,"FDCAPACITYCHECK FAIL line=%d errno=%d\n",__LINE__,errno); exit(1); } } while (0)
int main(void) {
    setvbuf(stdout,NULL,_IONBF,0);
    syscall(SYS_alarm,60);
    struct rlimit limit;
    CHECK(getrlimit(RLIMIT_NOFILE,&limit)==0 && limit.rlim_cur>=1024);
    int ep=epoll_create1(EPOLL_CLOEXEC);CHECK(ep>=0);
    int fds[COUNT]; struct pollfd polling[COUNT]; unsigned char seen[COUNT]={0};
    for (unsigned i=0;i<COUNT;++i) {
        fds[i]=eventfd(0,EFD_CLOEXEC|EFD_NONBLOCK);CHECK(fds[i]>=0);
        polling[i]=(struct pollfd){fds[i],POLLIN,0};
        struct epoll_event event={.events=EPOLLIN,.data.u32=i};
        CHECK(epoll_ctl(ep,EPOLL_CTL_ADD,fds[i],&event)==0);
    }
    CHECK(fds[COUNT-1]>256);
    int duplicate=fcntl(fds[0],F_DUPFD_CLOEXEC,HIGH_FD);CHECK(duplicate==HIGH_FD);
    CHECK(fcntl(duplicate,F_GETFD)&FD_CLOEXEC);
    CHECK(close(duplicate)==0);
    uint64_t one=1,value;
    for (unsigned i=0;i<COUNT;++i) CHECK(write(fds[i],&one,8)==8);
    CHECK(poll(polling,COUNT,0)==COUNT);
    for (unsigned i=0;i<COUNT;++i) CHECK(polling[i].revents==POLLIN);
    unsigned received=0;
    while (received<COUNT) {
        struct epoll_event events[64];
        int n=epoll_wait(ep,events,64,1000);CHECK(n>0);
        for (int i=0;i<n;++i) {
            unsigned id=events[i].data.u32;
            CHECK(id<COUNT && !seen[id] && events[i].events==EPOLLIN);
            seen[id]=1; ++received;
            CHECK(read(fds[id],&value,8)==8 && value==1);
        }
    }
    CHECK(poll(polling,COUNT,0)==0);
    CHECK(close(ep)==0);
    for (unsigned i=0;i<COUNT;++i) CHECK(close(fds[i])==0);
    printf("FDCAPACITYCHECK PASS descriptors=%u poll=%u epoll=%u high_fd=%u\n",COUNT,COUNT,received,HIGH_FD);
    return 0;
}
