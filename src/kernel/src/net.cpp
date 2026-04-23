// net.cpp — Brook OS network stack.
//
// Ethernet → ARP / IPv4 → ICMP / UDP
// Single-NIC, single-threaded receive path.

#include "net.h"
#include "tcp.h"
#include "serial.h"
#include "kprintf.h"
#include "scheduler.h"
#include "process.h"
#include "memory/heap.h"
#include "memory/physical_memory.h"
#include "klog.h"
#include "profiler.h"
#include "smp.h"
#include "rtc.h"
#include "apic.h"
#include "vfs.h"

// External C functions used by debug channel
extern "C" void MouseGetPosition(int32_t*, int32_t*);
extern "C" uint8_t MouseGetButtons();
extern "C" bool MouseIsAvailable();

namespace brook {

// ---------------------------------------------------------------------------
// Global state
// ---------------------------------------------------------------------------

static NetIf* g_netIfs[NET_MAX_IFS] = {};
static uint32_t g_netIfCount = 0;
// Back-compat alias used by legacy single-NIC code paths below. Points at
// g_netIfs[0] (the primary / default-route interface).
static NetIf* g_netIf = nullptr;

// ARP cache — small fixed table
static constexpr uint32_t ARP_CACHE_SIZE = 32;

struct ArpEntry {
    uint32_t ip;
    MacAddr  mac;
    bool     valid;
};

static ArpEntry g_arpCache[ARP_CACHE_SIZE];
static uint32_t g_arpCount = 0;

// Pending ARP resolution
static volatile bool g_arpReplyPending = false;
static uint32_t g_arpReplyIp = 0;
static MacAddr  g_arpReplyMac;
static Process* g_arpWaiter = nullptr;

// Socket table
// TODO: Replace with per-process dynamic socket pool to support high socket
// counts. The current global fixed table limits the whole system to
// MAX_SOCKETS simultaneous sockets. Each socket heap-allocates 64KB for its
// RX ring, so the practical limit is bounded by physical RAM anyway.
static constexpr uint32_t MAX_SOCKETS = 1024;
static Socket g_sockets[MAX_SOCKETS];
static bool   g_sockUsed[MAX_SOCKETS];

// IPv4 identification counter
static uint16_t g_ipId = 1;

static uint32_t g_tcpEphemeralPort = 49200; // accessed via atomic fetch-add

// Forward declaration for TCP send (defined below)
static void TcpSendSegment(Socket& s, uint8_t flags,
                           const void* data, uint32_t dataLen,
                           const char* why = "?");

// ---------------------------------------------------------------------------
// Checksum helpers
// ---------------------------------------------------------------------------

static uint16_t InetChecksum(const void* data, uint32_t len)
{
    const uint16_t* p = static_cast<const uint16_t*>(data);
    uint32_t sum = 0;
    while (len > 1) {
        sum += *p++;
        len -= 2;
    }
    if (len == 1)
        sum += *reinterpret_cast<const uint8_t*>(p);
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return static_cast<uint16_t>(~sum);
}

// ---------------------------------------------------------------------------
// Memory helpers
// ---------------------------------------------------------------------------

static void* NetMemset(void* dst, int val, uint64_t n)
{
    uint8_t* d = static_cast<uint8_t*>(dst);
    for (uint64_t i = 0; i < n; i++) d[i] = static_cast<uint8_t>(val);
    return dst;
}

static void* NetMemcpy(void* dst, const void* src, uint64_t n)
{
    uint8_t* d = static_cast<uint8_t*>(dst);
    const uint8_t* s = static_cast<const uint8_t*>(src);
    for (uint64_t i = 0; i < n; i++) d[i] = s[i];
    return dst;
}



// ---------------------------------------------------------------------------
// ARP cache
// ---------------------------------------------------------------------------

void ArpCacheInsert(uint32_t ip, const MacAddr& mac)
{
    // Update existing entry
    for (uint32_t i = 0; i < g_arpCount; i++) {
        if (g_arpCache[i].ip == ip) {
            g_arpCache[i].mac = mac;
            g_arpCache[i].valid = true;
            return;
        }
    }
    // Insert new
    if (g_arpCount < ARP_CACHE_SIZE) {
        g_arpCache[g_arpCount].ip = ip;
        g_arpCache[g_arpCount].mac = mac;
        g_arpCache[g_arpCount].valid = true;
        g_arpCount++;
    }
}

static bool ArpCacheLookup(uint32_t ip, MacAddr* out)
{
    for (uint32_t i = 0; i < g_arpCount; i++) {
        if (g_arpCache[i].ip == ip && g_arpCache[i].valid) {
            *out = g_arpCache[i].mac;
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// ARP send/receive
// ---------------------------------------------------------------------------

static void ArpSendRequest(uint32_t targetIp)
{
    NetIf* nif = NetIfForDst(targetIp);
    if (!nif) return;

    uint8_t frame[42]; // 14 eth + 28 arp
    NetMemset(frame, 0, sizeof(frame));

    auto* eth = reinterpret_cast<EthHeader*>(frame);
    NetMemset(eth->dst.b, 0xFF, 6); // broadcast
    eth->src = nif->mac;
    eth->etherType = htons(ETH_TYPE_ARP);

    auto* arp = reinterpret_cast<ArpPacket*>(frame + sizeof(EthHeader));
    arp->htype = htons(1);
    arp->ptype = htons(0x0800);
    arp->hlen  = 6;
    arp->plen  = 4;
    arp->oper  = htons(ARP_OP_REQUEST);
    arp->sha   = nif->mac;
    arp->spa   = nif->ipAddr;
    NetMemset(arp->tha.b, 0, 6);
    arp->tpa   = targetIp;

    nif->transmit(nif, frame, 42);
}

static void ArpSendReply(NetIf* nif, const MacAddr& dstMac, uint32_t dstIp)
{
    if (!nif) return;

    uint8_t frame[42];
    NetMemset(frame, 0, sizeof(frame));

    auto* eth = reinterpret_cast<EthHeader*>(frame);
    eth->dst = dstMac;
    eth->src = nif->mac;
    eth->etherType = htons(ETH_TYPE_ARP);

    auto* arp = reinterpret_cast<ArpPacket*>(frame + sizeof(EthHeader));
    arp->htype = htons(1);
    arp->ptype = htons(0x0800);
    arp->hlen  = 6;
    arp->plen  = 4;
    arp->oper  = htons(ARP_OP_REPLY);
    arp->sha   = nif->mac;
    arp->spa   = nif->ipAddr;
    arp->tha   = dstMac;
    arp->tpa   = dstIp;

    nif->transmit(nif, frame, 42);
}

static void HandleArp(NetIf* nif, const uint8_t* frame, uint32_t len)
{
    if (len < sizeof(EthHeader) + sizeof(ArpPacket)) return;

    auto* arp = reinterpret_cast<const ArpPacket*>(frame + sizeof(EthHeader));
    uint16_t op = ntohs(arp->oper);

    // Always learn from any ARP packet; only log when the cache entry is new
    // to avoid spamming the serial log with repeated gratuitous ARPs from the
    // gateway (QEMU sends one every few seconds for each host).
    MacAddr existing;
    bool isNew = !ArpCacheLookup(arp->spa, &existing);
    ArpCacheInsert(arp->spa, arp->sha);
    if (isNew)
        SerialPrintf("arp: RX op=%u spa=%d.%d.%d.%d sha=%02x:%02x:%02x:%02x:%02x:%02x tpa=%d.%d.%d.%d\n",
                     op,
                     arp->spa & 0xFF, (arp->spa >> 8) & 0xFF,
                     (arp->spa >> 16) & 0xFF, (arp->spa >> 24) & 0xFF,
                     arp->sha.b[0], arp->sha.b[1], arp->sha.b[2],
                     arp->sha.b[3], arp->sha.b[4], arp->sha.b[5],
                     arp->tpa & 0xFF, (arp->tpa >> 8) & 0xFF,
                     (arp->tpa >> 16) & 0xFF, (arp->tpa >> 24) & 0xFF);

    if (op == ARP_OP_REQUEST) {
        // Is this for us? Check against the receiving interface's IP.
        if (nif && arp->tpa == nif->ipAddr) {
            ArpSendReply(nif, arp->sha, arp->spa);
        }
    } else if (op == ARP_OP_REPLY) {

        // Wake up anyone waiting for this ARP reply
        if (arp->spa == g_arpReplyIp) {
            g_arpReplyMac = arp->sha;
            __asm__ volatile("mfence" ::: "memory");
            g_arpReplyPending = true;
            Process* waiter = g_arpWaiter;
            if (waiter) {
                g_arpWaiter = nullptr;
                __atomic_store_n(&waiter->pendingWakeup, 1, __ATOMIC_RELEASE);
                SchedulerUnblock(waiter);
            }
        }
    }
}

bool ArpResolve(uint32_t ip, MacAddr* outMac)
{
    // Check cache first
    if (ArpCacheLookup(ip, outMac))
        return true;

    // Broadcast address
    if (ip == 0xFFFFFFFF) {
        NetMemset(outMac->b, 0xFF, 6);
        return true;
    }

    // Need to send ARP request and wait
    g_arpReplyPending = false;
    g_arpReplyIp = ip;
    __asm__ volatile("mfence" ::: "memory");

    extern volatile uint64_t g_lapicTickCount;
    Process* self = SchedulerCurrentProcess();

    for (int attempt = 0; attempt < 3; attempt++) {
        ArpSendRequest(ip);

        // Block until ARP reply arrives (500ms timeout per attempt)
        uint64_t deadline = g_lapicTickCount + 500;
        while (!g_arpReplyPending && g_lapicTickCount < deadline) {
            if (self) {
                g_arpWaiter = self;
                self->wakeupTick = g_lapicTickCount + 500;
                SchedulerBlock(self);
            } else if (g_netIf && g_netIf->poll) {
                // Early-boot: no scheduler yet, fall back to polling
                g_netIf->poll(g_netIf);
            }
        }

        if (g_arpReplyPending) {
            *outMac = g_arpReplyMac;
            ArpCacheInsert(ip, *outMac);
            return true;
        }
    }

    SerialPrintf("net: ARP resolve failed for %d.%d.%d.%d\n",
                 ip & 0xFF, (ip >> 8) & 0xFF,
                 (ip >> 16) & 0xFF, (ip >> 24) & 0xFF);
    return false;
}

// ---------------------------------------------------------------------------
// IPv4 send/receive
// ---------------------------------------------------------------------------

static void HandleIpv4(const uint8_t* frame, uint32_t len);

int NetSendIpv4(uint32_t dstIp, uint8_t proto,
                const void* payload, uint32_t payloadLen)
{
    NetIf* nif = NetIfForDst(dstIp);
    if (!nif || !nif->ipAddr) return -1;

    // Loopback: if destination is our own IP on this iface, inject locally
    if (dstIp == nif->ipAddr)
    {
        uint32_t ipLen = sizeof(Ipv4Header) + payloadLen;
        uint32_t frameLen = sizeof(EthHeader) + ipLen;
        if (frameLen > ETH_FRAME_MAX) return -3;

        uint8_t frame[ETH_FRAME_MAX];
        NetMemset(frame, 0, frameLen);

        auto* eth = reinterpret_cast<EthHeader*>(frame);
        eth->src = nif->mac;
        eth->dst = nif->mac;
        eth->etherType = htons(ETH_TYPE_IPV4);

        auto* ip = reinterpret_cast<Ipv4Header*>(frame + sizeof(EthHeader));
        ip->verIhl   = 0x45;
        ip->totalLen = htons(static_cast<uint16_t>(ipLen));
        ip->id       = htons(g_ipId++);
        ip->flagsFrag = htons(0x4000);
        ip->ttl      = 64;
        ip->protocol = proto;
        ip->srcIp    = nif->ipAddr;
        ip->dstIp    = dstIp;
        ip->checksum = 0;
        ip->checksum = InetChecksum(ip, sizeof(Ipv4Header));

        NetMemcpy(frame + sizeof(EthHeader) + sizeof(Ipv4Header), payload, payloadLen);
        HandleIpv4(frame, frameLen);
        return 0;
    }

    // Determine next-hop: if same subnet, send direct; else use gateway
    uint32_t nextHop = dstIp;
    if ((dstIp & nif->netmask) != (nif->ipAddr & nif->netmask)) {
        nextHop = nif->gateway;
        if (!nextHop) return -1; // no gateway configured
    }

    // Resolve MAC
    MacAddr dstMac;
    if (!ArpResolve(nextHop, &dstMac))
        return -2; // ARP failed

    // Build frame
    uint32_t ipLen = sizeof(Ipv4Header) + payloadLen;
    uint32_t frameLen = sizeof(EthHeader) + ipLen;
    if (frameLen > ETH_FRAME_MAX) return -3;

    uint8_t frame[ETH_FRAME_MAX];
    NetMemset(frame, 0, frameLen);

    auto* eth = reinterpret_cast<EthHeader*>(frame);
    eth->dst = dstMac;
    eth->src = nif->mac;
    eth->etherType = htons(ETH_TYPE_IPV4);

    auto* ip = reinterpret_cast<Ipv4Header*>(frame + sizeof(EthHeader));
    ip->verIhl   = 0x45;
    ip->tos      = 0;
    ip->totalLen = htons(static_cast<uint16_t>(ipLen));
    ip->id       = htons(g_ipId++);
    ip->flagsFrag = htons(0x4000);
    ip->ttl      = 64;
    ip->protocol = proto;
    ip->srcIp    = nif->ipAddr;
    ip->dstIp    = dstIp;
    ip->checksum = 0;
    ip->checksum = InetChecksum(ip, sizeof(Ipv4Header));

    NetMemcpy(frame + sizeof(EthHeader) + sizeof(Ipv4Header), payload, payloadLen);

    return nif->transmit(nif, frame, frameLen);
}

int NetSendUdp(uint32_t dstIp, uint16_t srcPort, uint16_t dstPort,
               const void* data, uint32_t dataLen)
{
    uint32_t udpLen = sizeof(UdpHeader) + dataLen;
    if (udpLen > ETH_MTU - sizeof(Ipv4Header)) return -1;

    uint8_t buf[ETH_MTU];
    auto* udp = reinterpret_cast<UdpHeader*>(buf);
    udp->srcPort  = htons(srcPort);
    udp->dstPort  = htons(dstPort);
    udp->length   = htons(static_cast<uint16_t>(udpLen));
    udp->checksum = 0; // optional for UDP over IPv4

    NetMemcpy(buf + sizeof(UdpHeader), data, dataLen);

    return NetSendIpv4(dstIp, IP_PROTO_UDP, buf, udpLen);
}

// ---------------------------------------------------------------------------
// ICMP handling
// ---------------------------------------------------------------------------

static void HandleIcmp(const Ipv4Header* ip, const uint8_t* payload, uint32_t payloadLen)
{
    if (payloadLen < sizeof(IcmpHeader)) return;

    auto* icmp = reinterpret_cast<const IcmpHeader*>(payload);

    if (icmp->type == ICMP_ECHO_REQUEST && icmp->code == 0) {
        SerialPrintf("net: ICMP echo request from %d.%d.%d.%d\n",
                     ip->srcIp & 0xFF, (ip->srcIp >> 8) & 0xFF,
                     (ip->srcIp >> 16) & 0xFF, (ip->srcIp >> 24) & 0xFF);

        // Build echo reply — copy entire ICMP payload
        uint8_t reply[ETH_MTU];
        if (payloadLen > sizeof(reply)) return;

        NetMemcpy(reply, payload, payloadLen);
        auto* replyIcmp = reinterpret_cast<IcmpHeader*>(reply);
        replyIcmp->type = ICMP_ECHO_REPLY;
        replyIcmp->checksum = 0;
        replyIcmp->checksum = InetChecksum(reply, payloadLen);

        NetSendIpv4(ip->srcIp, IP_PROTO_ICMP, reply, payloadLen);
    }
}

// ---------------------------------------------------------------------------
// UDP handling
// ---------------------------------------------------------------------------

// Forward declarations
static void HandleUdpWithDhcp(const Ipv4Header* ip,
                               const uint8_t* payload, uint32_t payloadLen);
static bool IsDnsReply(uint16_t dstPort, const void* data, uint32_t len);

// ---------------------------------------------------------------------------
// IPv4 receive
// ---------------------------------------------------------------------------

static void HandleIpv4(const uint8_t* frame, uint32_t len)
{
    if (len < sizeof(EthHeader) + sizeof(Ipv4Header)) return;

    auto* ip = reinterpret_cast<const Ipv4Header*>(frame + sizeof(EthHeader));
    uint8_t ihl = (ip->verIhl & 0x0F) * 4;
    uint16_t totalLen = ntohs(ip->totalLen);

    if (totalLen < ihl) return;
    if (sizeof(EthHeader) + totalLen > len) return;

    // Check destination: accept if it targets any of our interface IPs,
    // the global broadcast, or any of our subnet-directed broadcasts.
    bool forUs = (ip->dstIp == 0xFFFFFFFF);
    if (!forUs) {
        for (uint32_t i = 0; i < g_netIfCount; i++) {
            NetIf* n = g_netIfs[i];
            if (!n) continue;
            if (ip->dstIp == n->ipAddr) { forUs = true; break; }
            if (n->netmask &&
                (ip->dstIp | n->netmask) == 0xFFFFFFFF &&
                (ip->dstIp & n->netmask) == (n->ipAddr & n->netmask)) {
                forUs = true; break;
            }
        }
    }
    if (!forUs) return;

    const uint8_t* payload = frame + sizeof(EthHeader) + ihl;
    uint32_t payloadLen = totalLen - ihl;

    switch (ip->protocol) {
    case IP_PROTO_ICMP:
        HandleIcmp(ip, payload, payloadLen);
        break;
    case IP_PROTO_UDP:
        HandleUdpWithDhcp(ip, payload, payloadLen);
        break;
    case IP_PROTO_TCP:
        HandleTcp(ip, payload, payloadLen);
        break;
    }
}

// ---------------------------------------------------------------------------
// Frame receive entry point (called by NIC driver)
// ---------------------------------------------------------------------------

void NetReceive(NetIf* nif, const void* frame, uint32_t len)
{
    if (len < sizeof(EthHeader)) return;

    auto* eth = reinterpret_cast<const EthHeader*>(frame);
    uint16_t etherType = ntohs(eth->etherType);

    switch (etherType) {
    case ETH_TYPE_ARP:
        HandleArp(nif, static_cast<const uint8_t*>(frame), len);
        break;
    case ETH_TYPE_IPV4:
        HandleIpv4(static_cast<const uint8_t*>(frame), len);
        break;
    }
}

// ---------------------------------------------------------------------------
// Network interface management
// ---------------------------------------------------------------------------

void NetInit()
{
    NetMemset(g_arpCache, 0, sizeof(g_arpCache));
    g_arpCount = 0;
    NetMemset(g_sockets, 0, sizeof(g_sockets));
    NetMemset(g_sockUsed, 0, sizeof(g_sockUsed));
    g_netIf = nullptr;
    for (uint32_t i = 0; i < NET_MAX_IFS; i++) g_netIfs[i] = nullptr;
    g_netIfCount = 0;
    SerialPuts("net: initialised\n");
}

void NetRegisterIf(NetIf* nif)
{
    if (g_netIfCount >= NET_MAX_IFS) {
        SerialPrintf("net: too many interfaces (max %u), ignoring\n", NET_MAX_IFS);
        return;
    }
    uint32_t idx = g_netIfCount++;
    g_netIfs[idx] = nif;
    if (idx == 0) g_netIf = nif;
    SerialPrintf("net: interface %u registered, MAC=%02x:%02x:%02x:%02x:%02x:%02x\n",
                 idx,
                 nif->mac.b[0], nif->mac.b[1], nif->mac.b[2],
                 nif->mac.b[3], nif->mac.b[4], nif->mac.b[5]);
}

uint32_t NetIfCount() { return g_netIfCount; }
NetIf* NetIfAt(uint32_t idx) { return (idx < g_netIfCount) ? g_netIfs[idx] : nullptr; }

// Pick the interface that should source packets destined for dstIp.
// Prefers subnet match; falls back to NIC 0 (default route).
NetIf* NetIfForDst(uint32_t dstIp)
{
    for (uint32_t i = 0; i < g_netIfCount; i++) {
        NetIf* nif = g_netIfs[i];
        if (!nif || !nif->ipAddr || !nif->netmask) continue;
        if ((dstIp & nif->netmask) == (nif->ipAddr & nif->netmask))
            return nif;
    }
    return g_netIf;
}

void NetStartPollThread()
{
    if (!g_netIf) return;

    // Spawn a background thread to continuously drain the NIC receive ring.
    // This is needed because SockRecv/Connect block via SchedulerBlock rather
    // than busy-polling; without this thread no one would call nif->poll() and
    // incoming packets would never be delivered post-scheduler-init.
    //
    // Priority 2 (NORMAL) so it doesn't preempt user processes. The 1ms sleep
    // between drain bursts is enough for good TCP throughput while keeping the
    // scheduler lock free for timer/IRQ handlers (mouse, profiler wakeups etc.).
    //
    // DO NOT use SchedulerYield() here — it spins at high frequency, contends
    // g_readyLock constantly, and delays LAPIC timer handlers that need the
    // lock to check wakeupTick. Use SchedulerBlock with a short timeout instead.
    //
    // IMPORTANT: must be called AFTER SchedulerInit(), not from NetRegisterIf,
    // because SchedulerAddProcess requires the MLFQ to be initialised.
    Process* pollThread = KernelThreadCreate("net_poll", [](void* /*arg*/) {
        extern volatile uint64_t g_lapicTickCount;
        while (true) {
            uint32_t n = NetIfCount();
            for (int burst = 0; burst < 8; burst++) {
                for (uint32_t i = 0; i < n; i++) {
                    NetIf* nif = NetIfAt(i);
                    if (nif && nif->poll) nif->poll(nif);
                }
            }
            Process* self = ProcessCurrent();
            if (self) {
                self->wakeupTick = g_lapicTickCount + 1;
                SchedulerBlock(self);
            }
        }
    }, nullptr, 2 /* NORMAL priority — same as user processes */);
    if (pollThread) {
        SchedulerAddProcess(pollThread);
        SerialPrintf("net: net_poll thread started (pid=%u)\n", pollThread->pid);
    }
}

NetIf* NetGetIf()
{
    return g_netIf;
}

// ---------------------------------------------------------------------------
// DHCP (minimal — enough for QEMU user-mode networking)
// ---------------------------------------------------------------------------

// DHCP message types
static constexpr uint8_t DHCP_DISCOVER = 1;
static constexpr uint8_t DHCP_OFFER    = 2;
static constexpr uint8_t DHCP_REQUEST  = 3;
static constexpr uint8_t DHCP_ACK      = 5;

struct __attribute__((packed)) DhcpPacket {
    uint8_t  op;          // 1 = request, 2 = reply
    uint8_t  htype;       // 1 = Ethernet
    uint8_t  hlen;        // 6
    uint8_t  hops;
    uint32_t xid;
    uint16_t secs;
    uint16_t flags;
    uint32_t ciaddr;
    uint32_t yiaddr;      // "your" IP address
    uint32_t siaddr;      // server IP
    uint32_t giaddr;
    uint8_t  chaddr[16];  // client hardware address
    uint8_t  sname[64];
    uint8_t  file[128];
    uint32_t magic;       // 0x63825363
    uint8_t  options[312]; // variable-length options
};

static constexpr uint32_t DHCP_MAGIC = 0x63538263; // little-endian of 0x63825363

static uint32_t g_dhcpXid = 0x42524F4B; // "BROK"
static volatile bool g_dhcpDone = false;
static volatile uint8_t g_dhcpState = 0; // 0=idle, 1=discovering, 2=requesting, 3=done

static void DhcpSend(uint8_t msgType, uint32_t serverIp, uint32_t requestedIp)
{
    if (!g_netIf) return;

    // Build DHCP over UDP over IP over Ethernet
    uint8_t frame[ETH_FRAME_MAX];
    NetMemset(frame, 0, sizeof(frame));

    auto* eth = reinterpret_cast<EthHeader*>(frame);
    NetMemset(eth->dst.b, 0xFF, 6); // broadcast
    eth->src = g_netIf->mac;
    eth->etherType = htons(ETH_TYPE_IPV4);

    auto* ip = reinterpret_cast<Ipv4Header*>(frame + sizeof(EthHeader));
    uint32_t dhcpLen = sizeof(DhcpPacket);
    uint32_t udpLen  = sizeof(UdpHeader) + dhcpLen;
    uint32_t ipLen   = sizeof(Ipv4Header) + udpLen;

    ip->verIhl    = 0x45;
    ip->totalLen  = htons(static_cast<uint16_t>(ipLen));
    ip->id        = htons(g_ipId++);
    ip->flagsFrag = htons(0x4000);
    ip->ttl       = 64;
    ip->protocol  = IP_PROTO_UDP;
    ip->srcIp     = 0; // 0.0.0.0
    ip->dstIp     = 0xFFFFFFFF; // 255.255.255.255
    ip->checksum  = 0;
    ip->checksum  = InetChecksum(ip, sizeof(Ipv4Header));

    auto* udp = reinterpret_cast<UdpHeader*>(frame + sizeof(EthHeader) + sizeof(Ipv4Header));
    udp->srcPort  = htons(68);  // DHCP client
    udp->dstPort  = htons(67);  // DHCP server
    udp->length   = htons(static_cast<uint16_t>(udpLen));
    udp->checksum = 0;

    auto* dhcp = reinterpret_cast<DhcpPacket*>(frame + sizeof(EthHeader) + sizeof(Ipv4Header) + sizeof(UdpHeader));
    dhcp->op    = 1; // BOOTREQUEST
    dhcp->htype = 1;
    dhcp->hlen  = 6;
    dhcp->xid   = g_dhcpXid;
    dhcp->flags = htons(0x8000); // broadcast flag
    NetMemcpy(dhcp->chaddr, g_netIf->mac.b, 6);
    dhcp->magic = DHCP_MAGIC;

    // Options
    uint8_t* opt = dhcp->options;
    // Option 53: DHCP Message Type
    *opt++ = 53; *opt++ = 1; *opt++ = msgType;

    if (msgType == DHCP_REQUEST) {
        // Option 50: Requested IP
        *opt++ = 50; *opt++ = 4;
        NetMemcpy(opt, &requestedIp, 4); opt += 4;
        // Option 54: Server Identifier
        *opt++ = 54; *opt++ = 4;
        NetMemcpy(opt, &serverIp, 4); opt += 4;
    }

    // Option 55: Parameter Request List
    *opt++ = 55; *opt++ = 3;
    *opt++ = 1;   // Subnet Mask
    *opt++ = 3;   // Router
    *opt++ = 6;   // DNS

    // End
    *opt++ = 255;

    uint32_t frameLen = sizeof(EthHeader) + ipLen;
    g_netIf->transmit(g_netIf, frame, frameLen);
}

// Process a DHCP reply (called from UDP receive path)
static void HandleDhcpReply(const uint8_t* data, uint32_t len)
{
    // Minimum: op(1)+htype(1)+hlen(1)+hops(1)+xid(4)+secs(2)+flags(2)+
    //   ciaddr(4)+yiaddr(4)+siaddr(4)+giaddr(4)+chaddr(16)+sname(64)+file(128)+magic(4) = 240
    if (len < 240) return;

    auto* dhcp = reinterpret_cast<const DhcpPacket*>(data);
    if (dhcp->op != 2) return; // not a reply
    if (dhcp->xid != g_dhcpXid) return;
    if (dhcp->magic != DHCP_MAGIC) return;

    // Parse options
    uint8_t msgType = 0;
    uint32_t subnetMask = 0;
    uint32_t router = 0;
    uint32_t dns = 0;
    uint32_t serverIp = 0;

    const uint8_t* opt = dhcp->options;
    const uint8_t* end = data + len;
    while (opt < end && *opt != 255) {
        uint8_t code = *opt++;
        if (code == 0) continue; // padding
        if (opt >= end) break;
        uint8_t olen = *opt++;
        if (opt + olen > end) break;

        switch (code) {
        case 53: if (olen >= 1) msgType = opt[0]; break;
        case 1:  if (olen >= 4) NetMemcpy(&subnetMask, opt, 4); break;
        case 3:  if (olen >= 4) NetMemcpy(&router, opt, 4); break;
        case 6:  if (olen >= 4) NetMemcpy(&dns, opt, 4); break;
        case 54: if (olen >= 4) NetMemcpy(&serverIp, opt, 4); break;
        }
        opt += olen;
    }

    if (msgType == DHCP_OFFER && g_dhcpState == 1) {
        SerialPrintf("net: DHCP offer: %d.%d.%d.%d\n",
                     dhcp->yiaddr & 0xFF, (dhcp->yiaddr >> 8) & 0xFF,
                     (dhcp->yiaddr >> 16) & 0xFF, (dhcp->yiaddr >> 24) & 0xFF);
        g_dhcpState = 2;
        DhcpSend(DHCP_REQUEST, serverIp, dhcp->yiaddr);
    } else if (msgType == DHCP_ACK && g_dhcpState == 2) {
        g_netIf->ipAddr  = dhcp->yiaddr;
        g_netIf->netmask = subnetMask;
        g_netIf->gateway = router;
        g_netIf->dns     = dns;

        SerialPrintf("net: DHCP ACK — IP %d.%d.%d.%d mask %d.%d.%d.%d gw %d.%d.%d.%d\n",
                     g_netIf->ipAddr & 0xFF, (g_netIf->ipAddr >> 8) & 0xFF,
                     (g_netIf->ipAddr >> 16) & 0xFF, (g_netIf->ipAddr >> 24) & 0xFF,
                     subnetMask & 0xFF, (subnetMask >> 8) & 0xFF,
                     (subnetMask >> 16) & 0xFF, (subnetMask >> 24) & 0xFF,
                     router & 0xFF, (router >> 8) & 0xFF,
                     (router >> 16) & 0xFF, (router >> 24) & 0xFF);

        KPrintf("net: IP %d.%d.%d.%d (DHCP)\n",
                g_netIf->ipAddr & 0xFF, (g_netIf->ipAddr >> 8) & 0xFF,
                (g_netIf->ipAddr >> 16) & 0xFF, (g_netIf->ipAddr >> 24) & 0xFF);

        g_dhcpState = 3;
        g_dhcpDone = true;
    }
}

// ---- Static interface config via /boot/BROOK.CFG -------------------------
//
// Keys parsed (ASCII, one "KEY=value" per line, '#' comments):
//   NET0_MODE    — "static" or "dhcp" (default dhcp)
//   NET0_IP      — dotted-quad
//   NET0_NETMASK — dotted-quad (default 255.255.255.0)
//   NET0_GATEWAY — dotted-quad (optional)
//   NET0_DNS     — dotted-quad (optional)

static bool ParseDottedQuad(const char* s, uint32_t len, uint32_t* out)
{
    uint32_t ip = 0;
    uint32_t octet = 0;
    uint32_t octetCount = 0;
    bool haveDigit = false;
    for (uint32_t i = 0; i <= len; i++) {
        char c = (i < len) ? s[i] : '.';
        if (c >= '0' && c <= '9') {
            octet = octet * 10 + (uint32_t)(c - '0');
            if (octet > 255) return false;
            haveDigit = true;
        } else if (c == '.') {
            if (!haveDigit) return false;
            ip |= (octet & 0xFF) << (8 * octetCount);
            octetCount++;
            octet = 0;
            haveDigit = false;
            if (octetCount == 4) {
                if (i != len) return false;
                *out = ip;
                return true;
            }
        } else {
            return false;
        }
    }
    return false;
}

static bool LineKeyIs(const char* line, uint32_t keyLen,
                      const char* key, uint32_t wantLen)
{
    if (keyLen != wantLen) return false;
    for (uint32_t i = 0; i < wantLen; i++) {
        if (line[i] != key[i]) return false;
    }
    return true;
}

bool NetApplyStaticConfig(NetIf* nif)
{
    if (!nif) return false;

    Vnode* cfg = VfsOpen("/boot/BROOK.CFG");
    if (!cfg) return false;

    char buf[1024] = {};
    uint64_t off = 0;
    int n = VfsRead(cfg, buf, sizeof(buf) - 1, &off);
    VfsClose(cfg);
    if (n <= 0) return false;

    bool isStatic = false;
    uint32_t ip = 0, mask = 0x00FFFFFF, gw = 0, dns = 0;  // mask default /24
    bool haveIp = false;

    uint32_t pos = 0;
    while (pos < (uint32_t)n) {
        uint32_t lineStart = pos;
        while (pos < (uint32_t)n && buf[pos] != '\n' && buf[pos] != '\r') pos++;
        uint32_t lineEnd = pos;
        while (pos < (uint32_t)n && (buf[pos] == '\n' || buf[pos] == '\r')) pos++;

        while (lineStart < lineEnd && (buf[lineStart] == ' ' || buf[lineStart] == '\t'))
            lineStart++;
        if (lineStart >= lineEnd) continue;
        if (buf[lineStart] == '#') continue;

        uint32_t eq = lineStart;
        while (eq < lineEnd && buf[eq] != '=') eq++;
        if (eq >= lineEnd) continue;

        const char* key = &buf[lineStart];
        uint32_t keyLen = eq - lineStart;
        const char* val = &buf[eq + 1];
        uint32_t valLen = lineEnd - (eq + 1);

        while (valLen > 0 && (val[0] == ' ' || val[0] == '\t')) { val++; valLen--; }
        while (valLen > 0 && (val[valLen - 1] == ' ' || val[valLen - 1] == '\t'
                              || val[valLen - 1] == '\r')) { valLen--; }

        if (LineKeyIs(key, keyLen, "NET0_MODE", 9)) {
            if (valLen == 6 && val[0] == 's' && val[1] == 't' && val[2] == 'a'
                            && val[3] == 't' && val[4] == 'i' && val[5] == 'c') {
                isStatic = true;
            }
        } else if (LineKeyIs(key, keyLen, "NET0_IP", 7)) {
            uint32_t v;
            if (ParseDottedQuad(val, valLen, &v)) { ip = v; haveIp = true; }
        } else if (LineKeyIs(key, keyLen, "NET0_NETMASK", 12)) {
            uint32_t v;
            if (ParseDottedQuad(val, valLen, &v)) mask = v;
        } else if (LineKeyIs(key, keyLen, "NET0_GATEWAY", 12)) {
            uint32_t v;
            if (ParseDottedQuad(val, valLen, &v)) gw = v;
        } else if (LineKeyIs(key, keyLen, "NET0_DNS", 8)) {
            uint32_t v;
            if (ParseDottedQuad(val, valLen, &v)) dns = v;
        }
    }

    if (!isStatic || !haveIp) return false;

    nif->ipAddr  = ip;
    nif->netmask = mask;
    nif->gateway = gw;
    nif->dns     = dns;

    SerialPrintf("net: static config %u.%u.%u.%u/%u.%u.%u.%u gw=%u.%u.%u.%u dns=%u.%u.%u.%u\n",
        ip & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF, (ip >> 24) & 0xFF,
        mask & 0xFF, (mask >> 8) & 0xFF, (mask >> 16) & 0xFF, (mask >> 24) & 0xFF,
        gw & 0xFF, (gw >> 8) & 0xFF, (gw >> 16) & 0xFF, (gw >> 24) & 0xFF,
        dns & 0xFF, (dns >> 8) & 0xFF, (dns >> 16) & 0xFF, (dns >> 24) & 0xFF);
    return true;
}

bool DhcpDiscover(NetIf* nif)
{
    g_dhcpState = 1;
    g_dhcpDone = false;

    for (int attempt = 0; attempt < 5; attempt++) {
        SerialPrintf("net: DHCP discover (attempt %d)\n", attempt + 1);
        DhcpSend(DHCP_DISCOVER, 0, 0);

        // Wait up to 2 seconds, polling for packets
        for (int i = 0; i < 2000000; i++) {
            // Poll the NIC for received packets (IRQ may not work during early boot)
            if (nif->poll) nif->poll(nif);
            __asm__ volatile("pause");
            if (g_dhcpDone) return true;
        }
    }

    SerialPuts("net: DHCP failed\n");
    return false;
}

// Override: check if incoming UDP is DHCP before delivering to sockets
static bool IsDhcpReply(uint16_t dstPort, const void* data, uint32_t len)
{
    if (dstPort == 68 && g_dhcpState > 0 && g_dhcpState < 3) {
        HandleDhcpReply(static_cast<const uint8_t*>(data), len);
        return true;
    }
    return false;
}

// Patched UDP handler that checks DHCP first
static void HandleUdpWithDhcp(const Ipv4Header* ip,
                               const uint8_t* payload, uint32_t payloadLen)
{
    if (payloadLen < sizeof(UdpHeader)) return;

    auto* udp = reinterpret_cast<const UdpHeader*>(payload);
    uint16_t srcPort = ntohs(udp->srcPort);
    uint16_t dstPort = ntohs(udp->dstPort);
    uint16_t udpLen  = ntohs(udp->length);

    if (udpLen < sizeof(UdpHeader) || udpLen > payloadLen) return;

    const uint8_t* data = payload + sizeof(UdpHeader);
    uint32_t dataLen = udpLen - sizeof(UdpHeader);

    // Check DHCP first, then DNS
    if (IsDhcpReply(dstPort, data, dataLen))
        return;
    if (IsDnsReply(dstPort, data, dataLen))
        return;

    SockDeliverUdp(ip->srcIp, srcPort, ip->dstIp, dstPort, data, dataLen);
}

// ---------------------------------------------------------------------------
// Socket layer
// ---------------------------------------------------------------------------

// DNS resolver
// ---------------------------------------------------------------------------

static constexpr uint16_t DNS_PORT = 53;
static constexpr uint16_t DNS_LOCAL_PORT = 10053;

// DNS header (RFC 1035)
struct __attribute__((packed)) DnsHeader {
    uint16_t id;
    uint16_t flags;
    uint16_t qdCount;
    uint16_t anCount;
    uint16_t nsCount;
    uint16_t arCount;
};

// DNS cache entry
struct DnsCacheEntry {
    char     name[128];
    uint32_t ip;       // big-endian
    uint32_t ttl;      // remaining TTL (not yet decremented)
};

static constexpr int DNS_CACHE_SIZE = 32;
static DnsCacheEntry g_dnsCache[DNS_CACHE_SIZE];
static int g_dnsCacheCount = 0;

// Pending DNS query state
static volatile bool g_dnsGotReply = false;
static volatile uint32_t g_dnsResolvedIp = 0;
static volatile uint16_t g_dnsQueryId = 0;
static Process* g_dnsWaiter = nullptr;

static int NetStrLen(const char* s)
{
    int n = 0;
    while (s[n]) n++;
    return n;
}

static bool NetStrEq(const char* a, const char* b)
{
    while (*a && *b) {
        if (*a != *b) return false;
        a++; b++;
    }
    return *a == *b;
}

// Encode a hostname in DNS wire format: "www.example.com" → 3www7example3com0
static int DnsEncodeName(uint8_t* buf, int maxLen, const char* name)
{
    int pos = 0;
    const char* p = name;

    while (*p) {
        const char* dot = p;
        while (*dot && *dot != '.') dot++;
        int labelLen = dot - p;
        if (labelLen == 0 || labelLen > 63) return -1;
        if (pos + 1 + labelLen >= maxLen) return -1;

        buf[pos++] = static_cast<uint8_t>(labelLen);
        for (int i = 0; i < labelLen; i++)
            buf[pos++] = static_cast<uint8_t>(p[i]);

        p = *dot ? dot + 1 : dot;
    }

    if (pos >= maxLen) return -1;
    buf[pos++] = 0; // root label
    return pos;
}

// Skip a DNS name (handles compression pointers)
static const uint8_t* DnsSkipName(const uint8_t* p, const uint8_t* end)
{
    while (p < end) {
        uint8_t len = *p;
        if (len == 0) return p + 1;
        if ((len & 0xC0) == 0xC0) return p + 2; // compression pointer
        p += 1 + len;
    }
    return nullptr;
}

// Parse a DNS response and extract the first A record
static uint32_t DnsParseResponse(const uint8_t* data, uint32_t len, uint16_t expectedId)
{
    if (len < sizeof(DnsHeader)) return 0;

    auto* hdr = reinterpret_cast<const DnsHeader*>(data);
    if (ntohs(hdr->id) != expectedId) return 0;

    uint16_t flags = ntohs(hdr->flags);
    if (!(flags & 0x8000)) return 0; // not a response
    if ((flags & 0x000F) != 0) return 0; // RCODE != 0 (error)

    uint16_t qdCount = ntohs(hdr->qdCount);
    uint16_t anCount = ntohs(hdr->anCount);

    const uint8_t* p = data + sizeof(DnsHeader);
    const uint8_t* end = data + len;

    // Skip question section
    for (uint16_t i = 0; i < qdCount && p < end; i++) {
        p = DnsSkipName(p, end);
        if (!p) return 0;
        p += 4; // QTYPE + QCLASS
    }

    // Parse answer section — find first A record
    for (uint16_t i = 0; i < anCount && p < end; i++) {
        p = DnsSkipName(p, end);
        if (!p || p + 10 > end) return 0;

        uint16_t rtype  = (p[0] << 8) | p[1];
        // uint16_t rclass = (p[2] << 8) | p[3];
        // uint32_t ttl = (p[4] << 24) | (p[5] << 16) | (p[6] << 8) | p[7];
        uint16_t rdlen  = (p[8] << 8) | p[9];
        p += 10;

        if (p + rdlen > end) return 0;

        if (rtype == 1 && rdlen == 4) { // A record
            uint32_t ip;
            NetMemcpy(&ip, p, 4);
            return ip; // big-endian
        }

        p += rdlen;
    }

    return 0;
}

// Handle incoming DNS response (called from UDP path)
static bool IsDnsReply(uint16_t dstPort, const void* data, uint32_t len)
{
    if (dstPort != DNS_LOCAL_PORT || !g_dnsQueryId) return false;

    uint32_t ip = DnsParseResponse(static_cast<const uint8_t*>(data), len, g_dnsQueryId);
    if (ip) {
        g_dnsResolvedIp = ip;
        g_dnsGotReply = true;
        Process* waiter = g_dnsWaiter;
        if (waiter) {
            g_dnsWaiter = nullptr;
            __atomic_store_n(&waiter->pendingWakeup, 1, __ATOMIC_RELEASE);
            SchedulerUnblock(waiter);
        }
    }
    return true;
}

uint32_t DnsCacheLookup(const char* hostname)
{
    for (int i = 0; i < g_dnsCacheCount; i++) {
        if (NetStrEq(g_dnsCache[i].name, hostname))
            return g_dnsCache[i].ip;
    }
    return 0;
}

static void DnsCacheInsert(const char* hostname, uint32_t ip)
{
    // Check if already cached
    for (int i = 0; i < g_dnsCacheCount; i++) {
        if (NetStrEq(g_dnsCache[i].name, hostname)) {
            g_dnsCache[i].ip = ip;
            return;
        }
    }

    // Insert new entry (overwrite oldest if full)
    int idx = g_dnsCacheCount < DNS_CACHE_SIZE ? g_dnsCacheCount++ : 0;
    int nameLen = NetStrLen(hostname);
    if (nameLen >= 127) nameLen = 127;
    NetMemcpy(g_dnsCache[idx].name, hostname, nameLen);
    g_dnsCache[idx].name[nameLen] = 0;
    g_dnsCache[idx].ip = ip;
}

uint32_t DnsResolve(const char* hostname)
{
    if (!g_netIf || !g_netIf->dns) return 0;

    // Check cache first
    uint32_t cached = DnsCacheLookup(hostname);
    if (cached) return cached;

    // Check if hostname is already a dotted-decimal IP
    // (simple check: starts with digit)

    // Build DNS query
    uint8_t pkt[512];
    NetMemset(pkt, 0, sizeof(pkt));

    static uint16_t s_dnsId = 1;
    uint16_t qid = s_dnsId++;

    auto* hdr = reinterpret_cast<DnsHeader*>(pkt);
    hdr->id      = htons(qid);
    hdr->flags   = htons(0x0100); // RD=1 (recursion desired)
    hdr->qdCount = htons(1);

    int nameLen = DnsEncodeName(pkt + sizeof(DnsHeader),
                                 sizeof(pkt) - sizeof(DnsHeader) - 4,
                                 hostname);
    if (nameLen < 0) return 0;

    uint8_t* qEnd = pkt + sizeof(DnsHeader) + nameLen;
    // QTYPE = A (1), QCLASS = IN (1)
    qEnd[0] = 0; qEnd[1] = 1; // A
    qEnd[2] = 0; qEnd[3] = 1; // IN
    uint32_t pktLen = sizeof(DnsHeader) + nameLen + 4;

    g_dnsGotReply = false;
    g_dnsResolvedIp = 0;
    g_dnsQueryId = qid;

    extern volatile uint64_t g_lapicTickCount;
    Process* self = SchedulerCurrentProcess();

    for (int attempt = 0; attempt < 3; attempt++) {
        SerialPrintf("net: DNS query '%s' (attempt %d)\n", hostname, attempt + 1);
        NetSendUdp(g_netIf->dns, DNS_LOCAL_PORT, DNS_PORT, pkt, pktLen);

        // Block until DNS reply arrives (2 second timeout per attempt)
        uint64_t deadline = g_lapicTickCount + 2000;
        while (!g_dnsGotReply && g_lapicTickCount < deadline) {
            if (self) {
                g_dnsWaiter = self;
                self->wakeupTick = g_lapicTickCount + 2000;
                SchedulerBlock(self);
            } else if (g_netIf->poll) {
                // Early-boot: no scheduler yet, fall back to polling
                g_netIf->poll(g_netIf);
            }
        }

        if (g_dnsGotReply) {
            uint32_t ip = g_dnsResolvedIp;
            g_dnsQueryId = 0;
            DnsCacheInsert(hostname, ip);

            SerialPrintf("net: DNS '%s' → %d.%d.%d.%d\n", hostname,
                         ip & 0xFF, (ip >> 8) & 0xFF,
                         (ip >> 16) & 0xFF, (ip >> 24) & 0xFF);
            return ip;
        }
    }

    SerialPrintf("net: DNS failed for '%s'\n", hostname);
    g_dnsQueryId = 0;
    return 0;
}

// ---------------------------------------------------------------------------

int SockCreate(int domain, int type, int protocol)
{
    if (domain != AF_INET) return -1;
    if (type != SOCK_DGRAM && type != SOCK_STREAM) return -1;

    for (uint32_t i = 0; i < MAX_SOCKETS; i++) {
        if (!g_sockUsed[i]) {
            g_sockUsed[i] = true;
            NetMemset(&g_sockets[i], 0, sizeof(Socket));
            g_sockets[i].domain = domain;
            g_sockets[i].type = type;
            g_sockets[i].protocol = protocol ? protocol :
                (type == SOCK_DGRAM ? IPPROTO_UDP : IPPROTO_TCP);

            // Allocate receive buffer
            g_sockets[i].rxBuf = static_cast<uint8_t*>(kmalloc(Socket::RX_BUF_SIZE));
            if (!g_sockets[i].rxBuf) {
                g_sockUsed[i] = false;
                return -1;
            }

            g_sockets[i].refCount = 1;
            {
                Process* self = ProcessCurrent();
                g_sockets[i].ownerPid = self ? self->tgid : 0;
            }
            return static_cast<int>(i);
        }
    }
    SerialPrintf("net: socket table full (MAX_SOCKETS=%u) — all slots in use\n", MAX_SOCKETS);
    return -1; // no free sockets
}

int SockBind(int sockIdx, const SockAddrIn* addr)
{
    if (sockIdx < 0 || sockIdx >= static_cast<int>(MAX_SOCKETS)) return -1;
    if (!g_sockUsed[sockIdx]) return -1;

    Socket& s = g_sockets[sockIdx];
    s.localIp   = addr->sin_addr;
    s.localPort  = addr->sin_port; // already big-endian
    s.bound = true;
    return 0;
}

int SockSendTo(int sockIdx, const void* buf, uint32_t len,
               const SockAddrIn* dest)
{
    if (sockIdx < 0 || sockIdx >= static_cast<int>(MAX_SOCKETS)) return -1;
    if (!g_sockUsed[sockIdx]) return -1;

    Socket& s = g_sockets[sockIdx];

    uint32_t dstIp;
    uint16_t dstPort;

    if (dest) {
        dstIp   = dest->sin_addr;
        dstPort = ntohs(dest->sin_port);
    } else if (s.connected) {
        dstIp   = s.remoteIp;
        dstPort = ntohs(s.remotePort);
    } else {
        return -1;
    }

    // Auto-bind if not yet bound
    if (!s.bound) {
        uint32_t port = __atomic_fetch_add(&g_tcpEphemeralPort, 1, __ATOMIC_RELAXED);
        if (port >= 65535) __atomic_store_n(&g_tcpEphemeralPort, 49200, __ATOMIC_RELAXED);
        s.localPort = htons(static_cast<uint16_t>(port));
        s.localIp = g_netIf ? g_netIf->ipAddr : 0;
        s.bound = true;
    }

    return NetSendUdp(dstIp, ntohs(s.localPort), dstPort, buf, len);
}

int SockRecvFrom(int sockIdx, void* buf, uint32_t len,
                 SockAddrIn* src)
{
    if (sockIdx < 0 || sockIdx >= static_cast<int>(MAX_SOCKETS)) return -1;
    if (!g_sockUsed[sockIdx]) return -1;

    Socket& s = g_sockets[sockIdx];

    // Brief poll to check for pending packets
    if (s.rxCount == 0 && g_netIf && g_netIf->poll) {
        for (int i = 0; i < 100000; i++) {
            g_netIf->poll(g_netIf);
            if (s.rxCount > 0) break;
            __asm__ volatile("pause");
        }
    }

    if (s.rxCount == 0) return -11; // EAGAIN

    // Read datagram length prefix (4 bytes) then data
    uint32_t dgLen = 0;
    for (int i = 0; i < 4; i++) {
        dgLen |= static_cast<uint32_t>(s.rxBuf[s.rxTail]) << (i * 8);
        s.rxTail = (s.rxTail + 1) % Socket::RX_BUF_SIZE;
    }

    // Read source IP (4 bytes) and port (2 bytes)
    uint32_t srcIp = 0;
    for (int i = 0; i < 4; i++) {
        srcIp |= static_cast<uint32_t>(s.rxBuf[s.rxTail]) << (i * 8);
        s.rxTail = (s.rxTail + 1) % Socket::RX_BUF_SIZE;
    }
    uint16_t srcPort = 0;
    srcPort = s.rxBuf[s.rxTail]; s.rxTail = (s.rxTail + 1) % Socket::RX_BUF_SIZE;
    srcPort |= static_cast<uint16_t>(s.rxBuf[s.rxTail]) << 8;
    s.rxTail = (s.rxTail + 1) % Socket::RX_BUF_SIZE;

    uint32_t copyLen = dgLen < len ? dgLen : len;
    for (uint32_t i = 0; i < copyLen; i++) {
        static_cast<uint8_t*>(buf)[i] = s.rxBuf[s.rxTail];
        s.rxTail = (s.rxTail + 1) % Socket::RX_BUF_SIZE;
    }
    // Skip remainder if dgLen > len
    for (uint32_t i = copyLen; i < dgLen; i++)
        s.rxTail = (s.rxTail + 1) % Socket::RX_BUF_SIZE;

    __asm__ volatile("mfence" ::: "memory");
    s.rxCount -= (10 + dgLen); // 4 len + 4 ip + 2 port + data

    if (src) {
        src->sin_family = AF_INET;
        src->sin_addr = srcIp;
        src->sin_port = htons(srcPort);
        NetMemset(src->sin_zero, 0, 8);
    }

    return static_cast<int>(copyLen);
}

void SockClose(int sockIdx)
{
    if (sockIdx < 0 || sockIdx >= static_cast<int>(MAX_SOCKETS)) return;
    if (!g_sockUsed[sockIdx]) return;

    Socket& s = g_sockets[sockIdx];

    // Listening socket: close all pending connections in accept queue
    if (s.listening)
    {
        for (int i = 0; i < s.acceptQueueCount; i++)
        {
            int childIdx = s.acceptQueue[i];
            if (childIdx >= 0 && childIdx < static_cast<int>(MAX_SOCKETS) && g_sockUsed[childIdx])
            {
                Socket& child = g_sockets[childIdx];
                if (child.tcpState == TcpState::Established ||
                    child.tcpState == TcpState::SynRecv)
                {
                    TcpSendSegment(child, TCP_RST, nullptr, 0, "rst-listener");
                }
                if (child.rxBuf) kfree(child.rxBuf);
                NetMemset(&g_sockets[childIdx], 0, sizeof(Socket));
                g_sockUsed[childIdx] = false;
            }
        }
    }

    // TCP: graceful close — send FIN and wait for the peer to acknowledge.
    //
    // We handle two states:
    //   Established: we initiate close (active close, FIN_WAIT1 path)
    //   CloseWait:   peer already sent FIN; we must send our own FIN to
    //                complete the 4-way handshake (passive close, LAST_ACK path).
    //
    // If we skip sending FIN in CloseWait, the peer stays in FIN_WAIT2 forever.
    // When the same local port is reused for a new connection to the same peer,
    // the peer sees a new SYN on a 4-tuple it thinks is still open and responds
    // with FIN/RST — killing the new connection immediately.
    if (s.type == SOCK_STREAM &&
        (s.tcpState == TcpState::Established ||
         s.tcpState == TcpState::CloseWait))
    {
        TcpSendSegment(s, TCP_FIN | TCP_ACK, nullptr, 0, "close");
        s.tcpSndNxt++;

        // Active close (Established): wait for FIN-ACK (FinWait1 → FinWait2 → done)
        // Passive close (CloseWait):  wait for ACK of our FIN (LastAck → Closed)
        s.tcpState = (s.tcpState == TcpState::Established)
                         ? TcpState::FinWait1
                         : TcpState::LastAck;

        // Wait up to 500ms for the peer to acknowledge our FIN.
        // We must use SchedulerBlock (not SchedulerYield) because SchedulerYield
        // returns immediately when no other thread is ready, so the FIN-ACK can
        // arrive via IRQ but we've already zeroed the socket. SchedulerBlock
        // suspends us and lets the IRQ handler wake us when the FIN-ACK arrives.
        extern volatile uint64_t g_lapicTickCount;
        Process* self = ProcessCurrent();
        if (self) {
            uint64_t deadline = g_lapicTickCount + 500;
            while (s.tcpState == TcpState::FinWait1 ||
                   s.tcpState == TcpState::FinWait2 ||
                   s.tcpState == TcpState::LastAck) {
                if (g_lapicTickCount >= deadline) break;
                uint64_t lf = SpinLockAcquire(&s.lock);
                s.pollWaiter = self;
                self->wakeupTick = g_lapicTickCount + 50; // check every 50ms
                SpinLockRelease(&s.lock, lf);
                SchedulerBlock(self);
            }
        }
    }

    if (s.rxBuf)
        kfree(s.rxBuf);

    NetMemset(&g_sockets[sockIdx], 0, sizeof(Socket));
    g_sockUsed[sockIdx] = false;
}

void SockRef(int sockIdx)
{
    if (sockIdx < 0 || sockIdx >= static_cast<int>(MAX_SOCKETS)) return;
    if (!g_sockUsed[sockIdx]) return;
    __atomic_fetch_add(&g_sockets[sockIdx].refCount, 1, __ATOMIC_RELEASE);
}

void SockUnref(int sockIdx)
{
    if (sockIdx < 0 || sockIdx >= static_cast<int>(MAX_SOCKETS)) return;
    if (!g_sockUsed[sockIdx]) return;
    uint32_t prev = __atomic_fetch_sub(&g_sockets[sockIdx].refCount, 1, __ATOMIC_ACQ_REL);
    if (prev <= 1)
        SockClose(sockIdx);
}

void SockSetPollWaiter(int sockIdx, Process* waiter)
{
    if (sockIdx < 0 || sockIdx >= static_cast<int>(MAX_SOCKETS)) return;
    if (!g_sockUsed[sockIdx]) return;
    g_sockets[sockIdx].pollWaiter = waiter;
}

void SockDeliverUdp(uint32_t srcIp, uint16_t srcPort,
                    uint32_t dstIp, uint16_t dstPort,
                    const void* data, uint32_t len)
{
    uint16_t dstPortBE = htons(dstPort);

    for (uint32_t i = 0; i < MAX_SOCKETS; i++) {
        if (!g_sockUsed[i]) continue;
        Socket& s = g_sockets[i];
        if (s.type != SOCK_DGRAM) continue;
        if (!s.bound) continue;
        if (s.localPort != dstPortBE) continue;

        // Match — enqueue datagram
        // Format: [len:4][srcIp:4][srcPort:2][data:len]
        uint32_t needed = 10 + len;

        uint64_t irqFlags = SpinLockAcquire(&s.lock);

        uint32_t avail = Socket::RX_BUF_SIZE - s.rxCount;
        if (needed > avail) {
            SpinLockRelease(&s.lock, irqFlags);
            SerialPrintf("net: socket %u rx buffer full, dropping\n", i);
            return;
        }

        // Write length
        for (int b = 0; b < 4; b++) {
            s.rxBuf[s.rxHead] = static_cast<uint8_t>(len >> (b * 8));
            s.rxHead = (s.rxHead + 1) % Socket::RX_BUF_SIZE;
        }
        // Write source IP
        for (int b = 0; b < 4; b++) {
            s.rxBuf[s.rxHead] = static_cast<uint8_t>(srcIp >> (b * 8));
            s.rxHead = (s.rxHead + 1) % Socket::RX_BUF_SIZE;
        }
        // Write source port
        s.rxBuf[s.rxHead] = static_cast<uint8_t>(srcPort);
        s.rxHead = (s.rxHead + 1) % Socket::RX_BUF_SIZE;
        s.rxBuf[s.rxHead] = static_cast<uint8_t>(srcPort >> 8);
        s.rxHead = (s.rxHead + 1) % Socket::RX_BUF_SIZE;
        // Write data
        const uint8_t* d = static_cast<const uint8_t*>(data);
        for (uint32_t j = 0; j < len; j++) {
            s.rxBuf[s.rxHead] = d[j];
            s.rxHead = (s.rxHead + 1) % Socket::RX_BUF_SIZE;
        }

        __asm__ volatile("mfence" ::: "memory");
        s.rxCount += needed;

        Process* waiter = s.pollWaiter;
        if (waiter) s.pollWaiter = nullptr;

        SpinLockRelease(&s.lock, irqFlags);

        // Wake poll waiter after releasing the lock
        if (waiter) {
            __atomic_store_n(&waiter->pendingWakeup, 1, __ATOMIC_RELEASE);
            SchedulerUnblock(waiter);
        }

        return; // only deliver to first matching socket
    }
}

// ---------------------------------------------------------------------------
// TCP implementation — delegates to tcp.cpp state machine
// ---------------------------------------------------------------------------

static void TcpSendSegment(Socket& s, uint8_t flags,
                           const void* data, uint32_t dataLen,
                           const char* why)
{
    // SYN packets include MSS option (4 bytes): kind=2, len=2, mss=1460
    // This makes us look like a real Linux client and avoids CDNs defaulting
    // to MSS=536 (which some servers use as a signal to reject the connection).
    static constexpr uint16_t TCP_OPT_MSS = 1460;
    bool addMss = (flags & TCP_SYN) && !(flags & TCP_ACK); // SYN only, not SYN-ACK

    uint32_t optLen  = addMss ? 4 : 0;   // MSS option is 4 bytes
    uint32_t tcpLen  = sizeof(TcpHeader) + optLen + dataLen;
    uint8_t buf[ETH_MTU];
    NetMemset(buf, 0, tcpLen);

    auto* tcp = reinterpret_cast<TcpHeader*>(buf);
    tcp->srcPort  = s.localPort;
    tcp->dstPort  = s.remotePort;
    tcp->seqNum   = htonl(s.tcpSndNxt);
    tcp->ackNum   = htonl(s.tcpRcvNxt);
    tcp->dataOff  = ((sizeof(TcpHeader) + optLen) / 4) << 4;
    tcp->flags    = flags;
    // Advertise actual free space to prevent the sender from overflowing our buffer
    {
        uint32_t freeSpace = Socket::RX_BUF_SIZE - s.rxCount;
        uint16_t wnd = freeSpace > 65535u ? 65535u : static_cast<uint16_t>(freeSpace);
        tcp->window = htons(wnd);
    }
    tcp->urgentPtr = 0;

    // Write MSS option immediately after the TCP header (SYN only)
    if (addMss) {
        uint8_t* opts = buf + sizeof(TcpHeader);
        opts[0] = 2;                                          // kind: MSS
        opts[1] = 4;                                          // length: 4 bytes
        opts[2] = static_cast<uint8_t>(TCP_OPT_MSS >> 8);    // MSS high byte
        opts[3] = static_cast<uint8_t>(TCP_OPT_MSS & 0xFF);  // MSS low byte
    }

    if (data && dataLen > 0)
        NetMemcpy(buf + sizeof(TcpHeader) + optLen, data, dataLen);

    tcp->checksum = 0;
    tcp->checksum = TcpChecksum(g_netIf->ipAddr, s.remoteIp, buf, tcpLen);

    int sendResult = NetSendIpv4(s.remoteIp, IP_PROTO_TCP, buf, tcpLen);
    if (sendResult != 0) {
        SerialPrintf("tcp: send failed flags=0x%02x err=%d\n", flags, sendResult);
    } else {
        s.txPktCount++;
        // Always log control segments (SYN/FIN/RST/PSH) and anything carrying
        // payload. Throttle pure ACKs (flags == ACK only, datalen == 0) the
        // same way we throttle RX: first 50, then every 100. During a bulk
        // download we emit thousands of pure ACKs and they drown the log.
        bool isPureAck = (flags == 0x10) && (dataLen == 0);
        bool log = !isPureAck || s.txPktCount <= 50 || (s.txPktCount % 100) == 0;
        if (log) {
            extern volatile uint64_t g_lapicTickCount;
            int sockIdx = static_cast<int>(&s - g_sockets);
            SerialPrintf("tcp: TX pid=%u sock=%d t=%lums flags=0x%02x seq=%u ack=%u datalen=%u why=%s [pkt#%u]\n",
                         s.ownerPid, sockIdx, g_lapicTickCount, flags,
                         s.tcpSndNxt, s.tcpRcvNxt, dataLen, why, s.txPktCount);
        }
    }
}

// Append raw bytes to socket RX ring buffer.
// Caller MUST hold s.lock.
static void TcpEnqueueData(Socket& s, const void* data, uint32_t len)
{
    uint32_t avail = Socket::RX_BUF_SIZE - s.rxCount;
    if (len > avail) len = avail;

    const uint8_t* d = static_cast<const uint8_t*>(data);
    for (uint32_t i = 0; i < len; i++) {
        s.rxBuf[s.rxHead] = d[i];
        s.rxHead = (s.rxHead + 1) % Socket::RX_BUF_SIZE;
    }
    __asm__ volatile("mfence" ::: "memory");
    s.rxCount += len;
    // Caller (HandleTcp) wakes pollWaiter after releasing the lock.
}

void HandleTcp(const Ipv4Header* ip, const void* payload, uint32_t len)
{
    if (len < sizeof(TcpHeader)) return;

    auto* tcp = static_cast<const TcpHeader*>(payload);
    uint32_t dataOff = ((tcp->dataOff >> 4) & 0xF) * 4;
    if (dataOff < 20 || dataOff > len) return;

    uint16_t srcPort = tcp->srcPort;
    uint16_t dstPort = tcp->dstPort;
    uint32_t seq     = ntohl(tcp->seqNum);
    uint32_t ack     = ntohl(tcp->ackNum);
    uint8_t  flags   = tcp->flags;
    uint16_t window  = ntohs(tcp->window);
    const uint8_t* tcpData = static_cast<const uint8_t*>(payload) + dataOff;
    uint32_t dataLen = len - dataOff;

    // Find matching connected socket (exact 4-tuple match)
    for (uint32_t i = 0; i < MAX_SOCKETS; i++) {
        if (!g_sockUsed[i]) continue;
        Socket& s = g_sockets[i];
        if (s.type != SOCK_STREAM) continue;
        if (s.localPort != dstPort) continue;
        if (s.remotePort != srcPort) continue;
        if (s.remoteIp != ip->srcIp) continue;

        uint64_t irqFlags = SpinLockAcquire(&s.lock);

        // Update peer's advertised window
        s.tcpSndWnd = window;

        // Log TCP segments — per-socket counter so each new connection gets
        // fresh verbose logging (the old global static went silent after 3
        // packets from the first connection).
        s.rxPktCount++;
        if (s.rxPktCount <= 50 || (s.rxPktCount % 100) == 0)
        {
            extern volatile uint64_t g_lapicTickCount;
            int sockIdx = static_cast<int>(&s - g_sockets);
            SerialPrintf("tcp: RX pid=%u sock=%d t=%lums flags=0x%02x seq=%u ack=%u datalen=%u rcvNxt=%u [pkt#%u]\n",
                         s.ownerPid, sockIdx, g_lapicTickCount, flags, seq, ack, dataLen, s.tcpRcvNxt, s.rxPktCount);
        }

        // Clamp incoming data to our actual free space so tcpRcvNxt stays
        // in sync with what we actually accept.  Without this, a full RX
        // buffer causes TcpEnqueueData to silently drop bytes while we've
        // already ACKed them, permanently desynchronising sequence numbers.
        uint32_t freeSpace = Socket::RX_BUF_SIZE - s.rxCount;
        bool bufferWasFull = (dataLen > 0 && freeSpace == 0);
        if (dataLen > freeSpace) dataLen = freeSpace;

        // Delegate to testable state machine
        uint32_t prevRxCount = s.rxCount;
        TcpAction act = TcpProcessSegment(s, seq, ack, flags, tcpData, dataLen);

        if (act.enqueueData && act.dataPtr && act.dataLen > 0)
            TcpEnqueueData(s, act.dataPtr, act.dataLen);

        // CRITICAL: compute shouldWake and capture pollWaiter while still
        // holding the lock.  If we release first, SockRecv can drain the
        // rx buffer on another core before we read s.rxCount here — the
        // delta (s.rxCount > prevRxCount) becomes false and the wakeup is
        // suppressed.  The sleeping SockRecv then waits the full 10-second
        // SchedulerBlock timeout instead of being woken immediately (the
        // 60-second stall = 6 missed wakeups × 10-second timeout).
        bool shouldWake = (s.rxCount > prevRxCount) || s.tcpRstRecv
                          || s.tcpFinRecv || act.justConnected;
        Process* waiter = nullptr;
        if (s.pollWaiter && shouldWake)
        {
            waiter = s.pollWaiter;
            s.pollWaiter = nullptr;
        }

        SpinLockRelease(&s.lock, irqFlags);

        // Send ACK outside the lock — TcpSendSegment is not re-entrant under
        // s.lock and may acquire other locks (e.g. TX queue).
        if (act.sendAck || bufferWasFull)
            TcpSendSegment(s, TCP_ACK, nullptr, 0,
                           bufferWasFull ? "rx-bufferfull" : "rx-action");

        // Wake the waiter outside the lock — SchedulerUnblock may need the
        // scheduler lock which must not be acquired while holding s.lock.
        if (waiter)
        {
            __atomic_store_n(&waiter->pendingWakeup, 1, __ATOMIC_RELEASE);
            SchedulerUnblock(waiter);
        }

        return; // handled
    }

    // No connected socket — check for listening socket (incoming SYN)
    if (flags & TCP_SYN)
    {
        for (uint32_t i = 0; i < MAX_SOCKETS; i++)
        {
            if (!g_sockUsed[i]) continue;
            Socket& s = g_sockets[i];
            if (s.type != SOCK_STREAM) continue;
            if (!s.listening) continue;
            if (s.localPort != dstPort) continue;

            // Check backlog
            if (s.acceptQueueCount >= s.listenBacklog)
            {
                SerialPrintf("net: TCP SYN dropped — accept queue full (port %u)\n",
                             ntohs(dstPort));
                break; // send RST below
            }

            // Create a new socket for this connection
            int childIdx = SockCreate(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (childIdx < 0)
            {
                SerialPrintf("net: TCP SYN dropped — no free sockets\n");
                break;
            }

            Socket& child = g_sockets[childIdx];
            child.localIp    = g_netIf->ipAddr;
            child.localPort  = dstPort;
            child.remoteIp   = ip->srcIp;
            child.remotePort = srcPort;
            child.bound      = true;
            child.tcpState   = TcpState::SynRecv;
            child.tcpRcvNxt  = seq + 1;
            child.listenSockIdx = static_cast<int>(i);

            // Generate ISS for the server side
            child.tcpSndIss = static_cast<uint32_t>(g_ipId) * 64000 +
                              ntohs(srcPort) + ntohs(dstPort);
            child.tcpSndNxt = child.tcpSndIss + 1; // after SYN-ACK
            child.tcpSndUna = child.tcpSndIss;

            // Add to listen socket's accept queue
            s.acceptQueue[s.acceptQueueCount++] = childIdx;

            // Send SYN-ACK
            // Temporarily set tcpSndNxt to ISS for the SYN-ACK segment
            child.tcpSndNxt = child.tcpSndIss;
            TcpSendSegment(child, TCP_SYN | TCP_ACK, nullptr, 0, "synack");
            child.tcpSndNxt = child.tcpSndIss + 1; // SYN consumes one seq

            SerialPrintf("net: TCP SYN-ACK sent for port %u -> %u.%u.%u.%u:%u (childIdx=%d)\n",
                         ntohs(dstPort),
                         ip->srcIp & 0xFF, (ip->srcIp >> 8) & 0xFF,
                         (ip->srcIp >> 16) & 0xFF, (ip->srcIp >> 24) & 0xFF,
                         ntohs(srcPort), childIdx);
            return; // handled
        }
    }

    // No matching socket — send RST
    if (!(flags & TCP_RST) && g_netIf) {
        SerialPrintf("tcp: no socket for %u.%u.%u.%u:%u→%u (flags=0x%02x) — sending RST\n",
                     (ntohl(ip->srcIp) >> 24) & 0xFF, (ntohl(ip->srcIp) >> 16) & 0xFF,
                     (ntohl(ip->srcIp) >> 8) & 0xFF, ntohl(ip->srcIp) & 0xFF,
                     ntohs(srcPort), ntohs(dstPort), flags);
        uint8_t rstBuf[sizeof(TcpHeader)];
        NetMemset(rstBuf, 0, sizeof(TcpHeader));
        auto* rst = reinterpret_cast<TcpHeader*>(rstBuf);
        rst->srcPort = dstPort;
        rst->dstPort = srcPort;
        if (flags & TCP_ACK) {
            rst->seqNum = tcp->ackNum;
        } else {
            rst->seqNum = 0;
            rst->ackNum = htonl(seq + dataLen + ((flags & TCP_SYN) ? 1 : 0) +
                                ((flags & TCP_FIN) ? 1 : 0));
            rst->flags = TCP_ACK;
        }
        rst->flags |= TCP_RST;
        rst->dataOff = (sizeof(TcpHeader) / 4) << 4;
        rst->window = 0;
        rst->checksum = 0;
        rst->checksum = TcpChecksum(g_netIf->ipAddr, ip->srcIp,
                                     rstBuf, sizeof(TcpHeader));
        NetSendIpv4(ip->srcIp, IP_PROTO_TCP, rstBuf, sizeof(TcpHeader));
    }
}

int SockConnect(int sockIdx, const SockAddrIn* addr)
{
    if (sockIdx < 0 || sockIdx >= static_cast<int>(MAX_SOCKETS)) return -1;
    if (!g_sockUsed[sockIdx]) return -1;

    Socket& s = g_sockets[sockIdx];

    // UDP connect: just set default destination
    if (s.type == SOCK_DGRAM) {
        s.remoteIp   = addr->sin_addr;
        s.remotePort = addr->sin_port;
        s.connected  = true;
        // Auto-bind if not yet bound
        if (!s.bound) {
            uint32_t port = __atomic_fetch_add(&g_tcpEphemeralPort, 1, __ATOMIC_RELAXED);
            if (port >= 65535) __atomic_store_n(&g_tcpEphemeralPort, 49200, __ATOMIC_RELAXED);
            s.localPort = htons(static_cast<uint16_t>(port));
            s.localIp = g_netIf ? g_netIf->ipAddr : 0;
            s.bound = true;
        }
        return 0;
    }

    if (s.type != SOCK_STREAM) return -95; // -EOPNOTSUPP

    s.remoteIp   = addr->sin_addr;
    s.remotePort = addr->sin_port; // already big-endian

    // Auto-bind local port
    if (!s.bound) {
        uint32_t port = __atomic_fetch_add(&g_tcpEphemeralPort, 1, __ATOMIC_RELAXED);
        if (port >= 65535) __atomic_store_n(&g_tcpEphemeralPort, 49200, __ATOMIC_RELAXED);
        s.localPort = htons(static_cast<uint16_t>(port));
        s.localIp = g_netIf ? g_netIf->ipAddr : 0;
        s.bound = true;
    }

    // Initialize TCP state
    // Use a simple ISS from the IP ID counter + port for uniqueness
    s.tcpSndIss = static_cast<uint32_t>(g_ipId) * 12345 +
                  ntohs(s.localPort) * 67890;
    s.tcpSndNxt = s.tcpSndIss;
    s.tcpSndUna = s.tcpSndIss;
    s.tcpRcvNxt = 0;
    s.tcpFinRecv = false;
    s.tcpRstRecv = false;
    s.tcpSndWnd  = 65535; // assume full window until server tells us otherwise
    s.tcpState = TcpState::SynSent;

    // Send SYN
    TcpSendSegment(s, TCP_SYN, nullptr, 0, "connect");
    s.tcpSndNxt++; // SYN consumes one sequence number

    SerialPrintf("tcp: SYN queued to %u.%u.%u.%u:%u\n",
                 (ntohl(s.remoteIp) >> 24) & 0xFF,
                 (ntohl(s.remoteIp) >> 16) & 0xFF,
                 (ntohl(s.remoteIp) >> 8) & 0xFF,
                 ntohl(s.remoteIp) & 0xFF,
                 ntohs(s.remotePort));

    extern volatile uint64_t g_lapicTickCount;
    Process* self = SchedulerCurrentProcess();

    // Block until SYN-ACK arrives or timeout (5 second per attempt, 2 attempts)
    for (int attempt = 0; attempt < 2; attempt++) {
        if (attempt == 1) {
            // Retry SYN
            s.tcpSndNxt = s.tcpSndIss;
            TcpSendSegment(s, TCP_SYN, nullptr, 0, "connect-retry");
            s.tcpSndNxt++;
        }

        uint64_t deadline = g_lapicTickCount + 5000;
        while (s.tcpState == TcpState::SynSent && g_lapicTickCount < deadline) {
            uint64_t irqFlags = SpinLockAcquire(&s.lock);
            s.pollWaiter = self;
            self->wakeupTick = g_lapicTickCount + 5000;
            SpinLockRelease(&s.lock, irqFlags);
            SchedulerBlock(self);
        }

        if (s.tcpState == TcpState::Established) {
            SerialPrintf("tcp: connected%s!\n", attempt ? " (retry)" : "");
            return 0;
        }
        if (s.tcpState == TcpState::Closed || s.tcpRstRecv) {
            SerialPrintf("tcp: connection refused (RST)\n");
            return -111; // ECONNREFUSED
        }
    }

    SerialPrintf("tcp: connect timeout\n");
    s.tcpState = TcpState::Closed;
    return -110; // ETIMEDOUT
}

int SockSend(int sockIdx, const void* buf, uint32_t len)
{
    if (sockIdx < 0 || sockIdx >= static_cast<int>(MAX_SOCKETS)) return -1;
    if (!g_sockUsed[sockIdx]) return -1;

    Socket& s = g_sockets[sockIdx];
    if (s.type != SOCK_STREAM) return -95;
    if (s.tcpState != TcpState::Established) {
        SerialPrintf("tcp: SockSend refused: state=%d (not Established) fd=%d\n",
                     (int)s.tcpState, sockIdx);
        return -104; // ECONNRESET
    }

    const uint8_t* data = static_cast<const uint8_t*>(buf);
    uint32_t sent = 0;

    // Send in MSS-sized chunks (max ~1460 for Ethernet)
    static constexpr uint32_t MSS = 1460;

    while (sent < len) {
        uint32_t chunk = len - sent;
        if (chunk > MSS) chunk = MSS;

        TcpSendSegment(s, TCP_ACK | TCP_PSH, data + sent, chunk, "data");
        s.tcpSndNxt += chunk;
        sent += chunk;

        // Yield after each segment to let the receive path process ACKs
        // without spinning. The send window check prevents overwhelming the peer.
        SchedulerYield();

        if (s.tcpState != TcpState::Established) return -104;
    }

    return static_cast<int>(sent);
}

int SockRecv(int sockIdx, void* buf, uint32_t len)
{
    if (sockIdx < 0 || sockIdx >= static_cast<int>(MAX_SOCKETS)) return -1;
    if (!g_sockUsed[sockIdx]) return -1;

    Socket& s = g_sockets[sockIdx];
    if (s.type != SOCK_STREAM) return -95;

    // Block until data arrives or the connection closes.
    //
    // IMPORTANT: the "is there data?" check must happen while holding the socket
    // lock.  Without the lock, a packet can arrive between the check and setting
    // pollWaiter — HandleTcp sees pollWaiter==null, skips the wakeup, and we
    // sleep for the full 10-second timeout with data sitting in the buffer.
    //
    // We use a 10-second wakeupTick as a heartbeat so we re-check state
    // periodically even without a data wakeup (e.g., after a missed timer).
    // We do NOT return EAGAIN on a per-heartbeat timeout — blocking sockets
    // must never surface EAGAIN, because callers (OpenSSL/curl) would retry
    // immediately and each retry would stall another 10 seconds, giving the
    // 6 × 10s = 60s failure pattern.  Instead we loop until data arrives,
    // the connection closes, or a 30-second hard timeout fires.
    if (s.rxCount == 0 && !s.tcpRstRecv && !s.tcpFinRecv &&
        s.tcpState != TcpState::Closed) {

        extern volatile uint64_t g_lapicTickCount;
        Process* self = SchedulerCurrentProcess();
        // 3-minute hard deadline — enough for slow TLS handshakes / server
        // cache misses, but still bounded so a truly dead peer releases the
        // caller rather than hanging forever.
        uint64_t hardDeadline = g_lapicTickCount + 180000;

        while (true) {
            uint64_t irqFlags = SpinLockAcquire(&s.lock);
            bool ready = s.rxCount > 0 || s.tcpRstRecv || s.tcpFinRecv
                         || s.tcpState == TcpState::Closed
                         || g_lapicTickCount >= hardDeadline;
            if (ready) {
                SpinLockRelease(&s.lock, irqFlags);
                break;
            }
            // Set pollWaiter while holding the lock so HandleTcp can never
            // enqueue data and miss the wakeup in a race window.
            s.pollWaiter = self;
            self->wakeupTick = g_lapicTickCount + 10000; // heartbeat every 10s
            SpinLockRelease(&s.lock, irqFlags);
            SchedulerBlock(self);
            // Woken by data (HandleTcp), heartbeat tick, or spurious —
            // loop back to re-check under the lock.
        }
    }

    if (s.rxCount == 0) {
        if (s.tcpRstRecv) {
            SerialPrintf("tcp: SockRecv ECONNRESET (RST) fd=%d\n", sockIdx);
            return -104; // ECONNRESET
        }
        if (s.tcpFinRecv || s.tcpState != TcpState::Established) {
            SerialPrintf("tcp: SockRecv EOF fd=%d finRecv=%d state=%d\n",
                         sockIdx, (int)s.tcpFinRecv, (int)s.tcpState);
            return 0; // EOF
        }
        // 30-second hard timeout — connection is established but no data
        // arrived at all.  Server likely gone without sending RST/FIN.
        SerialPrintf("tcp: SockRecv hard timeout fd=%d rxPkts=%u state=%d\n",
                     sockIdx, s.rxPktCount, (int)s.tcpState);
        return -110; // ETIMEDOUT (not EAGAIN — blocking socket must not return EAGAIN)
    }

    // Stream read — no framing, just read bytes from ring buffer
    uint64_t irqFlags = SpinLockAcquire(&s.lock);
    uint32_t copyLen = s.rxCount < len ? s.rxCount : len;
    // Capture free space BEFORE consuming, to decide whether window was constrained.
    uint32_t freeBefore = Socket::RX_BUF_SIZE - s.rxCount;
    uint8_t* dst = static_cast<uint8_t*>(buf);
    for (uint32_t i = 0; i < copyLen; i++) {
        dst[i] = s.rxBuf[s.rxTail];
        s.rxTail = (s.rxTail + 1) % Socket::RX_BUF_SIZE;
    }
    __asm__ volatile("mfence" ::: "memory");
    s.rxCount -= copyLen;
    uint32_t freeAfter = Socket::RX_BUF_SIZE - s.rxCount;
    TcpState stateSnap = s.tcpState;
    SpinLockRelease(&s.lock, irqFlags);

    // Send a window update ACK only when:
    //   1. The receive window was constrained (< 2 MSS of free space), AND
    //   2. It has now opened up enough for the server to send another segment.
    // During normal flow where the buffer is not near-full, no ACK is needed —
    // HandleTcp already ACKed each incoming segment as it arrived.
    // This prevents ACK storms when the application reads in MSS-sized chunks.
    static constexpr uint32_t MSS = 1460;
    bool wasConstrained = freeBefore < 2 * MSS;
    bool nowOpen        = freeAfter  >= MSS;
    if (wasConstrained && nowOpen && stateSnap == TcpState::Established)
        TcpSendSegment(s, TCP_ACK, nullptr, 0, "wnd-update");

    return static_cast<int>(copyLen);
}

bool SockIsStream(int sockIdx)
{
    if (sockIdx < 0 || sockIdx >= static_cast<int>(MAX_SOCKETS)) return false;
    if (!g_sockUsed[sockIdx]) return false;
    return g_sockets[sockIdx].type == SOCK_STREAM;
}

int SockGetType(int sockIdx)
{
    if (sockIdx < 0 || sockIdx >= static_cast<int>(MAX_SOCKETS)) return -1;
    if (!g_sockUsed[sockIdx]) return -1;
    return g_sockets[sockIdx].type;
}

void SockGetLocal(int sockIdx, uint32_t* ip, uint16_t* port)
{
    if (sockIdx < 0 || sockIdx >= static_cast<int>(MAX_SOCKETS)) return;
    if (!g_sockUsed[sockIdx]) return;
    Socket& s = g_sockets[sockIdx];
    if (ip)   *ip   = s.localIp;
    if (port) *port = s.localPort;
}

bool SockPollReady(int sockIdx, bool checkRead, bool checkWrite)
{
    if (sockIdx < 0 || sockIdx >= static_cast<int>(MAX_SOCKETS)) return false;
    if (!g_sockUsed[sockIdx]) return false;

    Socket& s = g_sockets[sockIdx];

    // Poll network to process any pending packets
    if (g_netIf && g_netIf->poll) g_netIf->poll(g_netIf);

    if (checkRead) {
        // Listening socket: readable when accept queue is non-empty
        if (s.listening && s.acceptQueueCount > 0) return true;
        if (s.rxCount > 0) return true;
        if (s.tcpFinRecv) return true; // EOF is readable
        if (s.tcpRstRecv) return true; // RST is readable (returns ECONNRESET)
        if (s.type == SOCK_STREAM &&
            s.tcpState != TcpState::Established &&
            s.tcpState != TcpState::SynSent &&
            s.tcpState != TcpState::Listen) return true; // closed/error
    }
    if (checkWrite) {
        if (s.type == SOCK_DGRAM) return true; // UDP always writable
        if (s.type == SOCK_STREAM && s.tcpState == TcpState::Established)
            return true;
    }
    return false;
}

uint32_t SockRxCount(int sockIdx)
{
    if (sockIdx < 0 || sockIdx >= static_cast<int>(MAX_SOCKETS)) return 0;
    if (!g_sockUsed[sockIdx]) return 0;
    return g_sockets[sockIdx].rxCount;
}

int SockListen(int sockIdx, int backlog)
{
    if (sockIdx < 0 || sockIdx >= static_cast<int>(MAX_SOCKETS)) return -1;
    if (!g_sockUsed[sockIdx]) return -1;

    Socket& s = g_sockets[sockIdx];
    if (s.type != SOCK_STREAM) return -1;
    if (!s.bound) return -1;

    s.listening = true;
    s.listenBacklog = (backlog > Socket::ACCEPT_QUEUE_MAX)
                        ? Socket::ACCEPT_QUEUE_MAX : (backlog < 1 ? 1 : backlog);
    s.acceptQueueCount = 0;
    s.tcpState = TcpState::Listen;
    s.listenSockIdx = -1;

    SerialPrintf("net: listen sockIdx=%d port=%u backlog=%d\n",
                 sockIdx, ntohs(s.localPort), s.listenBacklog);
    return 0;
}

int SockAccept(int sockIdx, SockAddrIn* addr)
{
    if (sockIdx < 0 || sockIdx >= static_cast<int>(MAX_SOCKETS)) return -1;
    if (!g_sockUsed[sockIdx]) return -1;

    Socket& s = g_sockets[sockIdx];
    if (!s.listening) return -1;

    // Drive the network stack a handful of times with proper yields so we
    // don't monopolise the CPU if no client is pending.  Real wakeup on
    // SYN arrival would be ideal, but this is good enough until we wire
    // per-socket wait queues.  The old 50000-iteration busy loop with a
    // 1000-iter volatile spin burned ~30–50 ms per call and showed up as
    // 17% of non-idle kernel time during a nix install.
    for (int attempt = 0; attempt < 16; attempt++)
    {
        if (g_netIf && g_netIf->poll) g_netIf->poll(g_netIf);

        // Scan accept queue for Established connections
        for (int qi = 0; qi < s.acceptQueueCount; qi++)
        {
            int childIdx = s.acceptQueue[qi];
            if (childIdx < 0 || childIdx >= static_cast<int>(MAX_SOCKETS)) continue;
            Socket& child = g_sockets[childIdx];

            if (child.tcpState == TcpState::Established)
            {
                // Remove from accept queue (shift remaining)
                for (int j = qi; j < s.acceptQueueCount - 1; j++)
                    s.acceptQueue[j] = s.acceptQueue[j + 1];
                s.acceptQueueCount--;

                // Fill in peer address if requested
                if (addr)
                {
                    addr->sin_family = AF_INET;
                    addr->sin_port   = child.remotePort;
                    addr->sin_addr   = child.remoteIp;
                }

                SerialPrintf("net: accept sockIdx=%d -> childIdx=%d remote=%u.%u.%u.%u:%u\n",
                             sockIdx, childIdx,
                             child.remoteIp & 0xFF, (child.remoteIp >> 8) & 0xFF,
                             (child.remoteIp >> 16) & 0xFF, (child.remoteIp >> 24) & 0xFF,
                             ntohs(child.remotePort));
                return childIdx;
            }
        }

        // Yield so other work can run while we wait for a SYN to land.
        SchedulerYield();
    }

    return -11; // EAGAIN
}

// ---------------------------------------------------------------------------
// Debug channel — TCP connection to host for realtime debugging
// ---------------------------------------------------------------------------

static int g_debugSockIdx = -1;

// Simple integer-to-string for debug formatting (avoids printf dependency)
static int IntToStr(char* buf, int32_t val)
{
    if (val == 0) { buf[0] = '0'; return 1; }
    bool neg = val < 0;
    if (neg) val = -val;
    char tmp[12];
    int len = 0;
    while (val > 0) { tmp[len++] = '0' + (val % 10); val /= 10; }
    int pos = 0;
    if (neg) buf[pos++] = '-';
    for (int i = len - 1; i >= 0; i--) buf[pos++] = tmp[i];
    return pos;
}

static int UintToStr(char* buf, uint32_t val)
{
    if (val == 0) { buf[0] = '0'; return 1; }
    char tmp[12];
    int len = 0;
    while (val > 0) { tmp[len++] = '0' + (val % 10); val /= 10; }
    int pos = 0;
    for (int i = len - 1; i >= 0; i--) buf[pos++] = tmp[i];
    return pos;
}

static void DebugHandleCommand(const char* cmd, uint32_t len);

// Kernel thread: attempts to connect the debug channel without blocking boot.
// Tries once with a short timeout; if no server is listening, exits quietly.
static void DebugChannelThreadFn(void* /*arg*/)
{
    int idx = SockCreate(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (idx < 0) {
        SerialPrintf("debug: failed to create socket\n");
        return;
    }

    SockAddrIn addr;
    NetMemset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(9999);
    addr.sin_addr = htonl(0x0A000202); // 10.0.2.2

    // Poll with yields instead of busy-spinning.
    // Try for ~2 seconds (200 iterations × ~10ms yield each).
    Socket& s = g_sockets[idx];
    s.remoteIp   = addr.sin_addr;
    s.remotePort = addr.sin_port;

    if (!s.bound) {
        uint32_t port = __atomic_fetch_add(&g_tcpEphemeralPort, 1, __ATOMIC_RELAXED);
        if (port >= 65535) __atomic_store_n(&g_tcpEphemeralPort, 49200, __ATOMIC_RELAXED);
        s.localPort = htons(static_cast<uint16_t>(port));
        s.localIp   = g_netIf ? g_netIf->ipAddr : 0;
        s.bound     = true;
    }

    s.tcpSndIss  = static_cast<uint32_t>(g_ipId) * 12345 + ntohs(s.localPort) * 67890;
    s.tcpSndNxt  = s.tcpSndIss;
    s.tcpSndUna  = s.tcpSndIss;
    s.tcpRcvNxt  = 0;
    s.tcpFinRecv = false;
    s.tcpRstRecv = false;
    s.tcpSndWnd  = 65535;
    s.tcpState   = TcpState::SynSent;

    TcpSendSegment(s, TCP_SYN, nullptr, 0, "connect-poll");
    s.tcpSndNxt++;

    for (int i = 0; i < 200; i++) {
        if (g_netIf && g_netIf->poll) g_netIf->poll(g_netIf);
        if (s.tcpState == TcpState::Established) {
            g_debugSockIdx = idx;
            SerialPrintf("debug: connected to host debug server\n");
            const char* hello = "=== Brook Debug Channel ===\n";
            SockSend(idx, hello, NetStrLen(hello));
            return;
        }
        if (s.tcpState == TcpState::Closed) break;
        SchedulerYield();
    }

    SerialPrintf("debug: no debug server on 10.0.2.2:9999 (skipped)\n");
    s.tcpState = TcpState::Closed;
    SockClose(idx);
}

void DebugChannelInit()
{
    if (!g_netIf) return;
    KernelThreadCreate("debug_ch", DebugChannelThreadFn, nullptr, 3);
}

void DebugChannelSend(const char* msg)
{
    if (g_debugSockIdx < 0) return;
    uint32_t len = 0;
    while (msg[len]) len++;
    SockSend(g_debugSockIdx, msg, len);
}

bool DebugChannelConnected()
{
    return g_debugSockIdx >= 0;
}

void DebugChannelPoll()
{
    if (g_debugSockIdx < 0) return;

    // Check for incoming commands
    if (!SockPollReady(g_debugSockIdx, true, false)) return;

    char buf[256];
    int n = SockRecv(g_debugSockIdx, buf, sizeof(buf) - 1);
    if (n <= 0) {
        if (n == 0) {
            SerialPrintf("debug: channel closed by host\n");
            SockClose(g_debugSockIdx);
            g_debugSockIdx = -1;
        }
        return;
    }
    buf[n] = '\0';

    // Strip trailing newline
    if (n > 0 && buf[n-1] == '\n') buf[--n] = '\0';
    if (n > 0 && buf[n-1] == '\r') buf[--n] = '\0';

    if (n > 0)
        DebugHandleCommand(buf, static_cast<uint32_t>(n));
}

// String prefix check (no libc dependency in kernel net code)
static bool StrStartsWith(const char* str, const char* prefix)
{
    while (*prefix) {
        if (*str++ != *prefix++) return false;
    }
    return true;
}

// Parse unsigned decimal from string, returns number of chars consumed
static int ParseUint(const char* s, uint32_t* out)
{
    uint32_t val = 0;
    int i = 0;
    while (s[i] >= '0' && s[i] <= '9') {
        val = val * 10 + static_cast<uint32_t>(s[i] - '0');
        i++;
    }
    *out = val;
    return i;
}

// Match a log category name to enum (case insensitive)
static bool MatchLogCat(const char* name, LogCat* out)
{
    static const struct { const char* name; LogCat cat; } kMap[] = {
        {"general", LogCat::General}, {"sched", LogCat::Sched},
        {"mem", LogCat::Mem}, {"vfs", LogCat::Vfs}, {"net", LogCat::Net},
        {"input", LogCat::Input}, {"wm", LogCat::Wm}, {"term", LogCat::Term},
        {"syscall", LogCat::Syscall}, {"driver", LogCat::Driver}, {"prof", LogCat::Prof},
    };
    for (auto& m : kMap) {
        const char* a = name;
        const char* b = m.name;
        bool match = true;
        while (*b) {
            char ca = (*a >= 'A' && *a <= 'Z') ? (*a + 32) : *a;
            if (ca != *b) { match = false; break; }
            a++; b++;
        }
        if (match && (*a == '\0' || *a == ' ' || *a == '\n')) {
            *out = m.cat;
            return true;
        }
    }
    return false;
}

static bool MatchLogLevel(const char* name, LogLevel* out)
{
    static const struct { const char* name; LogLevel level; } kMap[] = {
        {"error", LogLevel::Error}, {"warn", LogLevel::Warn},
        {"info", LogLevel::Info}, {"debug", LogLevel::Debug}, {"trace", LogLevel::Trace},
    };
    for (auto& m : kMap) {
        const char* a = name;
        const char* b = m.name;
        bool match = true;
        while (*b) {
            char ca = (*a >= 'A' && *a <= 'Z') ? (*a + 32) : *a;
            if (ca != *b) { match = false; break; }
            a++; b++;
        }
        if (match && (*a == '\0' || *a == ' ' || *a == '\n')) {
            *out = m.level;
            return true;
        }
    }
    return false;
}

static void DebugHandleCommand(const char* cmd, uint32_t len)
{
    (void)len;

    // -----------------------------------------------------------------------
    // help — list all commands
    // -----------------------------------------------------------------------
    if (NetStrEq(cmd, "help")) {
        DebugChannelSend(
            "=== Brook Debug Channel ===\n"
            "Diagnostics:\n"
            "  mouse           - mouse position/buttons\n"
            "  procs           - process list\n"
            "  net             - network interface info\n"
            "  sock            - open sockets\n"
            "  mem             - memory statistics (PMM + heap summary)\n"
            "  mem heap        - detailed heap block breakdown\n"
            "  mem pids        - per-PID PMM page counts (to serial)\n"
            "  uptime          - system uptime + wall clock\n"
            "  cpus            - CPU count + SMP info\n"
            "\n"
            "Log control:\n"
            "  log status      - show current log levels per category\n"
            "  log level <cat> <level>  - set level for category\n"
            "  log level all <level>    - set level for all categories\n"
            "  log stream on|off        - enable/disable log streaming here\n"
            "  (categories: general sched mem vfs net input wm term syscall driver prof)\n"
            "  (levels: error warn info debug trace)\n"
            "\n"
            "Profiler:\n"
            "  prof start [ms]  - start profiler (ms=0 or omitted: indefinite)\n"
            "  prof stop        - stop profiler + flush\n"
            "  prof status      - profiler status\n"
            "\n"
        );
    }

    // -----------------------------------------------------------------------
    // mouse — mouse position and buttons
    // -----------------------------------------------------------------------
    else if (NetStrEq(cmd, "mouse")) {
        int32_t mx = 0, my = 0;
        MouseGetPosition(&mx, &my);
        uint8_t btns = MouseGetButtons();
        bool avail = MouseIsAvailable();

        char buf[128];
        int p = 0;
        const char* h = "mouse: avail=";
        for (int i = 0; h[i]; i++) buf[p++] = h[i];
        buf[p++] = avail ? '1' : '0';
        const char* xh = " x=";
        for (int i = 0; xh[i]; i++) buf[p++] = xh[i];
        p += IntToStr(buf + p, mx);
        const char* yh = " y=";
        for (int i = 0; yh[i]; i++) buf[p++] = yh[i];
        p += IntToStr(buf + p, my);
        const char* bh = " btn=0x";
        for (int i = 0; bh[i]; i++) buf[p++] = bh[i];
        const char* hex = "0123456789abcdef";
        buf[p++] = hex[(btns >> 4) & 0xF];
        buf[p++] = hex[btns & 0xF];
        buf[p++] = '\n';
        SockSend(g_debugSockIdx, buf, static_cast<uint32_t>(p));
    }

    // -----------------------------------------------------------------------
    // procs — process list
    // -----------------------------------------------------------------------
    else if (NetStrEq(cmd, "procs")) {
        extern uint32_t SchedulerSnapshotProcesses(ProcessSnapshot* out, uint32_t maxCount);

        ProcessSnapshot snaps[MAX_PROCESSES];
        uint32_t count = SchedulerSnapshotProcesses(snaps, MAX_PROCESSES);

        char buf[128];
        for (uint32_t i = 0; i < count; i++) {
            int p = 0;
            const char* ph = "  pid=";
            for (int j = 0; ph[j]; j++) buf[p++] = ph[j];
            p += UintToStr(buf + p, snaps[i].pid);
            const char* sh = " state=";
            for (int j = 0; sh[j]; j++) buf[p++] = sh[j];
            p += UintToStr(buf + p, static_cast<uint32_t>(snaps[i].state));
            const char* nh = " cpu=";
            for (int j = 0; nh[j]; j++) buf[p++] = nh[j];
            p += IntToStr(buf + p, snaps[i].runningOnCpu);
            buf[p++] = ' ';
            buf[p++] = '\'';
            for (int j = 0; snaps[i].name[j] && j < 31; j++)
                buf[p++] = snaps[i].name[j];
            buf[p++] = '\'';
            buf[p++] = '\n';
            SockSend(g_debugSockIdx, buf, static_cast<uint32_t>(p));
        }
        const char* done = "---\n";
        SockSend(g_debugSockIdx, done, 4);
    }

    // -----------------------------------------------------------------------
    // net — network interface info
    // -----------------------------------------------------------------------
    else if (NetStrEq(cmd, "net")) {
        if (!g_netIf) {
            DebugChannelSend("net: no interface\n");
            return;
        }
        char buf[256];
        int p = 0;
        const char* h = "net: ip=";
        for (int i = 0; h[i]; i++) buf[p++] = h[i];
        uint32_t ip = ntohl(g_netIf->ipAddr);
        for (int oct = 3; oct >= 0; oct--) {
            p += UintToStr(buf + p, (ip >> (oct * 8)) & 0xFF);
            if (oct > 0) buf[p++] = '.';
        }
        const char* gh = " gw=";
        for (int i = 0; gh[i]; i++) buf[p++] = gh[i];
        uint32_t gw = ntohl(g_netIf->gateway);
        for (int oct = 3; oct >= 0; oct--) {
            p += UintToStr(buf + p, (gw >> (oct * 8)) & 0xFF);
            if (oct > 0) buf[p++] = '.';
        }
        const char* dh = " dns=";
        for (int i = 0; dh[i]; i++) buf[p++] = dh[i];
        uint32_t dns = ntohl(g_netIf->dns);
        for (int oct = 3; oct >= 0; oct--) {
            p += UintToStr(buf + p, (dns >> (oct * 8)) & 0xFF);
            if (oct > 0) buf[p++] = '.';
        }
        buf[p++] = '\n';
        SockSend(g_debugSockIdx, buf, static_cast<uint32_t>(p));
    }

    // -----------------------------------------------------------------------
    // sock — open sockets
    // -----------------------------------------------------------------------
    else if (NetStrEq(cmd, "sock")) {
        for (uint32_t i = 0; i < MAX_SOCKETS; i++) {
            if (!g_sockUsed[i]) continue;
            Socket& s = g_sockets[i];
            char buf[128];
            int p = 0;
            const char* h = "  sock ";
            for (int j = 0; h[j]; j++) buf[p++] = h[j];
            p += UintToStr(buf + p, i);
            const char* th = s.type == SOCK_STREAM ? " TCP" : " UDP";
            for (int j = 0; th[j]; j++) buf[p++] = th[j];
            if (s.type == SOCK_STREAM) {
                const char* sh = " state=";
                for (int j = 0; sh[j]; j++) buf[p++] = sh[j];
                p += UintToStr(buf + p, static_cast<uint32_t>(s.tcpState));
            }
            const char* rh = " rx=";
            for (int j = 0; rh[j]; j++) buf[p++] = rh[j];
            p += UintToStr(buf + p, s.rxCount);
            buf[p++] = '\n';
            SockSend(g_debugSockIdx, buf, static_cast<uint32_t>(p));
        }
        DebugChannelSend("---\n");
    }

    // -----------------------------------------------------------------------
    // mem — memory statistics
    // -----------------------------------------------------------------------
    else if (NetStrEq(cmd, "mem")) {
        uint64_t freePages = PmmGetFreePageCount();
        uint64_t freeMB = (freePages * 4096) / (1024 * 1024);
        uint64_t heapFree = HeapFreeBytes();

        char buf[256];
        int p = 0;
        const char* h1 = "mem: pmm_free_pages=";
        for (int i = 0; h1[i]; i++) buf[p++] = h1[i];
        p += UintToStr(buf + p, static_cast<uint32_t>(freePages));
        const char* h2 = " (";
        for (int i = 0; h2[i]; i++) buf[p++] = h2[i];
        p += UintToStr(buf + p, static_cast<uint32_t>(freeMB));
        const char* h3 = " MB) heap_free=";
        for (int i = 0; h3[i]; i++) buf[p++] = h3[i];
        p += UintToStr(buf + p, static_cast<uint32_t>(heapFree / 1024));
        const char* h4 = " KB\n";
        for (int i = 0; h4[i]; i++) buf[p++] = h4[i];
        SockSend(g_debugSockIdx, buf, static_cast<uint32_t>(p));
    }

    // -----------------------------------------------------------------------
    // mem heap — detailed heap block stats
    // -----------------------------------------------------------------------
    else if (NetStrEq(cmd, "mem heap") || NetStrEq(cmd, "heap")) {
        HeapStats s;
        HeapGetStats(&s);
        char buf[320];
        int p = 0;
        const char* h1 = "heap: size=";
        for (int i = 0; h1[i]; i++) buf[p++] = h1[i];
        p += UintToStr(buf + p, static_cast<uint32_t>(s.heapSizeBytes / 1024));
        const char* h2 = " KB blocks=";
        for (int i = 0; h2[i]; i++) buf[p++] = h2[i];
        p += UintToStr(buf + p, s.totalBlocks);
        const char* h3 = " used=";
        for (int i = 0; h3[i]; i++) buf[p++] = h3[i];
        p += UintToStr(buf + p, s.usedBlocks);
        const char* h4 = " free=";
        for (int i = 0; h4[i]; i++) buf[p++] = h4[i];
        p += UintToStr(buf + p, s.freeBlocks);
        const char* h5 = "\n      used_bytes=";
        for (int i = 0; h5[i]; i++) buf[p++] = h5[i];
        p += UintToStr(buf + p, static_cast<uint32_t>(s.usedBytes));
        const char* h6 = " free_bytes=";
        for (int i = 0; h6[i]; i++) buf[p++] = h6[i];
        p += UintToStr(buf + p, static_cast<uint32_t>(s.freeBytes));
        const char* h7 = " largest_free=";
        for (int i = 0; h7[i]; i++) buf[p++] = h7[i];
        p += UintToStr(buf + p, s.largestFreeBlock);
        const char* h8 = " poison=";
        for (int i = 0; h8[i]; i++) buf[p++] = h8[i];
        const char* pn = s.poisonEnabled ? "on" : "off";
        for (int i = 0; pn[i]; i++) buf[p++] = pn[i];
        buf[p++] = '\n';
        SockSend(g_debugSockIdx, buf, static_cast<uint32_t>(p));
    }

    // -----------------------------------------------------------------------
    // mem pids — per-PID page counts (PMM)
    // -----------------------------------------------------------------------
    else if (NetStrEq(cmd, "mem pids")) {
        PmmDumpPidStats();
        DebugChannelSend("mem pids: written to serial (see kernel log)\n");
    }

    // -----------------------------------------------------------------------
    // uptime — system uptime and wall clock
    // -----------------------------------------------------------------------
    else if (NetStrEq(cmd, "uptime")) {
        uint64_t ticks = ApicTickCount();
        uint32_t sec = static_cast<uint32_t>(ticks / 1000);
        uint32_t min = sec / 60;
        uint32_t hr  = min / 60;

        char buf[128];
        int p = 0;
        const char* h = "uptime: ";
        for (int i = 0; h[i]; i++) buf[p++] = h[i];
        p += UintToStr(buf + p, hr);
        buf[p++] = 'h';
        p += UintToStr(buf + p, min % 60);
        buf[p++] = 'm';
        p += UintToStr(buf + p, sec % 60);
        buf[p++] = 's';

        // Wall clock
        const char* wh = "  wall=";
        for (int i = 0; wh[i]; i++) buf[p++] = wh[i];
        char timeBuf[32];
        RtcFormatTaskbar(timeBuf, RtcNowLocal(), false);
        for (int i = 0; timeBuf[i]; i++) buf[p++] = timeBuf[i];
        buf[p++] = '\n';
        SockSend(g_debugSockIdx, buf, static_cast<uint32_t>(p));
    }

    // -----------------------------------------------------------------------
    // cpus — CPU count
    // -----------------------------------------------------------------------
    else if (NetStrEq(cmd, "cpus")) {
        uint32_t count = SmpGetCpuCount();
        char buf[64];
        int p = 0;
        const char* h = "cpus: ";
        for (int i = 0; h[i]; i++) buf[p++] = h[i];
        p += UintToStr(buf + p, count);
        buf[p++] = '\n';
        SockSend(g_debugSockIdx, buf, static_cast<uint32_t>(p));
    }

    // -----------------------------------------------------------------------
    // log status — show current log levels per category
    // -----------------------------------------------------------------------
    else if (NetStrEq(cmd, "log status") || NetStrEq(cmd, "log")) {
        static const char* const levelNames[] = {"ERROR", "WARN", "INFO", "DEBUG", "TRACE"};

        DebugChannelSend("Log levels:\n");
        for (int i = 0; i < static_cast<int>(LogCat::Count); i++) {
            LogCat cat = static_cast<LogCat>(i);
            LogLevel lvl = KLogGetLevel(cat);
            char buf[64];
            int p = 0;
            const char* pad = "  ";
            for (int j = 0; pad[j]; j++) buf[p++] = pad[j];
            const char* cn = LogCatName(cat);
            while (*cn) buf[p++] = *cn++;
            // Pad to 10 chars
            while (p < 12) buf[p++] = ' ';
            const char* eq = " = ";
            for (int j = 0; eq[j]; j++) buf[p++] = eq[j];
            const char* ln = levelNames[static_cast<int>(lvl)];
            while (*ln) buf[p++] = *ln++;
            buf[p++] = '\n';
            SockSend(g_debugSockIdx, buf, static_cast<uint32_t>(p));
        }
        char buf[64];
        int p = 0;
        const char* rs = "Remote stream: ";
        for (int i = 0; rs[i]; i++) buf[p++] = rs[i];
        const char* onoff = KLogRemoteStreamEnabled() ? "ON" : "OFF";
        while (*onoff) buf[p++] = *onoff++;
        buf[p++] = '\n';
        SockSend(g_debugSockIdx, buf, static_cast<uint32_t>(p));
    }

    // -----------------------------------------------------------------------
    // log level <cat> <level> — set log level for a category
    // log level all <level> — set for all categories
    // -----------------------------------------------------------------------
    else if (StrStartsWith(cmd, "log level ")) {
        const char* args = cmd + 10; // skip "log level "

        if (StrStartsWith(args, "all ")) {
            const char* lvlStr = args + 4;
            LogLevel lvl;
            if (MatchLogLevel(lvlStr, &lvl)) {
                KLogSetGlobalLevel(lvl);
                DebugChannelSend("OK: all categories set\n");
            } else {
                DebugChannelSend("ERR: unknown level (error/warn/info/debug/trace)\n");
            }
        } else {
            // Find space separating cat from level
            const char* sp = args;
            while (*sp && *sp != ' ') sp++;
            if (*sp == ' ') {
                // Null-terminate category name temporarily by copying
                char catBuf[16];
                int ci = 0;
                const char* c = args;
                while (c < sp && ci < 15) catBuf[ci++] = *c++;
                catBuf[ci] = '\0';

                LogCat cat;
                LogLevel lvl;
                if (!MatchLogCat(catBuf, &cat)) {
                    DebugChannelSend("ERR: unknown category\n");
                } else if (!MatchLogLevel(sp + 1, &lvl)) {
                    DebugChannelSend("ERR: unknown level\n");
                } else {
                    KLogSetLevel(cat, lvl);
                    DebugChannelSend("OK\n");
                }
            } else {
                DebugChannelSend("Usage: log level <cat> <level>\n");
            }
        }
    }

    // -----------------------------------------------------------------------
    // log stream on|off — enable/disable remote log streaming
    // -----------------------------------------------------------------------
    else if (StrStartsWith(cmd, "log stream ")) {
        const char* val = cmd + 11;
        if (NetStrEq(val, "on")) {
            KLogSetRemoteStream(true);
            DebugChannelSend("OK: log streaming enabled\n");
        } else if (NetStrEq(val, "off")) {
            KLogSetRemoteStream(false);
            DebugChannelSend("OK: log streaming disabled\n");
        } else {
            DebugChannelSend("Usage: log stream on|off\n");
        }
    }

    // -----------------------------------------------------------------------
    // prof start [ms] — start profiler
    // -----------------------------------------------------------------------
    else if (StrStartsWith(cmd, "prof start")) {
        const char* rest = cmd + 10;
        uint32_t durationMs = 0;
        if (*rest == ' ') {
            ParseUint(rest + 1, &durationMs);
        }
        ProfilerStart(durationMs);
        if (durationMs > 0) {
            char buf[64];
            int p = 0;
            const char* h = "OK: profiler started for ";
            for (int i = 0; h[i]; i++) buf[p++] = h[i];
            p += UintToStr(buf + p, durationMs);
            const char* t = " ms\n";
            for (int i = 0; t[i]; i++) buf[p++] = t[i];
            SockSend(g_debugSockIdx, buf, static_cast<uint32_t>(p));
        } else {
            DebugChannelSend("OK: profiler started (indefinite)\n");
        }
    }

    // -----------------------------------------------------------------------
    // prof stop — stop profiler
    // -----------------------------------------------------------------------
    else if (NetStrEq(cmd, "prof stop")) {
        ProfilerStop();
        DebugChannelSend("OK: profiler stopped\n");
    }

    // -----------------------------------------------------------------------
    // prof status — profiler status
    // -----------------------------------------------------------------------
    else if (NetStrEq(cmd, "prof status")) {
        if (ProfilerIsRunning()) {
            DebugChannelSend("Profiler: RUNNING\n");
        } else {
            DebugChannelSend("Profiler: IDLE\n");
        }
    }

    // -----------------------------------------------------------------------
    // Unknown command
    // -----------------------------------------------------------------------
    else {
        DebugChannelSend("Unknown command. Type 'help' for list.\n");
    }
}

} // namespace brook
