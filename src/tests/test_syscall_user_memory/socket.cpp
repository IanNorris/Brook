// H-14: inet socket paths stage every address and payload in kernel memory.
#include "harness.h"
#include "syscall_socket.cpp"

using namespace harness;
namespace {
constexpr int SocketFd=3;
void OpenInetSocket() {
    auto* proc=CurrentProcess();
    proc->fds[SocketFd].type=brook::FdType::Socket;
    proc->fds[SocketFd].handle=reinterpret_cast<void*>(uintptr_t{1});
}
brook::SockAddrIn Address(uint32_t ip,uint16_t port) {
    brook::SockAddrIn address{};address.sin_family=2;address.sin_addr=ip;address.sin_port=port;return address;
}
void Begin() { Reset();ResetProcess();g_net=FakeNet{};OpenInetSocket(); }
template<class T> void Put(uint64_t address,const T& value) { __builtin_memcpy(reinterpret_cast<void*>(address),&value,sizeof(value)); }
template<class T> T Get(uint64_t address) { T value;__builtin_memcpy(&value,reinterpret_cast<const void*>(address),sizeof(value));return value; }
}

static void Bind() {
    Begin();
    CHECK(brook::sys_bind(SocketFd,g_kernel.Address(),sizeof(brook::SockAddrIn),0,0,0)==-brook::EFAULT);
    CHECK(g_net.binds==0);
    const uint64_t user=UserAddress(0);
    Put(user,Address(0x0100007f,0x3500));
    CHECK(brook::sys_bind(SocketFd,user,sizeof(brook::SockAddrIn),0,0,0)==0);
    CHECK(g_net.binds==1 && g_net.bound.sin_port==0x3500);
}

static void SendTo() {
    Begin();
    const uint64_t payload=UserAddress(0),destination=UserAddress(PageSize);
    __builtin_memcpy(reinterpret_cast<void*>(payload),"query",5);
    Put(destination,Address(0x08080808,0x3500));
    CHECK(brook::sys_sendto(SocketFd,g_kernel.Address(),5,0,destination,sizeof(brook::SockAddrIn))==-brook::EFAULT);
    CHECK(brook::sys_sendto(SocketFd,payload,5,0,g_kernel.Address(),sizeof(brook::SockAddrIn))==-brook::EFAULT);
    CHECK(g_net.sends==0);
    CHECK(brook::sys_sendto(SocketFd,payload,5,0,destination,sizeof(brook::SockAddrIn))==5);
    CHECK(g_net.sends==1 && g_net.sentWithDestination && g_net.sentDestination.sin_addr==0x08080808);
    CHECK(__builtin_memcmp(g_net.sent,"query",5)==0);
}

static void RecvFrom() {
    Begin();
    g_net.datagramReady=true;g_net.datagramLength=6;__builtin_memcpy(g_net.datagram,"answer",6);
    g_net.source=Address(0x08080808,0x3500);
    const uint64_t buffer=UserAddress(0),source=UserAddress(PageSize),length=UserAddress(PageSize+64);
    Put<uint32_t>(length,sizeof(brook::SockAddrIn));
    // Kernel payload buffer: refused before the datagram is consumed.
    CHECK(brook::sys_recvfrom(SocketFd,g_kernel.Address(),64,0,source,length)==-brook::EFAULT);
    CHECK(g_kernel.Intact());
    CHECK(g_net.datagramReady);
    // Kernel source address: nothing lands in kernel memory.
    CHECK(brook::sys_recvfrom(SocketFd,buffer,64,0,g_kernel.Address(),length)==-brook::EFAULT);
    CHECK(g_kernel.Intact());
    // addrlen is honoured: a 4-byte buffer receives 4 bytes, length reports 16.
    g_net.datagramReady=true;
    Put<uint32_t>(length,4);
    Put<uint64_t>(source+4,0x1111111111111111ULL);
    CHECK(brook::sys_recvfrom(SocketFd,buffer,64,0,source,length)==6);
    CHECK(__builtin_memcmp(reinterpret_cast<const void*>(buffer),"answer",6)==0);
    CHECK(Get<uint32_t>(length)==sizeof(brook::SockAddrIn));
    CHECK(Get<uint64_t>(source+4)==0x1111111111111111ULL);
}

