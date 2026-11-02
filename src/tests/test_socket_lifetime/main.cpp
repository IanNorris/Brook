// Compile the real socket table, close path and RX code. Only hardware,
// allocation and scheduling boundaries are replaced by host stubs.
#include <atomic>
#include <cassert>
#include <cstdlib>
#include <cstdio>
#include <thread>
#include <chrono>
#include "../../kernel/src/net.cpp"
#include "../../kernel/src/arp_probe_tracker.h"

namespace {
std::atomic<unsigned> allocations{0}, frees{0}, wakes{0};
std::atomic<int> failAllocation{-1};
std::atomic<bool> pauseTransmit{false}, transmitting{false}, releaseTransmit{false};
std::atomic<bool> pauseOnlyPureAck{false};
std::atomic<bool> acknowledgeFin{false};
std::atomic<unsigned> sentFins{0};
std::atomic<unsigned> acknowledgeDataCount{0};
std::atomic<unsigned> skipDataAcks{0};
std::atomic<unsigned> failDataSubmissions{0};
std::atomic<unsigned> failDataAttempt{0};
std::atomic<uint32_t> rejectDataSeq{0};
std::atomic<unsigned> rejectDataRemaining{0};
std::atomic<uint64_t> firstDataRetryTick{0};
std::atomic<int> observeFlightControlSocket{-1};
std::atomic<uint32_t> observedRetryCwnd{0},observedRetryThreshold{0};
std::atomic<bool> ackUnsentOnFailure{false},shrinkWindowOnFailure{false};
std::atomic<uint32_t> failureAckOverride{0};
std::atomic<uint16_t> dataAckWindow{65535};
std::atomic<bool> signalOnDataSend{false};
std::atomic<uint64_t> lastDataSubmitTick{0};
std::atomic<unsigned> flightPartialMode{0},flightPartialCount{0};
std::atomic<bool> fourLossMode{false};
std::atomic<unsigned> fourLossCount{0};
std::atomic<uint32_t> flightRetrySeq{0},flightRetryLen{0};
std::atomic<int> windowUpdateSocket{-1};
std::atomic<uint64_t> windowUpdateTick{0};
std::atomic<int> failUserCopyAfter{-1};
std::atomic<int> shrinkWindowOnCopy{-1};
std::atomic<uint32_t> copyWindowValue{0};
std::atomic<uint8_t*> mutateUserOnSend{nullptr};
std::atomic<unsigned> stagedPayloadCount{0};
std::atomic<uint8_t> stagedPayloads[2]{};
std::atomic<unsigned> capturedDataCount{0};
std::atomic<uint32_t> capturedDataSeq[64]{};
std::atomic<int> deadlineAckSocket{-1};
std::atomic<uint64_t> deadlineAckTick{0};
std::atomic<uint32_t> deadlineAckValue{0};
std::atomic<uint32_t> deadlineSecondAckValue{0};
std::atomic<bool> holdGatePoll{false}, gatePollWaiting{false}, releaseGatePoll{false};
std::atomic<unsigned> rejectPureAcks{0},acceptedPureAcks{0};
std::atomic<uint32_t> lastAck{0},lastWindow{0};
std::atomic<int> updateAckSlot{-1};
std::atomic<bool> pauseAllocation{false}, allocating{false}, releaseAllocation{false};
std::atomic<uint64_t> ticks{1};
thread_local brook::Process* currentProcess = nullptr;
uint8_t capturedFrame[brook::ETH_FRAME_MAX]{};
uint32_t capturedFrameLength=0;
int frameTransmitResult=0;
int captureFrame(brook::NetIf*,const void* frame,uint32_t length) {
    assert(length<=sizeof(capturedFrame));
    memcpy(capturedFrame,frame,length);capturedFrameLength=length;
    return frameTransmitResult;
}
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
uint64_t CopyFromUserBytes(void* dst,uint64_t src,uint64_t len) {
    int remaining=failUserCopyAfter.load();
    if(remaining==0)return 0;
    if(remaining>0)--failUserCopyAfter;
    memcpy(dst,reinterpret_cast<const void*>(src),len);
    const int idx=shrinkWindowOnCopy.exchange(-1);
    if(idx>=0){auto& s=g_sockets[idx];SpinLockAcquire(&s.lock);s.tcpSndWnd=copyWindowValue.load();SpinLockRelease(&s.lock);}
    return len;
}
uint64_t ApicTickCount() { return ticks.load(); }
Process* ProcessCurrent() { return currentProcess; }
Process* SchedulerCurrentProcess() { return currentProcess; }
void SchedulerKernelRegionEnter(Process* p) { ProcessRef(p);++p->kernelRegionDepth; }
void SchedulerKernelRegionLeave(Process* p) { assert(p->kernelRegionDepth);--p->kernelRegionDepth;ProcessUnref(p); }
uint64_t ProcessPendingSignals(Process* p) { return __atomic_load_n(&p->sigPending,__ATOMIC_ACQUIRE); }
uint64_t PosixTimerPending(Process*) { return 0; }
void SchedulerBlock(Process* p) {
    ticks = p ? p->wakeupTick : ticks.load() + 50;
    if (windowUpdateSocket.load()>=0 && ticks.load()>=windowUpdateTick.load()) {
        ticks=windowUpdateTick.load();int idx=windowUpdateSocket.exchange(-1);
        if(idx>=0){auto& socket=g_sockets[idx];Ipv4Header ip{};ip.srcIp=socket.remoteIp;ip.dstIp=socket.localIp;
            TcpHeader ack{};ack.srcPort=socket.remotePort;ack.dstPort=socket.localPort;
            ack.seqNum=htonl(socket.tcpRcvNxt);ack.ackNum=htonl(socket.tcpSndUna);
            ack.flags=TCP_ACK;ack.dataOff=5<<4;ack.window=htons(65535);HandleTcp(&ip,&ack,sizeof(ack));}
    }
    if (ticks.load() >= deadlineAckTick.load()) {
        int idx=deadlineAckSocket.exchange(-1);
        if(idx>=0) {
            auto& socket=g_sockets[idx];Ipv4Header ip{};ip.srcIp=socket.remoteIp;ip.dstIp=socket.localIp;
            TcpHeader ack{};ack.srcPort=socket.remotePort;ack.dstPort=socket.localPort;
            ack.seqNum=htonl(socket.tcpRcvNxt);const uint32_t value=deadlineAckValue.exchange(0);
            ack.ackNum=htonl(value?value:socket.tcpSndNxt);
            ack.flags=TCP_ACK;ack.dataOff=5<<4;ack.window=htons(65535);HandleTcp(&ip,&ack,sizeof(ack));
            const uint32_t second=deadlineSecondAckValue.exchange(0);
            if(second){ticks=ticks.load()+3;ack.ackNum=htonl(second);HandleTcp(&ip,&ack,sizeof(ack));}
        }
    }
    std::this_thread::yield();
}
void SchedulerBlockInterruptible(Process* p) {
    if (holdGatePoll.load()) {
        gatePollWaiting=true;
        while (!releaseGatePoll.load()) std::this_thread::yield();
    }
    SchedulerBlock(p);
}
void SchedulerUnblock(Process*) { ++wakes; }
extern "C" void SchedulerYield() { std::this_thread::yield(); }
void SerialPuts(const char*) {}
void SerialPrintf(const char*, ...) {}
int SocketLifetimeTestTransmit(uint32_t src, uint32_t dst, uint8_t proto,
                               const void* data, uint32_t len) {
    assert(proto == IP_PROTO_TCP);
    const auto* tcp = static_cast<const TcpHeader*>(data);
    if (tcp->flags & TCP_FIN) ++sentFins;
    if ((tcp->flags & TCP_PSH) && len>sizeof(TcpHeader)) {
        lastDataSubmitTick=ticks.load();
        const unsigned index=capturedDataCount.fetch_add(1);
        if(index<64)capturedDataSeq[index]=ntohl(tcp->seqNum);
        if(index && ntohl(tcp->seqNum)==10)firstDataRetryTick=ticks.load();
        if(index && observeFlightControlSocket.load()>=0){
            const auto& control=g_sockets[observeFlightControlSocket.load()].tcpFlightControl;
            observedRetryCwnd=control.cwnd;observedRetryThreshold=control.threshold;
        }
        uint8_t* mutate=mutateUserOnSend.load();
        if(mutate){const unsigned n=stagedPayloadCount.fetch_add(1);if(n<2)stagedPayloads[n]=static_cast<const uint8_t*>(data)[sizeof(TcpHeader)];if(n==0)*mutate=9;}
        if(rejectDataRemaining.load() && ntohl(tcp->seqNum)==rejectDataSeq.load()){
            --rejectDataRemaining;return -11;
        }
        if(failDataSubmissions.load() || (failDataAttempt.load() && index+1==failDataAttempt.load())){
            if(failDataSubmissions.load())--failDataSubmissions;
            failDataAttempt=0;
            if(ackUnsentOnFailure.exchange(false) || shrinkWindowOnFailure.load()){
                const bool shrink=shrinkWindowOnFailure.exchange(false);
                Ipv4Header ip{};ip.srcIp=dst;ip.dstIp=src;
                TcpHeader ack{};ack.srcPort=tcp->dstPort;ack.dstPort=tcp->srcPort;ack.seqNum=tcp->ackNum;
                const uint32_t override=failureAckOverride.exchange(0);
                ack.ackNum=htonl(override?override:ntohl(tcp->seqNum)+(shrink?0:len-sizeof(TcpHeader)));
                ack.flags=TCP_ACK;ack.dataOff=5<<4;ack.window=htons(shrink?0:65535);HandleTcp(&ip,&ack,sizeof(ack));
            }
            return -11;
        }
        if(flightPartialMode.load()){
            unsigned n=flightPartialCount.fetch_add(1);
            unsigned mode=flightPartialMode.load();
            if((mode==1 && n==0) || (mode==2 && n==1) || n==2){Ipv4Header ip{};ip.srcIp=dst;ip.dstIp=src;
                TcpHeader ack{};ack.srcPort=tcp->dstPort;ack.dstPort=tcp->srcPort;ack.seqNum=tcp->ackNum;
                if(n!=2)ack.ackNum=htonl(ntohl(tcp->seqNum)+10);
                else {flightRetrySeq=ntohl(tcp->seqNum);flightRetryLen=len-sizeof(TcpHeader);ack.ackNum=htonl(1082);}
                ack.flags=TCP_ACK;ack.dataOff=5<<4;ack.window=htons(65535);HandleTcp(&ip,&ack,sizeof(ack));}
            return 0;
        }
        if(fourLossMode.load()){
            unsigned n=fourLossCount.fetch_add(1);
            if(n!=1){Ipv4Header ip{};ip.srcIp=dst;ip.dstIp=src;
                TcpHeader ack{};ack.srcPort=tcp->dstPort;ack.dstPort=tcp->srcPort;ack.seqNum=tcp->ackNum;
                ack.ackNum=htonl(n==4?2154:546);ack.flags=TCP_ACK;ack.dataOff=5<<4;ack.window=htons(65535);
                HandleTcp(&ip,&ack,sizeof(ack));}
            return 0;
        }
        if(signalOnDataSend.exchange(false))__atomic_store_n(&currentProcess->sigPending,1,__ATOMIC_RELEASE);
    }
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
    if ((tcp->flags & TCP_PSH) && len>sizeof(TcpHeader) && skipDataAcks.load()) {
        --skipDataAcks;
    } else if ((tcp->flags & TCP_PSH) && len>sizeof(TcpHeader) && acknowledgeDataCount.load()) {
        --acknowledgeDataCount;
        Ipv4Header ip{};ip.srcIp=dst;ip.dstIp=src;
        TcpHeader ack{};ack.srcPort=tcp->dstPort;ack.dstPort=tcp->srcPort;
        ack.seqNum=tcp->ackNum;ack.ackNum=htonl(ntohl(tcp->seqNum)+len-sizeof(TcpHeader));
        ack.flags=TCP_ACK;ack.dataOff=5<<4;ack.window=htons(dataAckWindow.load());HandleTcp(&ip,&ack,sizeof(ack));
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
    for(unsigned n=0;n<MAX_SOCKETS;++n)KMutexInit(&g_tcpSendGates[n]);
    {
        NetIf nif{};nif.transmit=captureFrame;nif.ipAddr=1;nif.netmask=2;nif.gateway=3;
        NetSingleInterfaceSnapshot first{},changed{},restored{};
        assert(!NetReadSingleInterface(&first));
        g_netIfs[0]=&nif;g_netIfCount=1;NetInterfaceChangedLocked();
        assert(NetReadSingleInterface(&first) && first.interface==&nif && first.ip==1 && first.gateway==3);
        nif.gateway=4;NetInterfaceChangedLocked();assert(NetReadSingleInterface(&changed));
        nif.gateway=3;NetInterfaceChangedLocked();assert(NetReadSingleInterface(&restored));
        assert(first.generation!=changed.generation && first.generation!=restored.generation && restored.gateway==first.gateway);
        g_netIfCount=2;assert(!NetReadSingleInterface(&changed) && changed.generation==0 && !changed.interface);g_netIfCount=1;
        g_interfaceGeneration=~uint64_t(0);NetInterfaceChangedLocked();
        assert(!NetReadSingleInterface(&changed));NetInterfaceChangedLocked();assert(g_interfaceGeneration==0);
        g_netIfs[0]=nullptr;g_netIfCount=0;g_interfaceGeneration=1;
    }
    {
        MacAddr mac={{1,2,3,4,5,6}},updated={{6,5,4,3,2,1}},found{};
        assert(ArpCacheQuery(1,&found)==ArpCacheResult::Missing);
        for(uint32_t ip=1;ip<=ARP_CACHE_SIZE;++ip)ArpCacheInsert(ip,mac);
        assert(ArpCacheQuery(ARP_CACHE_SIZE+1,&found)==ArpCacheResult::Full);
        ArpCacheInsert(1,updated);
        assert(ArpCacheQuery(1,&found)==ArpCacheResult::Found && !memcmp(&found,&updated,sizeof(found)));
        ArpCacheInsert(ARP_CACHE_SIZE+1,mac);
        assert(ArpCacheQuery(ARP_CACHE_SIZE+1,&found)==ArpCacheResult::Full);
        memset(g_arpCache,0,sizeof(g_arpCache));g_arpCount=0;
    }
    assert(NetIpv4PayloadFits(1480) && !NetIpv4PayloadFits(1481) && !NetIpv4PayloadFits(~uint32_t(0)));
    {
        NetIf nif{};nif.transmit=captureFrame;nif.mac={{1,2,3,4,5,6}};
        const MacAddr destination={{7,8,9,10,11,12}};
        uint8_t payload[1480];for(unsigned n=0;n<sizeof(payload);++n)payload[n]=static_cast<uint8_t>(n);
        const uint32_t src=htonl(0x0a000201),dst=htonl(0x0a000202);
        assert(NetSendIpv4Resolved(&nif,destination,src,dst,IP_PROTO_TCP,payload,sizeof(payload))==0);
        assert(capturedFrameLength==ETH_FRAME_MAX && nif.txPackets==1 && nif.txBytes==ETH_FRAME_MAX);
        const auto* eth=reinterpret_cast<const EthHeader*>(capturedFrame);
        const auto* ip=reinterpret_cast<const Ipv4Header*>(capturedFrame+sizeof(EthHeader));
        assert(!memcmp(&eth->src,&nif.mac,sizeof(MacAddr)) && !memcmp(&eth->dst,&destination,sizeof(MacAddr)));
        assert(ntohs(eth->etherType)==ETH_TYPE_IPV4 && ip->verIhl==0x45 && ip->ttl==64 &&
               ntohs(ip->flagsFrag)==0x4000 && ntohs(ip->totalLen)==1500 &&
               ip->protocol==IP_PROTO_TCP && ip->srcIp==src && ip->dstIp==dst && InetChecksum(ip,sizeof(*ip))==0);
        assert(!memcmp(capturedFrame+sizeof(EthHeader)+sizeof(Ipv4Header),payload,sizeof(payload)));
        capturedFrameLength=0;const auto nextId=__atomic_load_n(&g_ipId,__ATOMIC_RELAXED);
        assert(NetSendIpv4Resolved(&nif,destination,src,dst,IP_PROTO_TCP,payload,1481)==-3);
        assert(NetSendIpv4Resolved(&nif,destination,src,dst,IP_PROTO_TCP,payload,~uint32_t(0))==-3);
        assert(capturedFrameLength==0 && nif.txPackets==1 && nif.txBytes==ETH_FRAME_MAX &&
               __atomic_load_n(&g_ipId,__ATOMIC_RELAXED)==nextId);
        frameTransmitResult=-11;
        assert(NetSendIpv4Resolved(&nif,destination,src,dst,IP_PROTO_TCP,payload,1)==-11);
        frameTransmitResult=0;
    }
    {
        NetIf nif{};nif.transmit=captureFrame;nif.mac={{1,2,3,4,5,6}};
        nif.ipAddr=htonl(0x0a000201);const uint32_t target=htonl(0x0a000202);
        capturedFrameLength=0;assert(ArpSendRequestOn(nullptr,target)==-1 && capturedFrameLength==0);
        assert(ArpSendRequestOn(&nif,target)==0 && capturedFrameLength==42);
        const auto* eth=reinterpret_cast<const EthHeader*>(capturedFrame);
        const auto* arp=reinterpret_cast<const ArpPacket*>(capturedFrame+sizeof(EthHeader));
        const MacAddr broadcast={{255,255,255,255,255,255}},zero={};
        assert(!memcmp(&eth->dst,&broadcast,sizeof(broadcast)) && !memcmp(&eth->src,&nif.mac,sizeof(MacAddr)));
        assert(ntohs(eth->etherType)==ETH_TYPE_ARP && ntohs(arp->htype)==1 &&
               ntohs(arp->ptype)==ETH_TYPE_IPV4 && arp->hlen==6 && arp->plen==4 && ntohs(arp->oper)==ARP_OP_REQUEST &&
               arp->spa==nif.ipAddr && arp->tpa==target && !memcmp(&arp->sha,&nif.mac,sizeof(MacAddr)) &&
               !memcmp(&arp->tha,&zero,sizeof(MacAddr)));
        frameTransmitResult=-11;assert(ArpSendRequestOn(&nif,target)==-11);frameTransmitResult=0;
    }
    {
        using Tracker=ArpProbeTracker<2>;Tracker tracker;Tracker::Ticket first{},other{},retry{};
        NetIf nif{};nif.transmit=captureFrame;nif.mac={{1,2,3,4,5,6}};nif.ipAddr=htonl(0x0a000201);
        const uint64_t interfaceId=reinterpret_cast<uint64_t>(&nif);
        const uint32_t a=htonl(0x0a000202),b=htonl(0x0a000203),c=htonl(0x0a000204);
        assert(tracker.Request(interfaceId,a,1,2000).status==Tracker::Status::Pending);
        assert(tracker.Request(interfaceId,a,2,2000).status==Tracker::Status::Pending);
        assert(tracker.Request(interfaceId,b,2,2000).status==Tracker::Status::Pending);
        assert(tracker.Request(interfaceId,c,2,2000).status==Tracker::Status::Full);
        assert(tracker.Next(2,&first) && first.ip==a);
        frameTransmitResult=-11;assert(tracker.Complete(first,ArpSendRequestOn(&nif,a),2));frameTransmitResult=0;
        assert(tracker.Next(2,&other) && other.ip==b);
        assert(tracker.Complete(other,ArpSendRequestOn(&nif,b),2));
        assert(!tracker.Next(11,&retry) && tracker.Next(12,&retry) && retry.ip==a);
        // A reply during transmit resolves the slot. A later request can
        // evict the resolved value, but stale completion cannot alter it.
        const uint64_t learned=0x060504030202ULL;
        assert(tracker.Reply(interfaceId,a,learned,12,100));
        auto resolved=tracker.Request(interfaceId,a,12,2000);
        assert(resolved.status==Tracker::Status::Resolved && resolved.mac==learned);
        assert(tracker.Request(interfaceId,c,12,2000).status==Tracker::Status::Pending);
        assert(!tracker.Complete(retry,0,12));
        assert(tracker.Next(12,&retry) && retry.ip==c);
        assert(tracker.Complete(retry,-5,12));
        assert(tracker.Request(interfaceId,c,13,2000).status==Tracker::Status::Failed);
        assert(tracker.Request(interfaceId,c,512,2000).status==Tracker::Status::Pending);
    }
    {
        using Tracker=ArpProbeTracker<1>;Tracker tracker;Tracker::Ticket ticket{};
        assert(tracker.Request(1,1,1,1200).status==Tracker::Status::Pending);
        for(uint64_t now : {1ULL,501ULL,1001ULL}){
            assert(tracker.Next(now,&ticket));assert(tracker.Complete(ticket,0,now));
        }
        assert(!tracker.Next(1201,&ticket));
        assert(tracker.Request(1,1,1201,1200).status==Tracker::Status::Failed);
        assert(tracker.Request(1,1,1701,1200).status==Tracker::Status::Pending);
    }
    {
        using Tracker=ArpProbeTracker<1>;Tracker tracker;Tracker::Ticket ticket{};
        assert(tracker.Request(1,2,1,5000).status==Tracker::Status::Pending);
        for(uint64_t now : {1ULL,501ULL,1001ULL}){
            assert(tracker.Next(now,&ticket));assert(tracker.Complete(ticket,0,now));
        }
        assert(!tracker.Next(1501,&ticket));
        assert(tracker.Request(1,2,1501,5000).status==Tracker::Status::Failed);
        Tracker busy;assert(busy.Request(1,3,1,25).status==Tracker::Status::Pending);
        for(uint64_t now : {1ULL,11ULL,21ULL}){
            assert(busy.Next(now,&ticket));assert(busy.Complete(ticket,-11,now));
        }
        assert(!busy.Next(26,&ticket));
        assert(busy.Request(1,3,26,25).status==Tracker::Status::Failed);
    }
    {
        using Tracker=ArpProbeTracker<2>;Tracker tracker;Tracker::Ticket first{},second{},replacement{};
        assert(tracker.Request(1,1,1,20).status==Tracker::Status::Pending);
        assert(tracker.Request(1,2,1,1000).status==Tracker::Status::Pending);
        assert(tracker.Next(1,&first) && first.ip==1);
        assert(tracker.Next(1,&second) && second.ip==2);
        assert(tracker.Complete(second,-11,1));
        assert(tracker.Next(11,&second) && second.ip==2);
        assert(tracker.Complete(second,-11,11));
        assert(tracker.Next(21,&replacement) && replacement.ip==2);
        assert(tracker.Complete(replacement,-11,21));
        assert(!tracker.Complete(first,0,21));
        assert(tracker.Request(1,1,21,20).status==Tracker::Status::Failed);
    }
    {
        using Tracker=ArpProbeTracker<2>;Tracker tracker;Tracker::Ticket ticket{};
        const uint64_t mac=0x060504030202ULL;
        assert(tracker.Request(1,42,1,1000).status==Tracker::Status::Pending);
        assert(!tracker.Reply(1,42,mac,1,100)); // no probe submitted or in flight
        assert(tracker.Next(1,&ticket));
        assert(!tracker.Reply(2,42,mac,1,100)); // another configuration
        assert(!tracker.Reply(1,42,1,1,100)); // multicast MAC
        assert(tracker.Reply(1,42,mac,1,100)); // reply during submission
        assert(!tracker.Complete(ticket,-11,1));
        assert(tracker.Request(1,42,2,1000).status==Tracker::Status::Resolved);
        assert(tracker.Request(2,42,2,1000).status==Tracker::Status::Pending);
        assert(tracker.Request(1,42,101,1000).status==Tracker::Status::Pending); // expired
    }
    {
        using Tracker=ArpProbeTracker<2>;Tracker tracker;Tracker::Ticket a{},b{};
        const uint64_t macA=0x060504030202ULL,macB=0x060504030204ULL;
        assert(tracker.Request(10,1,1,1000).status==Tracker::Status::Pending);
        assert(tracker.Request(10,2,1,1000).status==Tracker::Status::Pending);
        assert(tracker.Next(1,&a) && tracker.Complete(a,0,1));
        assert(tracker.Next(1,&b) && tracker.Complete(b,0,1));
        assert(tracker.Reply(10,1,macA,2,100));
        assert(tracker.Reply(10,2,macB,2,50));
        assert(tracker.Request(10,3,3,1000).status==Tracker::Status::Pending);
        auto kept=tracker.Request(10,1,3,1000);
        assert(kept.status==Tracker::Status::Resolved && kept.mac==macA);
        assert(tracker.Request(10,2,3,1000).status==Tracker::Status::Pending); // earliest expiry was evicted
        Tracker expired;assert(expired.Request(10,1,1,20).status==Tracker::Status::Pending);
        assert(expired.Next(1,&a) && expired.Complete(a,0,1));
        assert(!expired.Reply(10,1,macA,21,100));
        assert(expired.Request(10,1,21,20).status==Tracker::Status::Failed);
    }
    {
        TcpSendFlightState invalid(17);assert(!invalid.Allocate());
        TcpSendFlightState empty(0);assert(!empty.Allocate());
        TcpSendFlightState normal(8);assert(normal.Allocate() && !normal.Allocate());
        TcpSendFlightState wide(16);assert(wide.Allocate() && !wide.Allocate());
    }
    {
        const char a[]="abc",b[]="defgh";
        TcpSendVector vectors[]={{reinterpret_cast<uint64_t>(a),3},{~uint64_t(0),0},
                                 {reinterpret_cast<uint64_t>(b),5}};
        TcpSendSource source{nullptr,true,vectors,3};char out[8]{};
        assert(source.Copy(reinterpret_cast<uint8_t*>(out),1,6));
        assert(!memcmp(out,"bcdefg",6));
        assert(!source.Copy(reinterpret_cast<uint8_t*>(out),7,2));
        TcpSendVector bad[]={{~uint64_t(0),2}};
        TcpSendSource badSource{nullptr,true,bad,1};
        assert(!badSource.Copy(reinterpret_cast<uint8_t*>(out),1,1));
    }
    // A caller needing ownership must distinguish a retained socket from a
    // slot already entering final close; an RX borrow is not that guarantee.
    assert(!SockTryRef(-1) && !SockTryRef(MAX_SOCKETS));
    int ownerSocket=create();setupTcp(ownerSocket);
    assert(SockTryRef(ownerSocket));
    SockUnref(ownerSocket);assert(published(ownerSocket));
    SpinLockAcquire(&g_sockets[ownerSocket].lock);
    std::thread finalOwner([&] { SockUnref(ownerSocket); });
    await([&] { return closingSlot(ownerSocket); });
    assert(!SockTryRef(ownerSocket));
    SpinLockRelease(&g_sockets[ownerSocket].lock);
    finalOwner.join();assert(!published(ownerSocket));
    assert(!SockTryRef(ownerSocket));
    // Real KMutex ownership and retained socket references prevent the last
    // FD close from racing either the current sender or a queued sender.
    int gateSocket=create();setupTcp(gateSocket);
    auto* gateOwner=new Process{};auto* gateWaiter=new Process{};
    gateOwner->refCount=gateWaiter->refCount=1;gateOwner->pid=101;gateWaiter->pid=102;
    currentProcess=gateOwner;ticks=1;holdGatePoll=true;gatePollWaiting=false;releaseGatePoll=false;
    std::thread queued;
    {
        TcpSendOwnership owner(gateSocket,gateOwner);assert(owner.Acquire(30001)==0);
        queued=std::thread([&] {
            currentProcess=gateWaiter;
            { TcpSendOwnership queuedOwner(gateSocket,gateWaiter);assert(queuedOwner.Acquire(30001)==0); }
            currentProcess=nullptr;
        });
        await([] { return gatePollWaiting.load(); });
        SockClose(gateSocket);assert(published(gateSocket) && !closingSlot(gateSocket));
        assert(g_sockets[gateSocket].refCount==2);
        acknowledgeFin=true;
    }
    releaseGatePoll=true;queued.join();holdGatePoll=false;acknowledgeFin=false;
    assert(!published(gateSocket));assert(gateOwner->kernelRegionDepth==0 && gateWaiter->kernelRegionDepth==0);
    assert(gateOwner->refCount==1 && gateWaiter->refCount==1);
    currentProcess=nullptr;delete gateOwner;delete gateWaiter;
    // A timeout or signal while waiting for ownership must not consume TCP
    // sequence space or close the stream belonging to the active writer.
    gateSocket=create();setupTcp(gateSocket);gateOwner=new Process{};gateWaiter=new Process{};
    gateOwner->refCount=gateWaiter->refCount=1;gateOwner->pid=103;gateWaiter->pid=104;
    currentProcess=gateOwner;ticks=1;
    {
        TcpSendOwnership owner(gateSocket,gateOwner);assert(owner.Acquire(30001)==0);
        currentProcess=gateWaiter;
        {TcpSendOwnership waiting(gateSocket,gateWaiter);assert(waiting.Acquire(21)==-110);}
        assert(g_sockets[gateSocket].tcpSndNxt==10 && g_sockets[gateSocket].tcpState==TcpState::Established);
        SockSetNonblock(gateSocket,true);
        {const uint8_t data=0;assert(SockSend(gateSocket,&data,1)==-11);}
        SockSetNonblock(gateSocket,false);
        gateWaiter->sigPending=1;
        {TcpSendOwnership waiting(gateSocket,gateWaiter);assert(waiting.Acquire(30001)==-4);}
        assert(g_sockets[gateSocket].refCount==2 && gateWaiter->kernelRegionDepth==0);
        currentProcess=gateOwner;
    }
    currentProcess=nullptr;SockClose(gateSocket);delete gateOwner;delete gateWaiter;
    // A signal arriving while a contended writer sleeps unwinds its owner
    // reference and outer region without disturbing the active writer.
    gateSocket=create();setupTcp(gateSocket);gateOwner=new Process{};gateWaiter=new Process{};
    gateOwner->refCount=gateWaiter->refCount=1;gateOwner->pid=107;gateWaiter->pid=108;
    currentProcess=gateOwner;ticks=1;holdGatePoll=true;gatePollWaiting=false;releaseGatePoll=false;
    {
        TcpSendOwnership owner(gateSocket,gateOwner);assert(owner.Acquire(30001)==0);
        std::thread interrupted([&]{
            currentProcess=gateWaiter;
            {TcpSendOwnership waiting(gateSocket,gateWaiter);assert(waiting.Acquire(30001)==-4);}
            currentProcess=nullptr;
        });
        await([]{return gatePollWaiting.load();});
        __atomic_store_n(&gateWaiter->sigPending,1,__ATOMIC_RELEASE);
        releaseGatePoll=true;interrupted.join();holdGatePoll=false;
        assert(gateWaiter->kernelRegionDepth==0 && gateWaiter->refCount==1);
        assert(g_sockets[gateSocket].refCount==2 && g_sockets[gateSocket].tcpSndNxt==10);
    }
    currentProcess=nullptr;SockClose(gateSocket);delete gateOwner;delete gateWaiter;
    gateSocket=create();setupTcp(gateSocket);
    {const uint8_t data=0;assert(SockSend(gateSocket,&data,1)==-11 && g_sockets[gateSocket].tcpSndNxt==10);}
    SockClose(gateSocket);
    // Run two actual writers while the first submission is paused. Closing
    // the FD leaves both operations owned; their sequence ranges do not overlap.
    gateSocket=create();setupTcp(gateSocket);gateOwner=new Process{};gateWaiter=new Process{};
    gateOwner->refCount=gateWaiter->refCount=1;gateOwner->pid=105;gateWaiter->pid=106;
    ticks=1;pauseTransmit=true;transmitting=false;releaseTransmit=false;
    holdGatePoll=true;gatePollWaiting=false;releaseGatePoll=false;
    capturedDataCount=0;acknowledgeDataCount=2;acknowledgeFin=true;
    std::thread writer1([&]{currentProcess=gateOwner;const uint8_t data=1;assert(SockSend(gateSocket,&data,1)==1);currentProcess=nullptr;});
    await([]{return transmitting.load();});
    std::thread writer2([&]{currentProcess=gateWaiter;const uint8_t data=2;assert(SockSend(gateSocket,&data,1)==1);currentProcess=nullptr;});
    await([]{return gatePollWaiting.load();});
    assert(capturedDataCount==1 && g_sockets[gateSocket].tcpSndNxt==11);
    SockClose(gateSocket);assert(published(gateSocket));
    releaseTransmit=true;writer1.join();releaseGatePoll=true;writer2.join();
    pauseTransmit=false;holdGatePoll=false;acknowledgeFin=false;
    assert(capturedDataCount==2 && capturedDataSeq[0]==10 && capturedDataSeq[1]==11);
    assert(!published(gateSocket) && gateOwner->kernelRegionDepth==0 && gateWaiter->kernelRegionDepth==0);
    assert(gateOwner->refCount==1 && gateWaiter->refCount==1);delete gateOwner;delete gateWaiter;
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
    assert(g_sockets[i].tcpState == TcpState::Closed && g_sockets[i].tcpRstRecv);
    assert(SockGetConnectError(i) == 110 && SockGetConnectError(i) == 0);
    currentProcess = nullptr; SockClose(i); delete waiter;
    // The full send path may not leave an established stream after the
    // caller's sole retransmission copy is lost to a timeout.
    waiter = new Process{}; waiter->refCount = 1;
    i=create();setupTcp(i);currentProcess=waiter;ticks=1;
    unsigned finsBefore=sentFins;
    assert(SockSend(i,&payload,1)==-110);
    assert(g_sockets[i].tcpState==TcpState::Closed && !g_sockets[i].connected);
    assert(SockGetConnectError(i)==110 && SockGetConnectError(i)==0);
    uint8_t recvByte=0;assert(SockRecv(i,&recvByte,1,true)==-104);
    assert(SockSend(i,&payload,1)==-104);
    assert(waiter->refCount==1 && !g_sockets[i].pollWaiter);
    currentProcess=nullptr;SockClose(i);assert(sentFins==finsBefore);delete waiter;
    // An ACK delivered on the deadline wake wins before the locked timeout decision.
    waiter=new Process{};waiter->refCount=1;
    i=create();setupTcp(i);currentProcess=waiter;g_sockets[i].tcpSndNxt=11;ticks=1;
    deadlineAckTick=30001;deadlineAckSocket=i;
    assert(TcpWaitForDataAck(g_sockets[i],i,10,&payload,1)==0);
    assert(deadlineAckSocket==-1 && g_sockets[i].tcpSndUna==11);
    assert(g_sockets[i].tcpState==TcpState::Established && SockGetConnectError(i)==0);
    currentProcess=nullptr;SockClose(i);delete waiter;
    // The optional flight emits two segments before the first cumulative ACK.
    waiter=new Process{};waiter->refCount=1;
    i=create();setupTcp(i);currentProcess=waiter;ticks=1;g_sockets[i].tcpSndWnd=65535;
    g_tcpTwoPacketFlight=true;skipDataAcks=1;acknowledgeDataCount=1;capturedDataCount=0;
    {uint8_t data[1072]{};assert(SockSend(i,data,sizeof(data))==sizeof(data));}
    assert(capturedDataCount==2 && capturedDataSeq[0]==10 && capturedDataSeq[1]==546 && ticks==1);
    assert(g_sockets[i].tcpSndUna==1082 && g_sockets[i].tcpSndNxt==1082);
    g_tcpTwoPacketFlight=false;currentProcess=nullptr;SockClose(i);delete waiter;
    // One vectored send fills MSS slots across boundaries in both sender
    // modes. An empty vector may have an invalid base without being read.
    for (bool flight : {false,true}) {
        waiter=new Process{};waiter->refCount=1;
        i=create();setupTcp(i);currentProcess=waiter;ticks=1;g_sockets[i].tcpSndWnd=65535;
        g_tcpTwoPacketFlight=flight;acknowledgeDataCount=2;capturedDataCount=0;
        uint8_t a[300]{},b[772]{};
        TcpSendVector v[]={{reinterpret_cast<uint64_t>(a),sizeof(a)},{~uint64_t(0),0},
                           {reinterpret_cast<uint64_t>(b),sizeof(b)}};
        assert(SockSendUserVector(i,v,3,1072)==1072);
        assert(capturedDataCount==2 && capturedDataSeq[0]==10 && capturedDataSeq[1]==546);
        g_tcpTwoPacketFlight=false;currentProcess=nullptr;SockClose(i);delete waiter;
    }
    // A failure in the next vector cannot reserve its slot; the preceding
    // acknowledged slot is returned as partial progress.
    for (bool flight : {false,true}) {
        waiter=new Process{};waiter->refCount=1;
        i=create();setupTcp(i);currentProcess=waiter;ticks=1;g_sockets[i].tcpSndWnd=65535;
        g_tcpTwoPacketFlight=flight;acknowledgeDataCount=1;failUserCopyAfter=1;
        uint8_t a[536]{},b[536]{};
        TcpSendVector v[]={{reinterpret_cast<uint64_t>(a),sizeof(a)},
                           {reinterpret_cast<uint64_t>(b),sizeof(b)}};
        assert(SockSendUserVector(i,v,2,1072)==536);
        assert(g_sockets[i].tcpSndNxt==546);
        failUserCopyAfter=-1;g_tcpTwoPacketFlight=false;currentProcess=nullptr;SockClose(i);delete waiter;
    }
    // A retry uses the retained vector bytes, not the modified user source.
    for (bool flight : {false,true}) {
        waiter=new Process{};waiter->refCount=1;
        i=create();setupTcp(i);currentProcess=waiter;ticks=1;g_sockets[i].tcpSndWnd=65535;
        g_tcpTwoPacketFlight=flight;uint8_t value=3;stagedPayloadCount=0;
        mutateUserOnSend=&value;skipDataAcks=1;acknowledgeDataCount=1;
        TcpSendVector v[]={{reinterpret_cast<uint64_t>(&value),1}};
        assert(SockSendUserVector(i,v,1,1)==1);
        assert(value==9 && stagedPayloadCount==2 && stagedPayloads[0]==3 && stagedPayloads[1]==3);
        mutateUserOnSend=nullptr;g_tcpTwoPacketFlight=false;currentProcess=nullptr;SockClose(i);delete waiter;
    }
    // A window change during caller-side copy must be observed before any
    // sequence reservation. Partial credit trims the first packet; zero
    // credit publishes nothing until a later window update.
    waiter=new Process{};waiter->refCount=1;
    i=create();setupTcp(i);currentProcess=waiter;ticks=1;g_sockets[i].tcpSndWnd=65535;
    g_tcpTwoPacketFlight=true;capturedDataCount=0;acknowledgeDataCount=3;
    shrinkWindowOnCopy=i;copyWindowValue=200;dataAckWindow=200;
    {uint8_t data[536]{};assert(SockSendUser(i,reinterpret_cast<uint64_t>(data),sizeof(data))==536);}
    assert(capturedDataCount==3 && capturedDataSeq[0]==10 && capturedDataSeq[1]==210 && capturedDataSeq[2]==410);
    dataAckWindow=65535;g_tcpTwoPacketFlight=false;currentProcess=nullptr;SockClose(i);delete waiter;
    waiter=new Process{};waiter->refCount=1;
    i=create();setupTcp(i);currentProcess=waiter;ticks=1;g_sockets[i].tcpSndWnd=65535;
    g_tcpTwoPacketFlight=true;capturedDataCount=0;acknowledgeDataCount=1;lastDataSubmitTick=0;
    shrinkWindowOnCopy=i;copyWindowValue=0;windowUpdateSocket=i;windowUpdateTick=11;
    {uint8_t data=7;assert(SockSendUser(i,reinterpret_cast<uint64_t>(&data),1)==1);}
    assert(capturedDataCount==1 && capturedDataSeq[0]==10 && lastDataSubmitTick>=11);
    g_tcpTwoPacketFlight=false;currentProcess=nullptr;SockClose(i);delete waiter;
    // A fault after a valid prefix within the same MSS must discard the
    // unfinished slot, without reserving or transmitting any of that prefix.
    for (bool flight : {false,true}) {
        waiter=new Process{};waiter->refCount=1;
        i=create();setupTcp(i);currentProcess=waiter;ticks=1;g_sockets[i].tcpSndWnd=65535;
        g_tcpTwoPacketFlight=flight;capturedDataCount=0;failUserCopyAfter=1;
        uint8_t a[300]{},b[236]{};
        TcpSendVector v[]={{reinterpret_cast<uint64_t>(a),sizeof(a)},
                           {reinterpret_cast<uint64_t>(b),sizeof(b)}};
        assert(SockSendUserVector(i,v,2,536)==-14);
        assert(capturedDataCount==0 && g_sockets[i].tcpSndNxt==10);
        failUserCopyAfter=-1;g_tcpTwoPacketFlight=false;currentProcess=nullptr;SockClose(i);delete waiter;
    }
    // Four-slot control: cumulative ACK after all four submissions, and local
    // EAGAIN at each position must retry the same range without a hole.
    for(unsigned failAt=0;failAt<=4;++failAt){
        waiter=new Process{};waiter->refCount=1;
        i=create();setupTcp(i);currentProcess=waiter;ticks=1;g_sockets[i].tcpSndWnd=65535;
        g_tcpFourPacketFlight=true;skipDataAcks=3;acknowledgeDataCount=1;capturedDataCount=0;failDataAttempt=failAt;
        {uint8_t data[2144]{};assert(SockSend(i,data,sizeof(data))==sizeof(data));}
        assert(capturedDataCount==(failAt?5u:4u) && g_sockets[i].tcpSndUna==2154 && g_sockets[i].tcpSndNxt==2154);
        if(failAt)assert(capturedDataSeq[failAt-1]==capturedDataSeq[failAt]);
        g_tcpFourPacketFlight=false;currentProcess=nullptr;SockClose(i);delete waiter;
    }
    // Reuse ring slots over a longer stream, including a short final packet.
    waiter=new Process{};waiter->refCount=1;
    i=create();setupTcp(i);currentProcess=waiter;ticks=1;g_sockets[i].tcpSndWnd=65535;
    g_tcpFourPacketFlight=true;skipDataAcks=3;acknowledgeDataCount=10;capturedDataCount=0;
    {uint8_t data[6439]{};assert(SockSend(i,data,sizeof(data))==sizeof(data));}
    assert(g_sockets[i].tcpSndUna==6449 && g_sockets[i].tcpSndNxt==6449);
    acknowledgeDataCount=0;g_tcpFourPacketFlight=false;currentProcess=nullptr;SockClose(i);delete waiter;
    // Drop the second of four packets; later packets elicit duplicate ACKs
    // and the timer retransmits the missing middle before completing.
    waiter=new Process{};waiter->refCount=1;
    i=create();setupTcp(i);currentProcess=waiter;ticks=1;g_sockets[i].tcpSndWnd=65535;
    g_tcpFourPacketFlight=true;fourLossMode=true;fourLossCount=0;capturedDataCount=0;
    {uint8_t data[2144]{};assert(SockSend(i,data,sizeof(data))==sizeof(data));}
    assert(fourLossCount==5 && capturedDataSeq[0]==10 && capturedDataSeq[1]==546 &&
           capturedDataSeq[2]==1082 && capturedDataSeq[3]==1618 && capturedDataSeq[4]==546);
    assert(g_sockets[i].tcpSndUna==2154 && g_sockets[i].tcpSndNxt==2154);
    fourLossMode=false;g_tcpFourPacketFlight=false;currentProcess=nullptr;SockClose(i);delete waiter;
    // ACK-driven growth persists across calls, reaches a bounded eight-MSS
    // flight, and is reset when the socket slot is reused.
    waiter=new Process{};waiter->refCount=1;
    i=create();setupTcp(i);currentProcess=waiter;ticks=1;g_sockets[i].tcpSndWnd=65535;
    g_tcpAdaptiveFlight=true;
    for(unsigned n=0;n<6;++n){uint8_t data[536]{};acknowledgeDataCount=1;assert(SockSend(i,data,sizeof(data))==sizeof(data));}
    assert(g_sockets[i].tcpFlightControl.cwnd==8*536);
    skipDataAcks=7;acknowledgeDataCount=1;capturedDataCount=0;
    {uint8_t data[8*536]{};assert(SockSend(i,data,sizeof(data))==sizeof(data));}
    assert(capturedDataCount==8 && g_sockets[i].tcpFlightControl.cwnd==8*536);
    const int oldSlot=i;g_tcpAdaptiveFlight=false;currentProcess=nullptr;SockClose(i);delete waiter;
    i=create();assert(i==oldSlot && g_sockets[i].tcpFlightControl.cwnd==0);SockClose(i);
    // The separate 16-slot mode starts at two MSS, grows across calls, and
    // reuses the larger ring over several turns without leaking its buffer.
    waiter=new Process{};waiter->refCount=1;
    i=create();setupTcp(i);currentProcess=waiter;ticks=1;g_sockets[i].tcpSndWnd=65535;
    g_tcpAdaptiveFlight=true;g_tcpAdaptiveFlight16=true;
    skipDataAcks=1;acknowledgeDataCount=1;capturedDataCount=0;
    {uint8_t data[1072]{};assert(SockSend(i,data,sizeof(data))==sizeof(data));}
    assert(capturedDataCount==2 && g_sockets[i].tcpFlightControl.cwnd==4*536);
    for(unsigned n=0;n<12;++n){uint8_t data[536]{};acknowledgeDataCount=1;assert(SockSend(i,data,sizeof(data))==sizeof(data));}
    assert(g_sockets[i].tcpFlightControl.cwnd==16*536);
    const unsigned wideFrees=frees.load();skipDataAcks=15;acknowledgeDataCount=40;capturedDataCount=0;
    {uint8_t data[48*536]{};assert(SockSend(i,data,sizeof(data))==sizeof(data));}
    assert(capturedDataCount==48 && frees==wideFrees+1);
    for(unsigned n=1;n<48;++n)assert(capturedDataSeq[n]==capturedDataSeq[n-1]+536);
    acknowledgeDataCount=0;g_tcpAdaptiveFlight16=false;g_tcpAdaptiveFlight=false;
    currentProcess=nullptr;SockClose(i);delete waiter;
    // Allocation failure precedes any sequence reservation or transmission.
    waiter=new Process{};waiter->refCount=1;
    i=create();setupTcp(i);currentProcess=waiter;ticks=1;g_sockets[i].tcpSndWnd=65535;
    g_tcpAdaptiveFlight=true;g_tcpAdaptiveFlight16=true;capturedDataCount=0;failAllocation=0;
    {uint8_t data[536]{};assert(SockSend(i,data,sizeof(data))==-12);}
    assert(capturedDataCount==0 && g_sockets[i].tcpSndNxt==10 && g_sockets[i].tcpSndUna==10);
    failAllocation=-1;g_tcpAdaptiveFlight16=false;g_tcpAdaptiveFlight=false;
    currentProcess=nullptr;SockClose(i);delete waiter;
    // RTO reduces the flight before a retransmission and clears its recovery
    // episode only after forward ACK progress.
    waiter=new Process{};waiter->refCount=1;
    i=create();setupTcp(i);currentProcess=waiter;ticks=1;g_sockets[i].tcpSndWnd=65535;
    g_tcpAdaptiveFlight=true;skipDataAcks=1;acknowledgeDataCount=1;capturedDataCount=0;observeFlightControlSocket=i;
    {uint8_t data[536]{};assert(SockSend(i,data,sizeof(data))==sizeof(data));}
    assert(observedRetryCwnd==536 && observedRetryThreshold==1072 && !g_sockets[i].tcpFlightControl.recovering);
    observeFlightControlSocket=-1;g_tcpAdaptiveFlight=false;currentProcess=nullptr;SockClose(i);delete waiter;
    // Opt-in diagnostics count real sends and exclude retransmitted segments
    // from timing samples, without changing their completion semantics.
    waiter=new Process{};waiter->refCount=1;
    i=create();setupTcp(i);currentProcess=waiter;ticks=1;g_sockets[i].tcpSndWnd=65535;
    g_tcpAdaptiveFlight=true;g_tcpSendProbe=true;g_tcpSendProbeStats={};
    acknowledgeDataCount=1;
    {uint8_t data[536]{};assert(SockSend(i,data,sizeof(data))==sizeof(data));}
    assert(g_tcpSendProbeStats.calls==1 && g_tcpSendProbeStats.bytes==536 &&
           g_tcpSendProbeStats.submitted==536 && g_tcpSendProbeStats.acked==536 &&
           g_tcpSendProbeStats.samples==1 && g_tcpSendProbeStats.retries==0);
    skipDataAcks=1;acknowledgeDataCount=1;
    {uint8_t data[536]{};assert(SockSend(i,data,sizeof(data))==sizeof(data));}
    assert(g_tcpSendProbeStats.calls==2 && g_tcpSendProbeStats.samples==1 && g_tcpSendProbeStats.retries==1);
    g_tcpSendProbe=false;g_tcpAdaptiveFlight=false;currentProcess=nullptr;SockClose(i);delete waiter;
    // Two ACKs before the sender resumes use the latest observed ACK time,
    // not a claimed first-covering RTT.
    waiter=new Process{};waiter->refCount=1;
    i=create();setupTcp(i);currentProcess=waiter;ticks=1;g_sockets[i].tcpSndWnd=65535;
    g_tcpAdaptiveFlight=true;g_tcpSendProbe=true;g_tcpSendProbeStats={};
    skipDataAcks=2;deadlineAckTick=21;deadlineAckValue=546;deadlineSecondAckValue=1082;deadlineAckSocket=i;
    {uint8_t data[1072]{};assert(SockSend(i,data,sizeof(data))==sizeof(data));}
    assert(g_tcpSendProbeStats.samples==2 && g_tcpSendProbeStats.rttTicks==46 &&
           g_tcpSendProbeStats.resumeTicks==0 && g_tcpSendProbeStats.drainWaits>0);
    g_tcpSendProbe=false;g_tcpAdaptiveFlight=false;currentProcess=nullptr;SockClose(i);delete waiter;
    // A full eight-slot flight counts both slot and congestion waits even
    // before the ninth segment can be reserved.
    waiter=new Process{};waiter->refCount=1;
    i=create();setupTcp(i);currentProcess=waiter;ticks=1;g_sockets[i].tcpSndWnd=65535;
    g_tcpAdaptiveFlight=true;g_tcpSendProbe=true;g_tcpSendProbeStats={};
    g_sockets[i].tcpFlightControl.Init(536);g_sockets[i].tcpFlightControl.Ack(6*536,536);
    skipDataAcks=8;acknowledgeDataCount=1;deadlineAckTick=21;deadlineAckValue=10+8*536;deadlineAckSocket=i;
    {uint8_t data[9*536]{};assert(SockSend(i,data,sizeof(data))==sizeof(data));}
    assert(g_tcpSendProbeStats.slotWaits>0 && g_tcpSendProbeStats.congestionWaits>0 &&
           g_tcpSendProbeStats.maxOutstanding==8*536);
    g_tcpSendProbe=false;g_tcpAdaptiveFlight=false;currentProcess=nullptr;SockClose(i);delete waiter;
    // A lost middle packet in an adaptive flight must recover the missing
    // range, preserve later submitted bytes, and reduce the congestion window.
    for(bool wide : {false,true}) {
    waiter=new Process{};waiter->refCount=1;
    i=create();setupTcp(i);currentProcess=waiter;ticks=1;g_sockets[i].tcpSndWnd=65535;
    g_tcpAdaptiveFlight=true;g_tcpAdaptiveFlight16=wide;g_sockets[i].tcpFlightControl.Init(536,wide?16:8);
    g_sockets[i].tcpFlightControl.Ack(2*536,536,wide?16:8);
    fourLossMode=true;fourLossCount=0;capturedDataCount=0;observeFlightControlSocket=i;
    {uint8_t data[2144]{};assert(SockSend(i,data,sizeof(data))==sizeof(data));}
    assert(fourLossCount==5 && capturedDataSeq[0]==10 && capturedDataSeq[1]==546 &&
           capturedDataSeq[2]==1082 && capturedDataSeq[3]==1618 && capturedDataSeq[4]==546);
    assert(observedRetryCwnd==536 && observedRetryThreshold>=2*536);
    assert(g_sockets[i].tcpSndUna==2154 && g_sockets[i].tcpSndNxt==2154 &&
           !g_sockets[i].tcpFlightControl.recovering);
    observeFlightControlSocket=-1;fourLossMode=false;g_tcpAdaptiveFlight16=false;g_tcpAdaptiveFlight=false;
    currentProcess=nullptr;SockClose(i);delete waiter;
    }
    // An EAGAIN tail stays unsent after head RTO shrinks cwnd, until forward
    // ACK progress grants credit again.
    for(bool wide : {false,true}) {
    waiter=new Process{};waiter->refCount=1;
    i=create();setupTcp(i);currentProcess=waiter;ticks=1;g_sockets[i].tcpSndWnd=65535;
    g_tcpAdaptiveFlight=true;g_tcpAdaptiveFlight16=wide;capturedDataCount=0;skipDataAcks=2;acknowledgeDataCount=1;
    rejectDataSeq=546;rejectDataRemaining=20;firstDataRetryTick=0;
    deadlineAckTick=251;deadlineAckValue=546;deadlineAckSocket=i;
    {uint8_t data[1072]{};assert(SockSend(i,data,sizeof(data))==sizeof(data));}
    assert(deadlineAckSocket==-1 && capturedDataCount==23 && capturedDataSeq[0]==10 &&
           capturedDataSeq[1]==546 && firstDataRetryTick==201 && lastDataSubmitTick>=251);
    g_tcpAdaptiveFlight16=false;g_tcpAdaptiveFlight=false;currentProcess=nullptr;SockClose(i);delete waiter;
    }
    // Wide mode preserves wraparound, forged-ACK rejection, partial-copy and
    // window-reopen behavior through its separately allocated flight buffer.
    waiter=new Process{};waiter->refCount=1;
    i=create();setupTcp(i);currentProcess=waiter;ticks=1;g_sockets[i].tcpSndWnd=65535;
    g_tcpAdaptiveFlight=true;g_tcpAdaptiveFlight16=true;g_sockets[i].tcpSndNxt=g_sockets[i].tcpSndUna=0xffffff00u;
    skipDataAcks=1;acknowledgeDataCount=1;
    {uint8_t data[1072]{};assert(SockSend(i,data,sizeof(data))==sizeof(data));}
    assert(g_sockets[i].tcpSndUna==0xffffff00u+1072u);
    g_tcpAdaptiveFlight16=false;g_tcpAdaptiveFlight=false;currentProcess=nullptr;SockClose(i);delete waiter;
    waiter=new Process{};waiter->refCount=1;
    i=create();setupTcp(i);currentProcess=waiter;ticks=1;g_sockets[i].tcpSndWnd=65535;
    g_tcpAdaptiveFlight=true;g_tcpAdaptiveFlight16=true;failDataSubmissions=1;ackUnsentOnFailure=true;
    {uint8_t data=0;assert(SockSend(i,&data,1)==-5);}
    assert(g_sockets[i].tcpState==TcpState::Closed);
    g_tcpAdaptiveFlight16=false;g_tcpAdaptiveFlight=false;currentProcess=nullptr;SockClose(i);delete waiter;
    waiter=new Process{};waiter->refCount=1;
    i=create();setupTcp(i);currentProcess=waiter;ticks=1;g_sockets[i].tcpSndWnd=65535;
    g_tcpAdaptiveFlight=true;g_tcpAdaptiveFlight16=true;failUserCopyAfter=1;deadlineAckTick=11;deadlineAckSocket=i;
    {uint8_t data[1072]{};assert(SockSendUser(i,reinterpret_cast<uint64_t>(data),sizeof(data))==536);}
    assert(g_sockets[i].tcpState==TcpState::Established);failUserCopyAfter=-1;
    g_tcpAdaptiveFlight16=false;g_tcpAdaptiveFlight=false;currentProcess=nullptr;SockClose(i);delete waiter;
    waiter=new Process{};waiter->refCount=1;
    i=create();setupTcp(i);currentProcess=waiter;ticks=1;g_sockets[i].tcpSndWnd=0;
    g_tcpAdaptiveFlight=true;g_tcpAdaptiveFlight16=true;windowUpdateTick=11;windowUpdateSocket=i;acknowledgeDataCount=1;
    {uint8_t data=0;assert(SockSend(i,&data,1)==1);}
    assert(windowUpdateSocket==-1 && g_sockets[i].tcpSndNxt==11);
    g_tcpAdaptiveFlight16=false;g_tcpAdaptiveFlight=false;currentProcess=nullptr;SockClose(i);delete waiter;
    waiter=new Process{};waiter->refCount=1;
    i=create();setupTcp(i);currentProcess=waiter;ticks=1;g_sockets[i].tcpSndWnd=65535;
    g_tcpAdaptiveFlight=true;g_tcpAdaptiveFlight16=true;flightPartialMode=1;flightPartialCount=0;
    {uint8_t data[1072]{};assert(SockSend(i,data,sizeof(data))==sizeof(data));}
    assert(flightPartialCount==3 && flightRetrySeq==20 && flightRetryLen==526);
    flightPartialMode=0;g_tcpAdaptiveFlight16=false;g_tcpAdaptiveFlight=false;currentProcess=nullptr;SockClose(i);delete waiter;
    waiter=new Process{};waiter->refCount=1;
    i=create();setupTcp(i);currentProcess=waiter;ticks=1;g_sockets[i].tcpSndWnd=65535;
    g_tcpAdaptiveFlight=true;g_tcpAdaptiveFlight16=true;signalOnDataSend=true;
    {uint8_t data[1072]{};assert(SockSend(i,data,sizeof(data))==-4);}
    assert(g_sockets[i].tcpState==TcpState::Closed);
    g_tcpAdaptiveFlight16=false;g_tcpAdaptiveFlight=false;currentProcess=nullptr;SockClose(i);delete waiter;
    waiter=new Process{};waiter->refCount=1;
    i=create();setupTcp(i);currentProcess=waiter;ticks=1;g_sockets[i].tcpSndWnd=0;
    g_tcpAdaptiveFlight=true;g_tcpAdaptiveFlight16=true;
    {uint8_t data=0;assert(SockSend(i,&data,1)==-110);}
    assert(g_sockets[i].tcpSndNxt==10 && g_sockets[i].tcpState==TcpState::Established);
    g_tcpAdaptiveFlight16=false;g_tcpAdaptiveFlight=false;currentProcess=nullptr;SockClose(i);delete waiter;
    // Reconnecting an existing socket starts with fresh congestion state.
    i=create();g_sockets[i].tcpFlightControl.Init(536);g_sockets[i].tcpFlightControl.Ack(536,536);
    SockSetNonblock(i,true);
    {SockAddrIn address{};address.sin_family=AF_INET;address.sin_addr=htonl(0x7f000001);address.sin_port=htons(200);
     assert(SockConnect(i,&address)==-115);}
    assert(g_sockets[i].tcpFlightControl.cwnd==0 && !g_sockets[i].tcpFlightControl.recovering);
    SockClose(i);
    // A rejected second submission retries its original range and payload.
    waiter=new Process{};waiter->refCount=1;
    i=create();setupTcp(i);currentProcess=waiter;ticks=1;g_sockets[i].tcpSndWnd=65535;
    g_tcpTwoPacketFlight=true;failDataAttempt=2;skipDataAcks=1;acknowledgeDataCount=1;capturedDataCount=0;
    {uint8_t data[1072]{};assert(SockSend(i,data,sizeof(data))==sizeof(data));}
    assert(capturedDataCount==3 && capturedDataSeq[0]==10 && capturedDataSeq[1]==546 && capturedDataSeq[2]==546);
    g_tcpTwoPacketFlight=false;currentProcess=nullptr;SockClose(i);delete waiter;
    // Sequence arithmetic across wrap still permits two outstanding packets.
    waiter=new Process{};waiter->refCount=1;
    i=create();setupTcp(i);currentProcess=waiter;ticks=1;g_sockets[i].tcpSndWnd=65535;
    g_sockets[i].tcpSndNxt=g_sockets[i].tcpSndUna=0xffffff00u;
    g_tcpTwoPacketFlight=true;skipDataAcks=1;acknowledgeDataCount=1;capturedDataCount=0;
    {uint8_t data[1072]{};assert(SockSend(i,data,sizeof(data))==sizeof(data));}
    assert(g_sockets[i].tcpSndUna==0xffffff00u+1072u && capturedDataCount==2);
    g_tcpTwoPacketFlight=false;currentProcess=nullptr;SockClose(i);delete waiter;
    // A failed second copy drains the first outstanding packet and returns
    // its acknowledged bytes, preserving the stream.
    waiter=new Process{};waiter->refCount=1;
    i=create();setupTcp(i);currentProcess=waiter;ticks=1;g_sockets[i].tcpSndWnd=65535;
    g_tcpTwoPacketFlight=true;failUserCopyAfter=1;deadlineAckTick=11;deadlineAckSocket=i;
    {uint8_t data[1072]{};assert(SockSendUser(i,reinterpret_cast<uint64_t>(data),sizeof(data))==536);}
    assert(deadlineAckSocket==-1 && g_sockets[i].tcpState==TcpState::Established && SockGetConnectError(i)==0);
    failUserCopyAfter=-1;g_tcpTwoPacketFlight=false;currentProcess=nullptr;SockClose(i);delete waiter;
    // A pending signal while bytes are outstanding closes the stream rather
    // than abandoning retained retransmission data.
    waiter=new Process{};waiter->refCount=1;
    i=create();setupTcp(i);currentProcess=waiter;ticks=1;g_sockets[i].tcpSndWnd=65535;
    g_tcpTwoPacketFlight=true;signalOnDataSend=true;
    {uint8_t data[1072]{};assert(SockSend(i,data,sizeof(data))==-4);}
    assert(g_sockets[i].tcpState==TcpState::Closed && SockGetConnectError(i)==4);
    g_tcpTwoPacketFlight=false;currentProcess=nullptr;SockClose(i);delete waiter;
    // A peer ACK cannot turn an EAGAIN-rejected local reservation into a
    // successful write. The compromised stream is made terminal.
    waiter=new Process{};waiter->refCount=1;
    i=create();setupTcp(i);currentProcess=waiter;ticks=1;g_sockets[i].tcpSndWnd=65535;
    g_tcpTwoPacketFlight=true;failDataSubmissions=1;ackUnsentOnFailure=true;
    {uint8_t data=0;assert(SockSend(i,&data,1)==-5);}
    assert(g_sockets[i].tcpState==TcpState::Closed && SockGetConnectError(i)==5);
    g_tcpTwoPacketFlight=false;currentProcess=nullptr;SockClose(i);delete waiter;
    // A local retry waits when the peer closes its window after reservation.
    waiter=new Process{};waiter->refCount=1;
    i=create();setupTcp(i);currentProcess=waiter;ticks=1;g_sockets[i].tcpSndWnd=65535;
    g_tcpTwoPacketFlight=true;failDataSubmissions=1;shrinkWindowOnFailure=true;
    windowUpdateTick=51;windowUpdateSocket=i;acknowledgeDataCount=1;
    {uint8_t data=0;assert(SockSend(i,&data,1)==1);}
    assert(windowUpdateSocket==-1 && lastDataSubmitTick>=51 && g_sockets[i].tcpSndUna==11);
    g_tcpTwoPacketFlight=false;currentProcess=nullptr;SockClose(i);delete waiter;
    // A window-blocked unsent second slot must not suppress retransmission
    // of a lost first slot. Reopen the window only after that retransmission.
    waiter=new Process{};waiter->refCount=1;
    i=create();setupTcp(i);currentProcess=waiter;ticks=1;g_sockets[i].tcpSndWnd=65535;
    g_tcpTwoPacketFlight=true;capturedDataCount=0;skipDataAcks=1;acknowledgeDataCount=2;
    failDataAttempt=2;shrinkWindowOnFailure=true;failureAckOverride=10;dataAckWindow=0;
    windowUpdateTick=251;windowUpdateSocket=i;
    {uint8_t data[1072]{};assert(SockSend(i,data,sizeof(data))==sizeof(data));}
    assert(capturedDataCount==4 && capturedDataSeq[0]==10 && capturedDataSeq[1]==546 &&
           capturedDataSeq[2]==10 && capturedDataSeq[3]==546);
    assert(windowUpdateSocket==-1 && lastDataSubmitTick>=251);
    dataAckWindow=65535;g_tcpTwoPacketFlight=false;currentProcess=nullptr;SockClose(i);delete waiter;
    // Repeated local EAGAIN for the second packet cannot starve the first
    // packet's retransmission timer.
    waiter=new Process{};waiter->refCount=1;
    i=create();setupTcp(i);currentProcess=waiter;ticks=1;g_sockets[i].tcpSndWnd=65535;
    g_tcpTwoPacketFlight=true;capturedDataCount=0;skipDataAcks=1;acknowledgeDataCount=2;
    rejectDataSeq=546;rejectDataRemaining=25;firstDataRetryTick=0;
    {uint8_t data[1072]{};assert(SockSend(i,data,sizeof(data))==sizeof(data));}
    assert(firstDataRetryTick==201 && rejectDataRemaining==0);
    g_tcpTwoPacketFlight=false;currentProcess=nullptr;SockClose(i);delete waiter;
    // Partial ACK followed by loss retries only the oldest missing suffix.
    waiter=new Process{};waiter->refCount=1;
    i=create();setupTcp(i);currentProcess=waiter;ticks=1;g_sockets[i].tcpSndWnd=65535;
    g_tcpTwoPacketFlight=true;flightPartialMode=1;flightPartialCount=0;
    {uint8_t data[1072]{};assert(SockSend(i,data,sizeof(data))==sizeof(data));}
    assert(flightPartialCount==3 && flightRetrySeq==20 && flightRetryLen==526);
    assert(g_sockets[i].tcpSndUna==1082 && g_sockets[i].tcpSndNxt==1082);
    flightPartialMode=0;g_tcpTwoPacketFlight=false;currentProcess=nullptr;SockClose(i);delete waiter;
    waiter=new Process{};waiter->refCount=1;
    i=create();setupTcp(i);currentProcess=waiter;ticks=1;g_sockets[i].tcpSndWnd=65535;
    g_tcpTwoPacketFlight=true;flightPartialMode=2;flightPartialCount=0;
    {uint8_t data[1072]{};assert(SockSend(i,data,sizeof(data))==sizeof(data));}
    assert(flightPartialCount==3 && flightRetrySeq==556 && flightRetryLen==526);
    flightPartialMode=0;g_tcpTwoPacketFlight=false;currentProcess=nullptr;SockClose(i);delete waiter;
    // Zero-window wait consumes no sequence space; a later window-only ACK
    // permits progress, while a persistent zero window ends without aborting.
    waiter=new Process{};waiter->refCount=1;
    i=create();setupTcp(i);currentProcess=waiter;ticks=1;g_sockets[i].tcpSndWnd=0;
    g_tcpTwoPacketFlight=true;windowUpdateTick=11;windowUpdateSocket=i;acknowledgeDataCount=1;
    {uint8_t data=0;assert(SockSend(i,&data,1)==1);}
    assert(windowUpdateSocket==-1 && g_sockets[i].tcpSndNxt==11);
    g_tcpTwoPacketFlight=false;currentProcess=nullptr;SockClose(i);delete waiter;
    waiter=new Process{};waiter->refCount=1;
    i=create();setupTcp(i);currentProcess=waiter;ticks=1;g_sockets[i].tcpSndWnd=0;g_tcpTwoPacketFlight=true;
    {uint8_t data=0;assert(SockSend(i,&data,1)==-110);}
    assert(g_sockets[i].tcpSndNxt==10 && g_sockets[i].tcpState==TcpState::Established);
    g_tcpTwoPacketFlight=false;currentProcess=nullptr;SockClose(i);delete waiter;
    // Local EAGAIN retries the same sequence rather than reserving a hole.
    waiter=new Process{};waiter->refCount=1;
    i=create();setupTcp(i);currentProcess=waiter;ticks=1;g_sockets[i].tcpSndWnd=65535;
    g_tcpTwoPacketFlight=true;failDataSubmissions=1;acknowledgeDataCount=2;capturedDataCount=0;
    {uint8_t data[1072]{};assert(SockSend(i,data,sizeof(data))==sizeof(data));}
    assert(capturedDataCount==3 && capturedDataSeq[0]==10 && capturedDataSeq[1]==10);
    assert(g_sockets[i].tcpSndUna==1082 && g_sockets[i].tcpSndNxt==1082);
    g_tcpTwoPacketFlight=false;currentProcess=nullptr;SockClose(i);delete waiter;
    // Previously ACKed chunks are reported even when the next one times out.
    waiter=new Process{};waiter->refCount=1;
    i=create();setupTcp(i);currentProcess=waiter;ticks=1;acknowledgeDataCount=1;
    {uint8_t data[537]{};assert(SockSend(i,data,sizeof(data))==536);}
    assert(g_sockets[i].tcpState==TcpState::Closed && SockGetConnectError(i)==110);
    currentProcess=nullptr;SockClose(i);delete waiter;
    // Faulting user copies never reserve the next sequence range, and a
    // later fault returns only previously acknowledged bytes.
    waiter=new Process{};waiter->refCount=1;
    i=create();setupTcp(i);currentProcess=waiter;ticks=1;failUserCopyAfter=0;
    {uint8_t data[537]{};assert(SockSendUser(i,reinterpret_cast<uint64_t>(data),sizeof(data))==-14);}
    assert(g_sockets[i].tcpSndNxt==10 && g_sockets[i].tcpState==TcpState::Established);
    failUserCopyAfter=1;acknowledgeDataCount=1;
    {uint8_t data[537]{};assert(SockSendUser(i,reinterpret_cast<uint64_t>(data),sizeof(data))==536);}
    assert(g_sockets[i].tcpSndNxt==546 && g_sockets[i].tcpState==TcpState::Established);
    failUserCopyAfter=-1;currentProcess=nullptr;SockClose(i);delete waiter;
    // A mutation after transmission must not alter a retransmission.
    waiter=new Process{};waiter->refCount=1;
    i=create();setupTcp(i);currentProcess=waiter;ticks=1;
    {uint8_t data=3;stagedPayloadCount=0;mutateUserOnSend=&data;skipDataAcks=1;acknowledgeDataCount=1;
     assert(SockSendUser(i,reinterpret_cast<uint64_t>(&data),1)==1);
     assert(data==9 && stagedPayloadCount==2 && stagedPayloads[0]==3 && stagedPayloads[1]==3);mutateUserOnSend=nullptr;}
    currentProcess=nullptr;SockClose(i);delete waiter;
    // Data received before the timeout stays readable before the error.
    waiter=new Process{};waiter->refCount=1;
    i=create();setupTcp(i);currentProcess=waiter;ticks=1;
    SpinLockAcquire(&g_sockets[i].lock);TcpEnqueueData(g_sockets[i],&payload,1);SpinLockRelease(&g_sockets[i].lock);
    assert(SockSend(i,&payload,1)==-110);
    recvByte=0;assert(SockRecv(i,&recvByte,1,true)==1 && recvByte==payload);
    assert(SockRecv(i,&recvByte,1,true)==-104);
    currentProcess=nullptr;SockClose(i);delete waiter;
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
