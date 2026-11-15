// H-15: abstract AF_UNIX names are the whole address, compared with length.
#include "harness.h"
#include "syscall_unix_socket.cpp"
#include "syscall_handlers.h"

using namespace harness;
namespace {
brook::UnixSocketData* NewSocket(int fd) {
    auto* socket=static_cast<brook::UnixSocketData*>(brook::kmalloc(sizeof(brook::UnixSocketData)));
    __builtin_memset(socket,0,sizeof(*socket));
    socket->refCount=1;socket->socketType=1;
    auto* proc=CurrentProcess();
    proc->fds[fd].type=brook::FdType::UnixSocket;proc->fds[fd].handle=socket;
    return socket;
}
uint64_t Abstract(uint64_t at,const char* name,uint32_t length) {
    auto* address=reinterpret_cast<uint8_t*>(at);
    address[0]=1;address[1]=0;address[2]=0;
    for(uint32_t i=0;i<length;++i)address[3+i]=static_cast<uint8_t>(name[i]);
    return at;
}
}

static void AbstractNames() {
    Reset();ResetProcess();
    auto* a=NewSocket(3);auto* b=NewSocket(4);auto* client=NewSocket(5);
    const uint64_t nameA=Abstract(UserAddress(0),"a",1),nameB=Abstract(UserAddress(256),"b",1);
    CHECK(brook::sys_bind(3,nameA,4,0,0,0)==0);
    CHECK(brook::sys_listen(3,8,0,0,0,0)==0);
    CHECK(brook::sys_bind(4,nameB,4,0,0,0)==0);
    CHECK(brook::sys_listen(4,8,0,0,0,0)==0);
    // Connecting to "\0b" reaches b, not the first abstract listener.
    CHECK(brook::sys_connect(5,nameB,4,0,0,0)==0);
    CHECK(b->pendingCount==1);
    CHECK(client->peerAddressLength==4 && client->peerAddress[3]=='b');
    // ...and "\0a" reaches a, whichever registry slot each listener has.
    NewSocket(10);
    CHECK(brook::sys_connect(10,nameA,4,0,0,0)==0);
    CHECK(a->pendingCount==1 && b->pendingCount==1);
    // An abstract name nobody listens on is refused.
    auto* other=NewSocket(6);(void)other;
    const uint64_t nameC=Abstract(UserAddress(512),"c",1);
    CHECK(brook::sys_connect(6,nameC,4,0,0,0)==-brook::ECONNREFUSED);
    // A second bind of "\0a" is EADDRINUSE.
    NewSocket(7);
    CHECK(brook::sys_bind(7,nameA,4,0,0,0)==-98);
    // A name that only differs after an embedded NUL is distinct.
    const uint64_t embedded=Abstract(UserAddress(768),"a\0x",3);
    NewSocket(8);
    CHECK(brook::sys_bind(8,embedded,6,0,0,0)==0);
    CHECK(brook::sys_listen(8,8,0,0,0,0)==0);
    NewSocket(9);
    CHECK(brook::sys_connect(9,embedded,6,0,0,0)==0);
    CHECK(a->pendingCount==1);
}

static void Autobind() {
    Reset();ResetProcess();
    auto* socket=NewSocket(3);
    const uint64_t address=UserAddress(0);
    *reinterpret_cast<uint16_t*>(address)=1;
    CHECK(brook::sys_bind(3,address,2,0,0,0)==0);
    CHECK(socket->localAddressLength==8);
    CHECK(socket->localAddress[2]==0 && socket->localAddress[3]!=0);
    // An unnamed listener is never reached by an empty abstract connect.
    auto* unnamed=NewSocket(4);(void)unnamed;
    CHECK(brook::sys_listen(3,8,0,0,0,0)==0);
    auto* client=NewSocket(5);(void)client;
    const uint64_t empty=Abstract(UserAddress(256),"",0);
    CHECK(brook::sys_connect(5,empty,3,0,0,0)==-brook::ECONNREFUSED);
}

void RunUnixTests() {
    AbstractNames();
    Autobind();
}