static void RecvMsg() {
    Begin();
    g_net.datagramReady=true;g_net.datagramLength=3;__builtin_memcpy(g_net.datagram,"abc",3);
    struct Vector { uint64_t base,length; };
    const uint64_t header=UserAddress(0),vectors=UserAddress(256),name=UserAddress(512);
    brook::LinuxMsgHdr message{};
    message.msg_iov=reinterpret_cast<decltype(message.msg_iov)>(vectors);message.msg_iovlen=1;
    Put(header,message);
    Put(vectors,Vector{g_kernel.Address(),64});
    CHECK(brook::sys_recvmsg(SocketFd,header,0,0,0,0)==-brook::EFAULT);
    CHECK(g_kernel.Intact());
    CHECK(g_net.datagramReady);
    Put(vectors,Vector{UserAddress(1024),64});
    message.msg_name=reinterpret_cast<void*>(g_kernel.Address());message.msg_namelen=sizeof(brook::SockAddrIn);
    Put(header,message);
    CHECK(brook::sys_recvmsg(SocketFd,header,0,0,0,0)==-brook::EFAULT);
    CHECK(g_kernel.Intact());
    g_net.datagramReady=true;
    message.msg_name=reinterpret_cast<void*>(name);
    Put(header,message);
    CHECK(brook::sys_recvmsg(SocketFd,header,0,0,0,0)==3);
    CHECK(__builtin_memcmp(reinterpret_cast<const void*>(UserAddress(1024)),"abc",3)==0);
}

static void Names() {
    Begin();
    g_net.localIp=0x0100007f;g_net.localPort=0x5000;g_net.remoteIp=0x08080808;g_net.remotePort=0x3500;
    const uint64_t address=UserAddress(0),length=UserAddress(64);
    Put<uint32_t>(length,sizeof(brook::SockAddrIn));
    CHECK(brook::sys_getsockname(SocketFd,g_kernel.Address(),length,0,0,0)==-brook::EFAULT);
    CHECK(brook::sys_getsockname(SocketFd,address,g_kernel.Address(),0,0,0)==-brook::EFAULT);
    CHECK(brook::sys_getpeername(SocketFd,g_kernel.Address(),length,0,0,0)==-brook::EFAULT);
    CHECK(g_kernel.Intact());
    CHECK(brook::sys_getsockname(SocketFd,address,length,0,0,0)==0);
    CHECK(Get<brook::SockAddrIn>(address).sin_port==0x5000);
    Put<uint32_t>(length,2);Put<uint64_t>(address,0);
    CHECK(brook::sys_getpeername(SocketFd,address,length,0,0,0)==0);
    CHECK(Get<uint16_t>(address)==2 && Get<uint32_t>(address+4)==0);
    CHECK(Get<uint32_t>(length)==sizeof(brook::SockAddrIn));
}

static void Accept() {
    Begin();
    g_net.acceptReady=true;g_net.peer=Address(0x0a000002,0x1234);
    const uint64_t address=UserAddress(0),length=UserAddress(64);
    // addrlen 8 < 16: the old code wrote the whole address past the buffer.
    Put<uint32_t>(length,8);
    CHECK(brook::sys_accept(SocketFd,g_kernel.Address(),length,0,0,0)==-brook::EFAULT);
    CHECK(g_kernel.Intact());
    CHECK(g_net.accepts==0);
    Put<uint64_t>(address+8,0x2222222222222222ULL);
    const int64_t fd=brook::sys_accept(SocketFd,address,length,0,0,0);
    CHECK(fd>=0);
    CHECK(Get<uint32_t>(address+4)==0x0a000002);
    CHECK(Get<uint64_t>(address+8)==0x2222222222222222ULL);
    CHECK(Get<uint32_t>(length)==sizeof(brook::SockAddrIn));
}

