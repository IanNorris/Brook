#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include "xwm-wire.h"
int main(void) {
    int fd[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,fd));
    struct bxwm_stream a={0},b={0};
    struct bxwm_message m={.version=BXWM_VERSION,.type=BXWM_WINDOW,.generation=7,
        .identity=42,.serial=99,.window=123,.width=320,.height=200,.flags=BXWM_MAPPED|BXWM_INPUT};
    assert(bxwm_valid(&m,7,1));assert(!bxwm_valid(&m,8,1));assert(!bxwm_valid(&m,7,0));
    m.flags|=BXWM_CLIENT_CHROME|BXWM_MAXIMIZED|BXWM_MINIMIZED;assert(bxwm_valid(&m,7,1));
    m.flags|=1u<<31;assert(!bxwm_valid(&m,7,1));m.flags=BXWM_MAPPED|BXWM_INPUT;
    m.type=BXWM_MAXIMIZE;m.x=2;assert(bxwm_valid(&m,7,1));m.x=3;assert(!bxwm_valid(&m,7,1));
    m.type=BXWM_BEGIN_RESIZE;m.x=7;assert(bxwm_valid(&m,7,1));m.x=8;assert(!bxwm_valid(&m,7,1));
    m.type=BXWM_WINDOW;m.x=0;
    struct bxwm_message received;
    /* Real stream fragmentation and multiple packets delivered in one write. */
    assert(send(fd[0],&m,17,MSG_NOSIGNAL)==17);
    assert(bxwm_receive(fd[1],&b,&received)==0 && b.received==17);
    assert(send(fd[0],(char *)&m+17,sizeof(m)-17,MSG_NOSIGNAL)==sizeof(m)-17);
    assert(bxwm_receive(fd[1],&b,&received)==1 && !memcmp(&m,&received,sizeof(m)));
    for (unsigned i=0;i<BXWM_QUEUE;++i) { m.identity=100+i;assert(bxwm_enqueue(&a,&m)); }
    assert(!bxwm_enqueue(&a,&m) && errno==ENOBUFS);
    assert(bxwm_flush(fd[0],&a)>=0);
    for (unsigned i=0;i<BXWM_QUEUE;++i) {
        assert(bxwm_receive(fd[1],&b,&received)==1 && received.identity==100+i);
    }
    assert(bxwm_receive(fd[1],&b,&received)==0);
    m.reserved=1;assert(!bxwm_valid(&m,7,1));m.reserved=0;
    m.title[63]='x';assert(!bxwm_valid(&m,7,1));m.title[63]=0;
    m.type=BXWM_CONFIGURE;assert(bxwm_valid(&m,7,0));
    m.x=32768;assert(!bxwm_valid(&m,7,0));m.x=0;
    m.width=0;assert(!bxwm_valid(&m,7,0));m.width=320;
    m.type=BXWM_DESTROY;assert(bxwm_valid(&m,7,1));m.identity=0;assert(!bxwm_valid(&m,7,1));
    /* A truncated final packet is not accepted as a complete event. */
    assert(send(fd[0],&m,13,MSG_NOSIGNAL)==13);close(fd[0]);
    assert(bxwm_receive(fd[1],&b,&received)==-1);close(fd[1]);
    assert(!socketpair(AF_UNIX,SOCK_STREAM,0,fd));
    int buffer=1024;assert(!setsockopt(fd[0],SOL_SOCKET,SO_SNDBUF,&buffer,sizeof(buffer)));
    memset(&a,0,sizeof(a));memset(&b,0,sizeof(b));
    for (unsigned i=0;i<BXWM_QUEUE;++i) { m.identity=200+i;assert(bxwm_enqueue(&a,&m)); }
    assert(bxwm_flush(fd[0],&a)==0 && a.count); /* real socket backpressure */
    unsigned delivered=0;
    for (unsigned attempt=0;attempt<1000 && delivered<BXWM_QUEUE;++attempt) {
        assert(bxwm_flush(fd[0],&a)>=0);
        int result;
        while ((result=bxwm_receive(fd[1],&b,&received))>0) {
            assert(received.identity==200+delivered);++delivered;
        }
        assert(result==0);
    }
    assert(delivered==BXWM_QUEUE && !a.count && !a.sent && !b.received);
    close(fd[0]);close(fd[1]);
    puts("XWM_WIRE_PASS split/coalesced frames, FIFO, queue exhaustion, generations, validation and truncated EOF");
    return 0;
}
