#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>
static unsigned char data[32768],reply[32768],disk[65519];
struct writer { int fd; unsigned char value; atomic_int* ready; atomic_int* go; int result; };
struct race_writer { int fd; const unsigned char* bytes; size_t length; atomic_int* entered; int result; };
static unsigned char long_write[262144],short_write[4096];
struct pool_drain {int fd;size_t bytes;int result;};
struct pool_wait {int fd;int epoll;atomic_int* ready;int result;};
static void* pool_drain_thread(void* opaque){
    struct pool_drain* d=opaque;unsigned char buffer[8192];size_t n=0;
    struct timespec ts;if(clock_gettime(CLOCK_MONOTONIC,&ts)){d->result=-1;return NULL;}
    uint64_t deadline=(uint64_t)ts.tv_sec*1000+ts.tv_nsec/1000000+15000;
    while(n<d->bytes){
        if(clock_gettime(CLOCK_MONOTONIC,&ts)){d->result=-1;return NULL;}
        uint64_t now=(uint64_t)ts.tv_sec*1000+ts.tv_nsec/1000000;
        if(now>=deadline){d->result=-1;return NULL;}
        struct pollfd p={.fd=d->fd,.events=POLLIN};
        if(poll(&p,1,(int)(deadline-now))<=0){d->result=-1;return NULL;}
        size_t want=d->bytes-n<sizeof(buffer)?d->bytes-n:sizeof(buffer);
        ssize_t got=read(d->fd,buffer,want);if(got<=0){d->result=-1;return NULL;}
        for(ssize_t i=0;i<got;++i)if(buffer[i]!='P'){d->result=-1;return NULL;}
        n+=(size_t)got;
    }
    d->result=0;return NULL;
}
static void* pool_wait_thread(void* opaque){
    struct pool_wait* w=opaque;atomic_fetch_add(w->ready,1);
    if(w->epoll){struct epoll_event event;w->result=epoll_wait(w->epoll,&event,1,10000);
        if(w->result==1 && (!(event.events&EPOLLOUT) || (event.events&(EPOLLERR|EPOLLHUP)) || event.data.u64!=1))w->result=-1;}
    else {struct pollfd p={.fd=w->fd,.events=POLLOUT};w->result=poll(&p,1,10000);
        if(w->result==1 && (!(p.revents&POLLOUT) || (p.revents&(POLLERR|POLLHUP|POLLNVAL))))w->result=-1;}
    return NULL;
}
static int async_pool_test(void){
    int listener=-1,senders[9],receivers[9],ep=-1;size_t sent[8]={};
    for(unsigned i=0;i<9;++i)senders[i]=receivers[i]=-1;
    struct sockaddr_in addr={.sin_family=AF_INET,.sin_port=htons(49411)};
    inet_pton(AF_INET,"127.0.0.1",&addr.sin_addr);
    listener=socket(AF_INET,SOCK_STREAM,0);
    if(listener<0 || bind(listener,(struct sockaddr*)&addr,sizeof(addr)) || listen(listener,16))goto failure;
    unsigned char block[4096];memset(block,'P',sizeof(block));
    for(unsigned i=0;i<9;++i){
        senders[i]=socket(AF_INET,SOCK_STREAM,0);
        if(senders[i]<0 || connect(senders[i],(struct sockaddr*)&addr,sizeof(addr)))goto failure;
        receivers[i]=accept(listener,NULL,NULL);if(receivers[i]<0)goto failure;
        if(fcntl(senders[i],F_SETFL,O_NONBLOCK))goto failure;
        if(i==8)break;
        int blocked=0;
        while(sent[i]<2*1024*1024){
            ssize_t n=send(senders[i],block,sizeof(block),MSG_NOSIGNAL);
            if(n>0){sent[i]+=(size_t)n;continue;}
            if(n<0 && (errno==EAGAIN || errno==EWOULDBLOCK)){
                struct pollfd p={.fd=senders[i],.events=POLLOUT};
                int r=poll(&p,1,100);if(r==0){blocked=1;break;}if(r>0)continue;
            }
            goto failure;
        }
        if(!blocked || !sent[i])goto failure;
    }
    {struct pollfd p={.fd=senders[8],.events=POLLOUT};if(poll(&p,1,0)!=0)goto failure;}
    ep=epoll_create1(0);if(ep<0)goto failure;
    {struct epoll_event event={.events=EPOLLOUT,.data.u64=1};
     if(epoll_ctl(ep,EPOLL_CTL_ADD,senders[8],&event) || epoll_wait(ep,&event,1,0)!=0)goto failure;}
    {
        atomic_int ready=0;pthread_t poller,epoller,drainer;
        struct pool_wait p={senders[8],0,&ready,-1},e={senders[8],ep,&ready,-1};
        struct pool_drain d={receivers[0],sent[0],-1};
        if(pthread_create(&poller,NULL,pool_wait_thread,&p))goto failure;
        if(pthread_create(&epoller,NULL,pool_wait_thread,&e)){pthread_join(poller,NULL);goto failure;}
        while(atomic_load(&ready)!=2)sched_yield();
        usleep(50000); // allow both waiters to enter their blocking calls
        if(pthread_create(&drainer,NULL,pool_drain_thread,&d)){pthread_join(poller,NULL);pthread_join(epoller,NULL);goto failure;}
        if(pthread_join(drainer,NULL) || pthread_join(poller,NULL) || pthread_join(epoller,NULL) || d.result || p.result!=1 || e.result!=1)goto failure;
    }
    if(ep>=0)close(ep);
    if(listener>=0)close(listener);
    for(unsigned i=0;i<9;++i){if(senders[i]>=0)close(senders[i]);if(receivers[i]>=0)close(receivers[i]);}
    puts("ECM_ASYNC_POOL_WAIT_OK");return 0;
failure:
    if(ep>=0)close(ep);
    if(listener>=0)close(listener);
    for(unsigned i=0;i<9;++i){if(senders[i]>=0)close(senders[i]);if(receivers[i]>=0)close(receivers[i]);}
    return -1;
}
static void* close_race_writer(void* opaque){
    struct race_writer* w=opaque;atomic_store(w->entered,1);
    w->result=write(w->fd,w->bytes,w->length)==(ssize_t)w->length?0:-1;return NULL;
}
static void* concurrent_writer(void* opaque){
    struct writer* w=opaque;unsigned char block[4096];memset(block,w->value,sizeof(block));
    atomic_fetch_add(w->ready,1);while(!atomic_load(w->go))sched_yield();
    w->result=write(w->fd,block,sizeof(block))==(ssize_t)sizeof(block)?0:-1;return NULL;
}
static int fail(const char* why){printf("ECM_TEST_FAIL %s errno=%d\n",why,errno);return 1;}
static int exact(int fd,void* data,size_t size,int sending){
    size_t done=0;while(done<size){ssize_t n=sending?write(fd,(char*)data+done,size-done):read(fd,(char*)data+done,size-done);if(n<=0)return -1;done+=(size_t)n;}return 0;
}
static int storage(int fd,unsigned pass){
    const off_t offset=511+(pass*65519u)%(8*1024*1024-sizeof(disk)-511);
    if(pread(fd,disk,sizeof(disk),offset)!=(ssize_t)sizeof(disk))return -1;
    for(unsigned i=0;i<sizeof(disk);++i)if(disk[i]!=(unsigned char)(((offset+i)*17+5)%251))return -1;
    return 0;
}
int main(int argc,char** argv){
    setvbuf(stdout,NULL,_IONBF,0);if(argc<2 || argc>8)return 2;
    int concurrency=0,close_race=0,user_copy=0,async=0,bench_small=0,pool_test=0;
    for(int i=2;i<argc;++i){if(!strcmp(argv[i],"concurrency"))concurrency=1;else if(!strcmp(argv[i],"close-race"))close_race=1;else if(!strcmp(argv[i],"user-copy"))user_copy=1;else if(!strcmp(argv[i],"async"))async=1;else if(!strcmp(argv[i],"bench-small"))bench_small=1;else if(!strcmp(argv[i],"pool-test"))pool_test=1;else return 2;}
    if(close_race && !concurrency)return 2;
    struct sockaddr_in peer={.sin_family=AF_INET,.sin_port=htons((unsigned short)atoi(argv[1]))};
    inet_pton(AF_INET,"10.0.2.100",&peer.sin_addr);
    int file=open("/media/usb/READTEST.BIN",O_RDONLY);if(file<0)return fail("USB file");
    int tcp=socket(AF_INET,SOCK_STREAM,0);if(tcp<0 || connect(tcp,(struct sockaddr*)&peer,sizeof(peer)))return fail("connect");
    // 14-byte Ethernet + 20-byte IPv4 + 20-byte TCP + 10 data = 64.
    char small[]="ecm-zlp-ok";if(exact(tcp,small,10,1) || exact(tcp,reply,10,0) || memcmp(small,reply,10))return fail("64-byte ZLP echo");
    usleep(1500000); // RX remains posted while idle.
    for(unsigned pass=0;pass<8;++pass){
        for(unsigned i=0;i<sizeof(data);++i)data[i]=(unsigned char)((i*13+pass*7)%251);
        if(exact(tcp,data,sizeof(data),1) || exact(tcp,reply,sizeof(reply),0) || memcmp(data,reply,sizeof(data)))return fail("TCP integrity");
        if(storage(file,pass))return fail("concurrent USB read");
    }
    if(bench_small){
        memset(data,'T',1024);
        for(unsigned n=0;n<1024;++n)if(exact(tcp,data,1024,1))return fail("small write benchmark");
        char done;if(exact(tcp,&done,1,0) || done!='S')return fail("small write benchmark reply");
        for(unsigned i=0;i<sizeof(data);++i)data[i]=(unsigned char)((i*13+7*7)%251);
        puts("ECM_SMALL_WRITES_OK bytes=1048576 calls=1024");
    }
    if(user_copy){
        struct iovec vector[2]={{(void*)"write",5},{(void*)"v",1}};
        struct iovec messageVector[2]={{(void*)"multi",5},{(void*)"iov",3}};
        struct msghdr message={.msg_iov=messageVector,.msg_iovlen=2};
        char* pages=mmap(NULL,8192,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        if(pages==MAP_FAILED || mprotect(pages+4096,4096,PROT_NONE))return fail("user copy mapping");
        memcpy(pages+4093,"bad",3);errno=0;
        if(send(tcp,pages+4093,6,0)!=-1 || errno!=EFAULT)return fail("cross-page send fault");
        memset(pages+4096-1200,'X',1200);
        if(send(tcp,pages+4096-1200,1201,0)!=1200)return fail("partial user send fault");
        struct iovec firstFault[2]={{pages+4096-300,300},{pages+4096,1}};
        errno=0;if(writev(tcp,firstFault,2)!=-1 || errno!=EFAULT)return fail("cross-vector first packet fault");
        memset(pages+4096-1200,'V',1200);
        struct iovec faultVector[3]={{(void*)-1,0},{pages+4096-1200,1200},{pages+4096,1}};
        if(writev(tcp,faultVector,3)!=1200)return fail("partial vectored send fault");
        struct iovec overflowVector[2]={{pages,(size_t)0x7fffffffffffffffULL},{pages,1}};
        errno=0;if(writev(tcp,overflowVector,2)!=-1 || errno!=EINVAL)return fail("vectored length overflow");
        if(munmap(pages,8192))return fail("user copy unmap");
        if(send(tcp,"send",4,0)!=4 || writev(tcp,vector,2)!=6 || sendmsg(tcp,&message,0)!=8)return fail("user send paths");
        if(exact(tcp,reply,18,0) || memcmp(reply,"sendwritevmultiiov",18))return fail("user send paths echo");
        puts("ECM_USER_SEND_PATHS_OK");
    }
    if(concurrency){
        atomic_int ready=0,go=0;pthread_t threads[2];
        struct writer writers[2]={{tcp,'A',&ready,&go,-1},{tcp,'B',&ready,&go,-1}};
        for(unsigned n=0;n<2;++n)if(pthread_create(&threads[n],NULL,concurrent_writer,&writers[n]))return fail("writer create");
        while(atomic_load(&ready)!=2)sched_yield();
        atomic_store(&go,1);
        for(unsigned n=0;n<2;++n)if(pthread_join(threads[n],NULL) || writers[n].result)return fail("concurrent write");
        if(exact(tcp,reply,8192,0))return fail("concurrent echo");
        for(unsigned n=0;n<8192;++n)if(reply[n]!=(n<4096?reply[0]:reply[4096]))return fail("interleaved writes");
        if(reply[0]==reply[4096] || (reply[0]!='A' && reply[0]!='B') || (reply[4096]!='A' && reply[4096]!='B'))return fail("concurrent values");
        puts("ECM_CONCURRENT_WRITERS_OK");
    }
    if(close_race){
        memset(long_write,'C',sizeof(long_write));memset(short_write,'D',sizeof(short_write));
        atomic_int entered1=0,entered2=0;pthread_t first,second;
        // Async writes may legally return a partial prefix on a full queue.
        // Keep this close test within the queue; the host verifies all bytes.
        struct race_writer one={tcp,long_write,async?16384:sizeof(long_write),&entered1,-1};
        struct race_writer two={tcp,short_write,sizeof(short_write),&entered2,-1};
        if(pthread_create(&first,NULL,close_race_writer,&one))return fail("close race first writer");
        while(!atomic_load(&entered1))sched_yield();
        char marker;if(read(tcp,&marker,1)!=1 || marker!='R')return fail("close race marker");
        if(pthread_create(&second,NULL,close_race_writer,&two))return fail("close race second writer");
        while(!atomic_load(&entered2))sched_yield();
        usleep(20000); // Allow the second syscall to pin the descriptor and contend.
        if(close(tcp))return fail("close race close");
        if(pthread_join(first,NULL) || pthread_join(second,NULL) || one.result || two.result)return fail("close race join");
        tcp=socket(AF_INET,SOCK_STREAM,0);if(tcp<0 || close(tcp))return fail("socket after close race");
        puts("ECM_SEND_CLOSE_RACE_OK");
    }else close(tcp);
    int udp=socket(AF_INET,SOCK_DGRAM,0);if(udp<0)return fail("UDP socket");
    // 14 Ethernet + 20 IPv4 + 8 UDP + 982 data = 1024 (also an SS packet).
    struct sockaddr_in local={.sin_family=AF_INET,.sin_port=peer.sin_port};
    if(bind(udp,(struct sockaddr*)&local,sizeof(local)))return fail("UDP bind");
    socklen_t length=sizeof(peer);struct pollfd wait={.fd=udp,.events=POLLIN};
    if(poll(&wait,1,10000)<=0)return fail("UDP readiness");
    if(recvfrom(udp,reply,sizeof(reply),0,(struct sockaddr*)&peer,&length)!=982 || memcmp(data,reply,982))return fail("1024-byte frame receive");
    if(sendto(udp,reply,982,0,(struct sockaddr*)&peer,length)!=982)return fail("1024-byte ZLP echo");
    close(udp);
    if(pool_test && async_pool_test())return fail("async pool wait");
    puts("ECM_NETWORK_INTEGRITY_OK bytes=262144 zlp_frames=64,1024");
    sleep(2);
    for(unsigned pass=0;pass<8;++pass)if(storage(file,pass))return fail("USB after disconnect");
    close(file);puts("ECM_USB_STILL_ALIVE");return 0;
}
