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
std::atomic<bool> acknowledgeFin{false};
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
    if (pauseTransmit.load()) {
        transmitting = true;
        while (!releaseTransmit.load()) std::this_thread::yield();
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
    assert(allocations == frees);
    std::puts("socket lifetime: PASS");
}