// H-20 / L-34: SCM_RIGHTS passes the description itself. The receiver shares
// the sender's offset and status flags; FD_CLOEXEC comes from the receive call.
static void RightsShareDescription() {
    Reset();ResetProcess();auto* proc=CurrentProcess();
    auto& source=proc->fds[5];
    source.type=brook::FdType::DevNull;source.fdFlags=1;
    auto* description=Describe(source,2|0x800);
    brook::OpenFileSetOffset(description,42);
    brook::UnixFdSnap snap{};
    CHECK(brook::UnixFdSnapFrom(&source,&snap));
    CHECK(snap.file==description && description->refs==2);
    snap.fdFlags=0; // recvmsg without MSG_CMSG_CLOEXEC
    const int installed=brook::UnixFdSnapInstall(proc,&snap);
    CHECK(installed>=0 && installed!=5);
    if(installed>=0) {
        auto& received=proc->fds[installed];
        CHECK(received.file==description && received.fdFlags==0 && source.fdFlags==1);
        CHECK(brook::OpenFileOffset(received.file)==42);
        brook::OpenFileUpdateStatus(received.file,brook::OPEN_FILE_SETFL_MASK,0);
        CHECK(brook::FdStatus(&source)==2); // receiver's F_SETFL reaches the sender
    }
}

// getsockopt(IPPROTO_TCP, TCP_INFO): Linux struct tcp_info, truncated to the
// caller's length, which is written back as min(len, sizeof).
static void TcpInfo() {
    Begin();
    const uint64_t value=UserAddress(0),length=UserAddress(PageSize);
    Put<int32_t>(length,sizeof(brook::LinuxTcpInfo));
    CHECK(brook::sys_getsockopt(SocketFd,6,11,value,length,0)==-92); // UDP: ENOPROTOOPT
    g_net.tcp=true;
    CHECK(brook::sys_getsockopt(SocketFd,6,11,value,g_kernel.Address(),0)==-brook::EFAULT);
    CHECK(brook::sys_getsockopt(SocketFd,6,11,g_kernel.Address(),length,0)==-brook::EFAULT);
    CHECK(g_kernel.Intact());
    Put<int32_t>(length,-1);
    CHECK(brook::sys_getsockopt(SocketFd,6,11,value,length,0)==-brook::EINVAL);
    Put<int32_t>(length,1024);
    CHECK(brook::sys_getsockopt(SocketFd,6,11,value,length,0)==0);
    CHECK(Get<uint32_t>(length)==248);
    const auto info=Get<brook::LinuxTcpInfo>(value);
    CHECK(info.state==1 && info.sndMss==1400 && info.rcvMss==1460 && info.pmtu==1500);
    CHECK(info.unacked==2 && info.sndWnd==65535 && info.rcvWnd==4096 && info.bytesAcked==5000);
    CHECK(info.segsIn==7 && info.segsOut==9 && info.lastDataRecv==250 && info.ato==40000);
    CHECK((info.options&4) && (info.wscale&15)==7 && info.rtt==0);
    // A short buffer (older struct layouts) receives a prefix.
    Put<uint64_t>(value+104,0x3333333333333333ULL);
    Put<int32_t>(length,104);
    CHECK(brook::sys_getsockopt(SocketFd,6,11,value,length,0)==0);
    CHECK(Get<uint32_t>(length)==104 && Get<uint64_t>(value+104)==0x3333333333333333ULL);
    g_net.tcpState=1; // Listen
    Put<int32_t>(length,8);
    CHECK(brook::sys_getsockopt(SocketFd,6,11,value,length,0)==0 && Get<uint8_t>(value)==10);
    // Other TCP-level options remain ENOPROTOOPT.
    CHECK(brook::sys_getsockopt(SocketFd,6,1,value,length,0)==-92);
}

void RunSocketTests() {
    TcpInfo();
    RightsShareDescription();
    Bind();
    SendTo();
    RecvFrom();
    RecvMsg();
    Names();
    Accept();
}
