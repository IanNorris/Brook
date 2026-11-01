// Compile the real socket table, close path and RX code. Only hardware,
// allocation and scheduling boundaries are replaced by host stubs.
#include <atomic>
#include <cassert>
#include <cstdlib>
#include <cstdio>
#include <thread>
#include <chrono>
#include "../../kernel/src/net.cpp"

namespace {
std::atomic<unsigned> allocations{0}, frees{0}, wakes{0};
std::atomic<int> failAllocation{-1};
std::atomic<bool> pauseTransmit{false}, transmitting{false}, releaseTransmit{false};
std::atomic<bool> pauseOnlyPureAck{false};
std::atomic<bool> acknowledgeFin{false};
std::atomic<unsigned> rejectPureAcks{0},acceptedPureAcks{0};
std::atomic<uint32_t> lastAck{0},lastWindow{0};
std::atomic<int> updateAckSlot{-1};
std::atomic<bool> pauseAllocation{false}, allocating{false}, releaseAllocation{false};
std::atomic<uint64_t> ticks{1};
thread_local brook::Process* currentProcess = nullptr;
}
namespace brook {
void* kmalloc(size_t n) {
    int count = failAllocation.load();
    if (count == 0) return nullptr;
    if (count > 0) --failAllocation;
    void* p = std::malloc(n);
    if (p) ++allocations;
    if (pauseAllocation.exchange(false)) {
        allocating = true;
        while (!releaseAllocation.load()) std::this_thread::yield();
    }
    return p;
}
void kfree(void* p) { if (p) { ++frees; std::free(p); } }
uint64_t ApicTickCount() { return ticks.load(); }
Process* ProcessCurrent() { return currentProcess; }
Process* SchedulerCurrentProcess() { return currentProcess; }
void SchedulerBlock(Process* p) { ticks = p ? p->wakeupTick : ticks.load() + 50; std::this_thread::yield(); }
void SchedulerUnblock(Process*) { ++wakes; }
extern "C" void SchedulerYield() { std::this_thread::yield(); }
void SerialPuts(const char*) {}
void SerialPrintf(const char*, ...) {}
int SocketLifetimeTestTransmit(uint32_t src, uint32_t dst, uint8_t proto,
                               const void* data, uint32_t len) {
    assert(proto == IP_PROTO_TCP);
    const auto* tcp = static_cast<const TcpHeader*>(data);
    if (pauseTransmit.load() && (!pauseOnlyPureAck.load() || (tcp->flags==TCP_ACK && len==sizeof(TcpHeader)))) {
        transmitting = true;
        while (!releaseTransmit.load()) std::this_thread::yield();
    }
    if(tcp->flags==TCP_ACK && len==sizeof(TcpHeader)) {
        if(rejectPureAcks.load()) {
            --rejectPureAcks;
            int slot=updateAckSlot.exchange(-1);
            if(slot>=0){auto& socket=g_sockets[slot];SpinLockAcquire(&socket.lock);
                ++socket.tcpRcvNxt;socket.tcpUnackedSegs=1;socket.tcpLastDataTick=7;
                SpinLockRelease(&socket.lock);}
            return -11;
        }
        ++acceptedPureAcks;lastAck=ntohl(tcp->ackNum);lastWindow=ntohs(tcp->window);
    }
    if (acknowledgeFin.load() && (tcp->flags & TCP_FIN)) {
        // Deliver synchronously, just as local RX may reenter the sender.
        Ipv4Header ip{}; ip.srcIp = dst; ip.dstIp = src;
        TcpHeader ack{}; ack.srcPort = tcp->dstPort; ack.dstPort = tcp->srcPort;
        ack.seqNum = tcp->ackNum; ack.ackNum = htonl(ntohl(tcp->seqNum) + 1);
        ack.dataOff = 5 << 4; ack.flags = TCP_ACK; ack.window = htons(65535);
        HandleTcp(&ip, &ack, sizeof(ack));
    }
    assert(len >= sizeof(TcpHeader));
    return 0;
}
}
using namespace brook;
template<class F> void await(F predicate) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!predicate()) {
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
}
static int create(int type = SOCK_STREAM) {
    int i = SockCreateOwned(AF_INET, type, 0, 10, 1000, 1000);
    assert(i >= 0); return i;
}
static bool published(int i) {
    auto flags = IrqSpinLockAcquire(&g_socketRegistryLock);
    bool result = g_sockUsed[i];
    IrqSpinLockRelease(&g_socketRegistryLock, flags); return result;
}
static bool closingSlot(int i) {
    auto flags = IrqSpinLockAcquire(&g_socketRegistryLock);
    bool result = g_sockClosing[i];
    IrqSpinLockRelease(&g_socketRegistryLock, flags); return result;
}
static bool reservedSlot(int i) {
    auto flags=IrqSpinLockAcquire(&g_socketRegistryLock);
    bool result=g_sockReserved[i];IrqSpinLockRelease(&g_socketRegistryLock,flags);return result;
}
static void setupTcp(int i) {
    auto& s = g_sockets[i];
    s.localIp = 1; s.remoteIp = 2;
    s.localPort = htons(100); s.remotePort = htons(200);
    s.tcpState = TcpState::Established; s.connected = true;
    s.tcpSndNxt = s.tcpSndUna = 10; s.tcpRcvNxt = 20;
}
static void deliver(int i, bool fin = false) {
    const auto& s = g_sockets[i];
    Ipv4Header ip{}; ip.srcIp = s.remoteIp; ip.dstIp = s.localIp;
    struct { TcpHeader tcp; uint8_t byte; } packet{};
    packet.tcp.srcPort = s.remotePort; packet.tcp.dstPort = s.localPort;
    packet.tcp.seqNum = htonl(s.tcpRcvNxt); packet.tcp.ackNum = htonl(s.tcpSndNxt);
    packet.tcp.flags = TCP_ACK | (fin ? TCP_FIN : 0);
    packet.tcp.dataOff = 5 << 4; packet.tcp.window = htons(65535);
    packet.byte = 42;
    HandleTcp(&ip, &packet, sizeof(TcpHeader) + 1);
}
static void deliverControl(int i,uint32_t seq,uint32_t ack,uint8_t flags,uint16_t window) {
    const auto& s=g_sockets[i];Ipv4Header ip{};ip.srcIp=s.remoteIp;ip.dstIp=s.localIp;
    TcpHeader tcp{};tcp.srcPort=s.remotePort;tcp.dstPort=s.localPort;
    tcp.seqNum=htonl(seq);tcp.ackNum=htonl(ack);tcp.flags=flags;tcp.window=htons(window);tcp.dataOff=5<<4;
    HandleTcp(&ip,&tcp,sizeof(tcp));
}
int main() {
    // Queued RX ticket must finish before buffer retirement/reset.
    int i = create(); setupTcp(i);
    auto& s = g_sockets[i];
    SpinLockAcquire(&s.lock);
    uint32_t next = s.lock.next;
    std::thread rx([&] { deliver(i); });
    await([&] { return __atomic_load_n(&s.lock.next, __ATOMIC_ACQUIRE) > next; });
    std::atomic<bool> closed{false};
    std::thread close([&] { SockClose(i); closed = true; });
    await([&] { return __atomic_load_n(&s.lock.next, __ATOMIC_ACQUIRE) >= next + 2; });
    assert(!closed && s.rxBuf);
    SpinLockRelease(&s.lock);
    rx.join(); close.join(); assert(closed);
    assert(s.lock.next == s.lock.serving && s.lock.next > next);

    // A paused post-unlock transmit tail still pins buffers and reservation.
    i = create(); setupTcp(i); uint64_t generation = g_sockGeneration[i];
    pauseTransmit = true; transmitting = false; releaseTransmit = false;
    std::thread tail([&] { deliver(i, true); });
    await([] { return transmitting.load(); });
    unsigned before = frees.load(); closed = false;
    std::thread closer([&] { SockClose(i); closed = true; });
    // Close itself may transmit FIN; release both send tails only after the
    // callback has been observed alive. No freed buffers are allowed yet.
    await([&] { return closingSlot(i); });
    assert(!closed && frees == before && g_sockets[i].rxBuf);
    releaseTransmit = true; tail.join(); closer.join(); pauseTransmit = false;
    int reused = create(); assert(reused == i);
    { SocketBorrow stale(i, generation); assert(!stale); }
    { SocketBorrow current(i, g_sockGeneration[i]); assert(current); }
    SockClose(i);

    // A caller holding a callback borrow must prevent reuse after unpublish.
    i = create(SOCK_DGRAM); generation = g_sockGeneration[i];
    std::thread draining;
    {
        SocketBorrow borrow(i, generation); assert(borrow);
        closed = false;
        draining = std::thread([&] { SockClose(i); closed = true; });
        await([&] { return !published(i); });
        assert(!closed && g_sockets[i].rxBuf);
        int other = create(SOCK_DGRAM); assert(other != i); SockClose(other);
        // Leave the borrowed scope before joining its finalizer.

    }
    draining.join(); assert(closed);

    // Loopback FIN/ACK reentry cannot wait on the sender's own socket lock.
    i = create(); setupTcp(i); acknowledgeFin = true; SockClose(i);
    acknowledgeFin = false; assert(!published(i));

    // Direct close consumes one owner rather than forcibly destroying aliases.
    i = create(); SockRef(i); SockClose(i); assert(published(i));
    SockUnref(i); assert(!published(i));

    // Concurrent last-owner drops must claim exactly one finalizer.
    i = create(); SockRef(i); before = frees;
    std::atomic<bool> start{false};
    std::thread ownerA([&] { while (!start.load()) std::this_thread::yield(); SockClose(i); });
    std::thread ownerB([&] { while (!start.load()) std::this_thread::yield(); SockClose(i); });
    start = true; ownerA.join(); ownerB.join();
    assert(!published(i) && frees == before + 2);

    // Allocation failure never resets synchronization and frees partial RX.
    for (int fail = 0; fail < 2; ++fail) {
        uint32_t beforeTicket = g_sockets[0].lock.next;
        failAllocation = fail;
        assert(SockCreateOwned(AF_INET, SOCK_STREAM, 0, 1, 0, 0) < 0);
        failAllocation = -1;
        assert(g_sockets[0].lock.next == beforeTicket);
    }
    // Listener close during child allocation must reject publication and drop
    // the child's creator reference exactly once, outside the queue guard.
    i = create(); auto& listener = g_sockets[i];
    listener.listening = true; listener.listenBacklog = 4;
    listener.localPort = htons(100); listener.tcpState = TcpState::Listen;
    Ipv4Header ip{}; ip.srcIp = 2; ip.dstIp = 1;
    TcpHeader syn{}; syn.srcPort = htons(200); syn.dstPort = htons(100);
    syn.seqNum = htonl(10); syn.dataOff = 5 << 4; syn.flags = TCP_SYN;
    pauseAllocation = true; allocating = false; releaseAllocation = false;
    std::thread incoming([&] { HandleTcp(&ip, &syn, sizeof(syn)); });
    await([] { return allocating.load(); });
    closed = false;
    std::thread listenerClose([&] { SockClose(i); closed = true; });
    await([&] {
        auto f = IrqSpinLockAcquire(&g_acceptQueueLock);
        bool closing = !listener.listening;
        IrqSpinLockRelease(&g_acceptQueueLock, f); return closing;
    });
    assert(!closed);
    releaseAllocation = true; incoming.join(); listenerClose.join();
    assert(!published(i));

    // A deferred UDP wake owns its Process until SchedulerUnblock returns.
    i = create(SOCK_DGRAM);
    Process* waiter = new Process{}; waiter->refCount = 1;
    g_sockets[i].bound = true; g_sockets[i].localPort = htons(123);
    SockSetPollWaiter(i, waiter); assert(waiter->refCount == 2);
    const uint8_t byte = 7; unsigned wakeCount = wakes;
    SockDeliverUdp(2, 200, 1, 123, &byte, 1);
    assert(wakes == wakeCount + 1 && waiter->refCount == 1);
    SockSetPollWaiter(i, waiter); SockClearPollWaiter(i, waiter);
    assert(waiter->refCount == 1);
    SockSetPollWaiter(i, waiter); SockClose(i);
    assert(waiter->refCount == 1); delete waiter;
    // Blocking timeouts release waiter refs without relying on socket close.
    // Another FD owner outlives the waiting thread, as with dup/fork.
    waiter = new Process{}; waiter->refCount = 1;
    i = create(); SockRef(i);
    std::thread timeoutConnect([&] {
        currentProcess = waiter;
        SockAddrIn address{}; address.sin_addr = htonl(0x7f000001);
        address.sin_port = htons(200);
        assert(SockConnect(i, &address) == -110);
        assert(waiter->refCount == 1 && g_sockets[i].pollWaiter == nullptr);
        SockClose(i); // the other owner survives thread exit
        ProcessUnref(waiter); currentProcess = nullptr;
    });
    timeoutConnect.join();
    assert(waiter->refCount == 0 && published(i));
    SockClose(i); delete waiter;

    waiter = new Process{}; waiter->refCount = 1;
    i = create(); setupTcp(i); currentProcess = waiter;
    uint8_t payload = 1;
    assert(TcpWaitForDataAck(g_sockets[i], i, 10, &payload, 1) == -110);
    assert(waiter->refCount == 1 && g_sockets[i].pollWaiter == nullptr);
    currentProcess = nullptr; SockClose(i); delete waiter;
    // A busy NIC must not lose a cumulative ACK. Exercise actual HandleTcp,
    // real send failure and the production maintenance timer, without any
    // later packet prompting the retry.
    i=create();setupTcp(i);ticks=100;rejectPureAcks=2;acceptedPureAcks=0;
    uint64_t failuresBefore=__atomic_load_n(&g_tcpAckSubmissionFailures,__ATOMIC_RELAXED);
    deliver(i);deliver(i);
    assert(g_sockets[i].tcpRcvNxt==22 && g_sockets[i].tcpUnackedSegs==1 && acceptedPureAcks==0);
    ticks=141;TcpStatsMaybeFlush();assert(g_sockets[i].tcpUnackedSegs==1 && acceptedPureAcks==0);
    // Receive progress and free-window changes may occur while the first ACK
    // is in flight; a retry must use a fresh snapshot rather than stale bytes.
    SpinLockAcquire(&g_sockets[i].lock);
    g_sockets[i].tcpRcvNxt=25;g_sockets[i].rxCount=3;
    const auto expectedWindow=TcpAdvertisedWindow(TcpReceiveFree(g_sockets[i]),g_sockets[i].tcpRcvWndScale,0);
    SpinLockRelease(&g_sockets[i].lock);
    ticks=182;TcpStatsMaybeFlush();
    assert(acceptedPureAcks==1 && lastAck==25 && lastWindow==expectedWindow && g_sockets[i].tcpUnackedSegs==0);
    assert(__atomic_load_n(&g_tcpAckSubmissionFailures,__ATOMIC_RELAXED)==failuresBefore+2);
    SockClose(i);
    i=create();setupTcp(i);ticks=200;rejectPureAcks=1;acceptedPureAcks=0;updateAckSlot=i;
    deliver(i);deliver(i);
    assert(g_sockets[i].tcpRcvNxt==23 && g_sockets[i].tcpUnackedSegs==1 && g_sockets[i].tcpLastDataTick==7);
    ticks=201;TcpStatsMaybeFlush();assert(acceptedPureAcks==1 && lastAck==23);
    SockClose(i);
    i=create();setupTcp(i);ticks=300;rejectPureAcks=1;acceptedPureAcks=0;
    deliver(i);deliver(i);assert(g_sockets[i].tcpUnackedSegs==1);
    SpinLockAcquire(&g_sockets[i].lock);g_sockets[i].tcpState=TcpState::Closed;SpinLockRelease(&g_sockets[i].lock);
    ticks=341;TcpStatsMaybeFlush();assert(acceptedPureAcks==0 && g_sockets[i].tcpUnackedSegs==0);
    SockClose(i);
    // Draining a nearly full receive buffer must succeed even when its window
    // update is rejected; maintenance retries the current window afterward.
    i=create();setupTcp(i);ticks=400;rejectPureAcks=1;acceptedPureAcks=0;
    g_sockets[i].rxCount=Socket::RX_BUF_SIZE-100;g_sockets[i].rxTail=0;
    uint8_t received[2048]{};assert(SockRecv(i,received,sizeof(received),true)==sizeof(received));
    assert(g_sockets[i].tcpUnackedSegs==1 && acceptedPureAcks==0);
    const auto reopened=TcpAdvertisedWindow(TcpReceiveFree(g_sockets[i]),g_sockets[i].tcpRcvWndScale,0);
    ticks=441;TcpStatsMaybeFlush();assert(acceptedPureAcks==1 && lastWindow==reopened && g_sockets[i].tcpUnackedSegs==0);
    SockClose(i);
    // The unlocked window-update tail pins its incarnation while close is
    // pending; no receive storage is reclaimed before that tail returns.
    i=create();setupTcp(i);ticks=500;rejectPureAcks=1;pauseOnlyPureAck=true;pauseTransmit=true;transmitting=false;releaseTransmit=false;
    g_sockets[i].rxCount=Socket::RX_BUF_SIZE-100;g_sockets[i].rxTail=0;
    before=frees;std::thread reader([&]{uint8_t bytes[2048];assert(SockRecv(i,bytes,sizeof(bytes),true)==sizeof(bytes));});
    await([]{return transmitting.load();});closed=false;
    std::thread closeDuringWindow([&]{SockClose(i);closed=true;});
    await([&]{return !published(i);});assert(reservedSlot(i) && !closed && frees==before && g_sockets[i].rxBuf);
    releaseTransmit=true;reader.join();closeDuringWindow.join();pauseTransmit=false;pauseOnlyPureAck=false;assert(closed);
    // Real receive dispatch must ignore stale, future, unacknowledged and
    // out-of-window advertisements, while accepting an equal-sequence reopen.
    i=create();setupTcp(i);g_sockets[i].tcpSndNxt=12;g_sockets[i].tcpSndWnd=100;
    deliverControl(i,20,10,TCP_ACK,50);
    assert(g_sockets[i].tcpSndWnd==50 && g_sockets[i].tcpSndWlValid);
    deliverControl(i,19,10,TCP_ACK,99);assert(g_sockets[i].tcpSndWnd==50);
    deliverControl(i,20,9,TCP_ACK,99);assert(g_sockets[i].tcpSndWnd==50);
    deliverControl(i,20,13,TCP_ACK,99);assert(g_sockets[i].tcpSndWnd==50);
    deliverControl(i,20,10,0,99);assert(g_sockets[i].tcpSndWnd==50);
    deliverControl(i,1000000,10,TCP_ACK,99);assert(g_sockets[i].tcpSndWnd==50);
    deliverControl(i,20,10,TCP_ACK,0);assert(g_sockets[i].tcpSndWnd==0);
    waiter=new Process{};waiter->refCount=1;SockSetPollWaiter(i,waiter);wakeCount=wakes;
    deliverControl(i,20,10,TCP_ACK,80);
    assert(g_sockets[i].tcpSndWnd==80 && wakes==wakeCount+1 && waiter->refCount==1);
    deliverControl(i,20,10,TCP_RST|TCP_ACK,99);assert(g_sockets[i].tcpSndWnd==80);
    SockClose(i);delete waiter;
    // Sequence and ACK ordering use modular comparisons around wraparound.
    i=create();setupTcp(i);g_sockets[i].tcpRcvNxt=0xfffffffeu;
    g_sockets[i].tcpSndUna=0xfffffffeu;g_sockets[i].tcpSndNxt=2;
    deliverControl(i,0xfffffffeu,0xfffffffeu,TCP_ACK,30);
    deliverControl(i,0u,1u,TCP_ACK,40);assert(g_sockets[i].tcpSndWnd==40);
    deliverControl(i,0xffffffffu,1u,TCP_ACK,50);assert(g_sockets[i].tcpSndWnd==40);
    SockClose(i);
    // Active-open SYN window is unscaled even when scaling is negotiated;
    // subsequent acceptable ACK windows are scaled.
    i=create();setupTcp(i);g_sockets[i].tcpState=TcpState::SynSent;
    g_sockets[i].tcpSndUna=10;g_sockets[i].tcpSndNxt=11;g_sockets[i].tcpSndWnd=90;
    deliverControl(i,70,9,TCP_SYN|TCP_ACK,1234);
    assert(g_sockets[i].tcpState==TcpState::SynSent && g_sockets[i].tcpSndWnd==90 && !g_sockets[i].tcpSndWlValid);
    {
        Ipv4Header packetIp{};packetIp.srcIp=2;packetIp.dstIp=1;
        struct {TcpHeader tcp;uint8_t option[4];} packet{};
        packet.tcp.srcPort=htons(200);packet.tcp.dstPort=htons(100);packet.tcp.seqNum=htonl(70);
        packet.tcp.ackNum=htonl(11);packet.tcp.flags=TCP_SYN|TCP_ACK;packet.tcp.window=htons(1234);packet.tcp.dataOff=6<<4;
        packet.option[0]=1;packet.option[1]=3;packet.option[2]=3;packet.option[3]=3;
        HandleTcp(&packetIp,&packet,sizeof(packet));
    }
    assert(g_sockets[i].tcpState==TcpState::Established && g_sockets[i].tcpSndWnd==1234 && g_sockets[i].tcpSndWndScale==3);
    deliverControl(i,71,11,TCP_ACK,30);assert(g_sockets[i].tcpSndWnd==240);
    SockClose(i);
    // A passive SYN supplies an unscaled initial window and its final ACK
    // initializes ordering without negotiating scale.
    i=create();g_sockets[i].listening=true;g_sockets[i].listenBacklog=4;
    g_sockets[i].localPort=htons(100);g_sockets[i].tcpState=TcpState::Listen;
    {Ipv4Header packetIp{};packetIp.srcIp=2;packetIp.dstIp=1;TcpHeader packet{};
     packet.srcPort=htons(200);packet.dstPort=htons(100);packet.seqNum=htonl(70);
     packet.flags=TCP_SYN;packet.window=htons(1234);packet.dataOff=5<<4;HandleTcp(&packetIp,&packet,sizeof(packet));}
    assert(g_sockets[i].acceptQueueCount==1);int child=g_sockets[i].acceptQueue[0];
    assert(g_sockets[child].tcpSndWnd==1234 && !g_sockets[child].tcpSndWlValid);
    deliverControl(child,71,g_sockets[child].tcpSndNxt,TCP_ACK,500);
    assert(g_sockets[child].tcpState==TcpState::Established && g_sockets[child].tcpSndWnd==500 && g_sockets[child].tcpSndWlValid);
    SockClose(i);
    // A zero receive window permits an empty ACK at RCV.NXT, but not a
    // payload-bearing update, even when the ACK number itself is valid.
    i=create();setupTcp(i);g_sockets[i].rxCount=Socket::RX_BUF_SIZE;
    deliverControl(i,20,10,TCP_ACK,200);assert(g_sockets[i].tcpSndWnd==200);
    auto clippedBefore=__atomic_load_n(&g_tcpReceiveDiagnostics.clippedSegments,__ATOMIC_RELAXED);
    {Ipv4Header packetIp{};packetIp.srcIp=2;packetIp.dstIp=1;
     struct {TcpHeader tcp;uint8_t byte;} packet{};packet.tcp.srcPort=htons(200);packet.tcp.dstPort=htons(100);
     packet.tcp.seqNum=htonl(20);packet.tcp.ackNum=htonl(10);packet.tcp.flags=TCP_ACK;
     packet.tcp.window=htons(300);packet.tcp.dataOff=5<<4;HandleTcp(&packetIp,&packet,sizeof(TcpHeader)+1);}
    assert(g_sockets[i].tcpSndWnd==200);
    assert(__atomic_load_n(&g_tcpReceiveDiagnostics.clippedSegments,__ATOMIC_RELAXED)==clippedBefore+1);SockClose(i);
    // An overlapping segment is trimmed by receive processing; the window
    // order retains its original sequence rather than the trimmed position.
    i=create();setupTcp(i);
    {Ipv4Header packetIp{};packetIp.srcIp=2;packetIp.dstIp=1;
     struct {TcpHeader tcp;uint8_t bytes[2];} packet{};packet.tcp.srcPort=htons(200);packet.tcp.dstPort=htons(100);
     packet.tcp.seqNum=htonl(19);packet.tcp.ackNum=htonl(10);packet.tcp.flags=TCP_ACK;
     packet.tcp.window=htons(400);packet.tcp.dataOff=5<<4;HandleTcp(&packetIp,&packet,sizeof(TcpHeader)+2);}
    assert(g_sockets[i].tcpSndWnd==400 && g_sockets[i].tcpSndWl1==19 && g_sockets[i].tcpRcvNxt==21);
    SockClose(i);
    // Diagnostic counters reflect the actual OOO admission and overlap paths.
    i=create();setupTcp(i);
    auto arrivals=__atomic_load_n(&g_tcpReceiveDiagnostics.oooArrivals,__ATOMIC_RELAXED);
    auto held=__atomic_load_n(&g_tcpReceiveDiagnostics.oooStored,__ATOMIC_RELAXED);
    auto overlaps=__atomic_load_n(&g_tcpReceiveDiagnostics.oooOverlap,__ATOMIC_RELAXED);
    {Ipv4Header packetIp{};packetIp.srcIp=2;packetIp.dstIp=1;
     struct {TcpHeader tcp;uint8_t byte;} packet{};packet.tcp.srcPort=htons(200);packet.tcp.dstPort=htons(100);
     packet.tcp.seqNum=htonl(21);packet.tcp.ackNum=htonl(10);packet.tcp.flags=TCP_ACK;
     packet.tcp.window=htons(400);packet.tcp.dataOff=5<<4;
     HandleTcp(&packetIp,&packet,sizeof(TcpHeader)+1);HandleTcp(&packetIp,&packet,sizeof(TcpHeader)+1);}
    assert(__atomic_load_n(&g_tcpReceiveDiagnostics.oooArrivals,__ATOMIC_RELAXED)==arrivals+2);
    assert(__atomic_load_n(&g_tcpReceiveDiagnostics.oooStored,__ATOMIC_RELAXED)==held+1);
    assert(__atomic_load_n(&g_tcpReceiveDiagnostics.oooOverlap,__ATOMIC_RELAXED)==overlaps+1);
    SockClose(i);
    i=create();setupTcp(i);auto fullBefore=__atomic_load_n(&g_tcpReceiveDiagnostics.oooFull,__ATOMIC_RELAXED);
    {Ipv4Header packetIp{};packetIp.srcIp=2;packetIp.dstIp=1;
     struct {TcpHeader tcp;uint8_t byte;} packet{};packet.tcp.srcPort=htons(200);packet.tcp.dstPort=htons(100);
     packet.tcp.ackNum=htonl(10);packet.tcp.flags=TCP_ACK;packet.tcp.window=htons(400);packet.tcp.dataOff=5<<4;
     for(unsigned n=0;n<Socket::TCP_OOO_SLOT_COUNT+1;++n){packet.tcp.seqNum=htonl(21+n*2);HandleTcp(&packetIp,&packet,sizeof(TcpHeader)+1);}}
    assert(__atomic_load_n(&g_tcpReceiveDiagnostics.oooFull,__ATOMIC_RELAXED)==fullBefore+1);
    SockClose(i);
    // Future segments must reassemble in order even when inserted backwards
    // across sequence wrap, and leave no retained accounting behind.
    i=create();setupTcp(i);g_sockets[i].tcpRcvNxt=0xffffffe0u;
    {Ipv4Header ip{};ip.srcIp=2;ip.dstIp=1;
     struct {TcpHeader tcp;uint8_t byte;} packet{};packet.tcp.srcPort=htons(200);packet.tcp.dstPort=htons(100);
     packet.tcp.ackNum=htonl(10);packet.tcp.flags=TCP_ACK;packet.tcp.window=htons(400);packet.tcp.dataOff=5<<4;
     for(unsigned n=Socket::TCP_OOO_SLOT_COUNT;n>0;--n){packet.tcp.seqNum=htonl(0xffffffe0u+n);packet.byte=static_cast<uint8_t>(n);HandleTcp(&ip,&packet,sizeof(TcpHeader)+1);}
     assert(g_sockets[i].tcpOooBytes==Socket::TCP_OOO_SLOT_COUNT);
     packet.tcp.seqNum=htonl(0xffffffe0u);packet.byte=0;HandleTcp(&ip,&packet,sizeof(TcpHeader)+1);}
    {uint8_t data[Socket::TCP_OOO_SLOT_COUNT+1]{};assert(SockRecv(i,data,sizeof(data),true)==sizeof(data));
     for(unsigned n=0;n<sizeof(data);++n)assert(data[n]==n);}
    assert(g_sockets[i].tcpOooBytes==0 && g_sockets[i].tcpRcvNxt==0xffffffe0u+Socket::TCP_OOO_SLOT_COUNT+1);
    for(const auto& slot:g_sockets[i].tcpOooSlots)assert(!slot.used);
    SockClose(i);
    // Exercise the byte budget and copying with full-sized retained packets.
    i=create();setupTcp(i);
    {Ipv4Header ip{};ip.srcIp=2;ip.dstIp=1;
     struct {TcpHeader tcp;uint8_t bytes[Socket::TCP_OOO_SLOT_SIZE];} packet{};
     packet.tcp.srcPort=htons(200);packet.tcp.dstPort=htons(100);packet.tcp.ackNum=htonl(10);
     packet.tcp.flags=TCP_ACK;packet.tcp.window=htons(400);packet.tcp.dataOff=5<<4;
     for(unsigned n=0;n<Socket::TCP_OOO_SLOT_COUNT;++n){packet.tcp.seqNum=htonl(21+n*Socket::TCP_OOO_SLOT_SIZE);
       for(auto& b:packet.bytes)b=static_cast<uint8_t>(n);HandleTcp(&ip,&packet,sizeof(packet));}
     assert(g_sockets[i].tcpOooBytes==Socket::TCP_OOO_SLOT_COUNT*Socket::TCP_OOO_SLOT_SIZE);
     packet.tcp.seqNum=htonl(20);packet.bytes[0]=255;HandleTcp(&ip,&packet,sizeof(TcpHeader)+1);}
    {uint8_t data[Socket::TCP_OOO_SLOT_SIZE];uint8_t prefix;
     assert(SockRecv(i,&prefix,1,true)==1 && prefix==255);
     for(unsigned n=0;n<Socket::TCP_OOO_SLOT_COUNT;++n){assert(SockRecv(i,data,sizeof(data),true)==sizeof(data));for(auto b:data)assert(b==n);}}
    assert(g_sockets[i].tcpOooBytes==0);SockClose(i);
    // A retransmission that covers retained future bytes must retire those
    // entries without delivering them twice.
    i=create();setupTcp(i);
    {Ipv4Header ip{};ip.srcIp=2;ip.dstIp=1;
     struct {TcpHeader tcp;uint8_t bytes[4];} packet{};packet.tcp.srcPort=htons(200);packet.tcp.dstPort=htons(100);
     packet.tcp.ackNum=htonl(10);packet.tcp.flags=TCP_ACK;packet.tcp.window=htons(400);packet.tcp.dataOff=5<<4;
     packet.tcp.seqNum=htonl(22);packet.bytes[0]=2;HandleTcp(&ip,&packet,sizeof(TcpHeader)+1);
     packet.tcp.seqNum=htonl(20);for(unsigned n=0;n<4;++n)packet.bytes[n]=n;HandleTcp(&ip,&packet,sizeof(TcpHeader)+4);}
    {uint8_t data[4]{};assert(SockRecv(i,data,sizeof(data),true)==4);for(unsigned n=0;n<4;++n)assert(data[n]==n);}
    assert(g_sockets[i].tcpOooBytes==0);for(const auto& slot:g_sockets[i].tcpOooSlots)assert(!slot.used);
    SockClose(i);
    // A partially covered retained segment contributes only its new suffix.
    i=create();setupTcp(i);
    {Ipv4Header ip{};ip.srcIp=2;ip.dstIp=1;
     struct {TcpHeader tcp;uint8_t bytes[3];} packet{};packet.tcp.srcPort=htons(200);packet.tcp.dstPort=htons(100);
     packet.tcp.ackNum=htonl(10);packet.tcp.flags=TCP_ACK;packet.tcp.window=htons(400);packet.tcp.dataOff=5<<4;
     packet.tcp.seqNum=htonl(22);for(unsigned n=0;n<3;++n)packet.bytes[n]=n+2;HandleTcp(&ip,&packet,sizeof(TcpHeader)+3);
     packet.tcp.seqNum=htonl(20);for(unsigned n=0;n<3;++n)packet.bytes[n]=n;HandleTcp(&ip,&packet,sizeof(TcpHeader)+3);}
    {uint8_t data[5]{};assert(SockRecv(i,data,sizeof(data),true)==5);for(unsigned n=0;n<5;++n)assert(data[n]==n);}
    assert(g_sockets[i].tcpOooBytes==0);SockClose(i);
    // Retained data shares the RX budget, leaving room for a missing prefix.
    i=create();setupTcp(i);g_sockets[i].rxCount=Socket::RX_BUF_SIZE-4;
    {Ipv4Header ip{};ip.srcIp=2;ip.dstIp=1;
     struct {TcpHeader tcp;uint8_t bytes[2];} packet{};packet.tcp.srcPort=htons(200);packet.tcp.dstPort=htons(100);
     packet.tcp.ackNum=htonl(10);packet.tcp.flags=TCP_ACK;packet.tcp.window=htons(400);packet.tcp.dataOff=5<<4;
     packet.tcp.seqNum=htonl(22);HandleTcp(&ip,&packet,sizeof(TcpHeader)+2);
     assert(g_sockets[i].tcpOooBytes==2 && TcpReceiveFree(g_sockets[i])==2);
     packet.tcp.seqNum=htonl(20);HandleTcp(&ip,&packet,sizeof(TcpHeader)+2);}
    assert(g_sockets[i].tcpOooBytes==0 && g_sockets[i].rxCount==Socket::RX_BUF_SIZE && g_sockets[i].tcpRcvNxt==24);
    SockClose(i);
    assert(allocations == frees);
    std::puts("socket lifetime: PASS");
}
