#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

/* Each byte carries one independent reference to the same mapped memfd.
 * Concurrent receivers must neither duplicate nor lose those references. */
static int sockets[2], backing;
static atomic_int failures, bytes, handles;
static void *receiver(void *unused) {
    (void)unused;
    for (;;) {
        char byte;
        union { struct cmsghdr align; char bytes[CMSG_SPACE(8*sizeof(int))]; } control;
        struct iovec iov={&byte,1};
        struct msghdr message={.msg_iov=&iov,.msg_iovlen=1,
            .msg_control=control.bytes,.msg_controllen=sizeof(control)};
        ssize_t n=recvmsg(sockets[1],&message,MSG_CMSG_CLOEXEC);
        if(n<0) { if(errno==EINTR)continue; atomic_fetch_add(&failures,1);break; }
        if(!n)break;
        atomic_fetch_add(&bytes,n);
        int delivered=0;
        for(struct cmsghdr *c=CMSG_FIRSTHDR(&message);c;c=CMSG_NXTHDR(&message,c)) {
            if(c->cmsg_level!=SOL_SOCKET||c->cmsg_type!=SCM_RIGHTS) {atomic_fetch_add(&failures,1);continue;}
            int *fds=(int*)CMSG_DATA(c);
            size_t count=(c->cmsg_len-CMSG_LEN(0))/sizeof(int);
            for(size_t i=0;i<count;++i) {
                unsigned char value=0;
                if(pread(fds[i],&value,1,0)!=1||value!=0x5a)atomic_fetch_add(&failures,1);
                if(close(fds[i]))atomic_fetch_add(&failures,1);
                ++delivered;
            }
        }
        atomic_fetch_add(&handles,delivered);
        if(delivered!=1||(message.msg_flags&MSG_CTRUNC))atomic_fetch_add(&failures,1);
    }
    return NULL;
}
int main(int argc,char **argv) {
    syscall(SYS_alarm,30);
    int workers=argc==2&&!strcmp(argv[1],"--concurrent")?4:1;
    backing=memfd_create("unixrights",MFD_CLOEXEC);
    if(backing<0||ftruncate(backing,4096)||socketpair(AF_UNIX,SOCK_STREAM,0,sockets))return 1;
    volatile unsigned char *map=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED,backing,0);
    if(map==MAP_FAILED)return 1;
    map[0]=0x5a;
    pthread_t threads[4];
    for(int i=0;i<workers;++i)if(pthread_create(&threads[i],NULL,receiver,NULL))return 1;
    enum {COUNT=512};
    for(int i=0;i<COUNT;++i) {
        char byte='F';
        union { struct cmsghdr align; char bytes[CMSG_SPACE(sizeof(int))]; } control={0};
        struct iovec iov={&byte,1};
        struct msghdr message={.msg_iov=&iov,.msg_iovlen=1,
            .msg_control=control.bytes,.msg_controllen=sizeof(control)};
        struct cmsghdr *c=CMSG_FIRSTHDR(&message);
        c->cmsg_level=SOL_SOCKET;c->cmsg_type=SCM_RIGHTS;c->cmsg_len=CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(c),&backing,sizeof(backing));
        ssize_t n;
        while((n=sendmsg(sockets[0],&message,0))<0&&(errno==EINTR||errno==EAGAIN))sched_yield();
        if(n!=1) {atomic_fetch_add(&failures,1);break;}
    }
    close(sockets[0]);
    for(int i=0;i<workers;++i)pthread_join(threads[i],NULL);
    if(bytes!=COUNT||handles!=COUNT||map[0]!=0x5a)atomic_fetch_add(&failures,1);
    munmap((void*)map,4096);close(backing);close(sockets[1]);
    printf("UNIXRIGHTSCHECK %s workers=%d bytes=%d handles=%d expected=%d failures=%d\n",
        failures?"FAIL":"PASS",workers,bytes,handles,COUNT,failures);
    return failures?1:0;
}
