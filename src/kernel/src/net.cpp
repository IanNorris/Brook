// net.cpp — Brook OS network stack.
//
// Ethernet → ARP / IPv4 → ICMP / UDP
// Single-NIC, single-threaded receive path.

#include "net.h"
#include "serial.h"
#include "kprintf.h"
#include "scheduler.h"
#include "memory/heap.h"

namespace brook {

// ---------------------------------------------------------------------------
// Global state
// ---------------------------------------------------------------------------

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

// Socket table
static constexpr uint32_t MAX_SOCKETS = 64;
static Socket g_sockets[MAX_SOCKETS];
static bool   g_sockUsed[MAX_SOCKETS];

// IPv4 identification counter
static uint16_t g_ipId = 1;

static uint16_t g_tcpEphemeralPort = 49200;

// Forward declarations for TCP
static void TcpSendSegment(Socket& s, uint8_t flags,
                           const void* data, uint32_t dataLen);
static uint16_t TcpChecksum(uint32_t srcIp, uint32_t dstIp,
                            const void* tcpSeg, uint32_t tcpLen);

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

static void ArpCacheInsert(uint32_t ip, const MacAddr& mac)
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
    if (!g_netIf) return;

    uint8_t frame[42]; // 14 eth + 28 arp
    NetMemset(frame, 0, sizeof(frame));

    auto* eth = reinterpret_cast<EthHeader*>(frame);
    NetMemset(eth->dst.b, 0xFF, 6); // broadcast
    eth->src = g_netIf->mac;
    eth->etherType = htons(ETH_TYPE_ARP);

    auto* arp = reinterpret_cast<ArpPacket*>(frame + sizeof(EthHeader));
    arp->htype = htons(1);
    arp->ptype = htons(0x0800);
    arp->hlen  = 6;
    arp->plen  = 4;
    arp->oper  = htons(ARP_OP_REQUEST);
    arp->sha   = g_netIf->mac;
    arp->spa   = g_netIf->ipAddr;
    NetMemset(arp->tha.b, 0, 6);
    arp->tpa   = targetIp;

    g_netIf->transmit(g_netIf, frame, 42);
}

static void ArpSendReply(const MacAddr& dstMac, uint32_t dstIp)
{
    if (!g_netIf) return;

    uint8_t frame[42];
    NetMemset(frame, 0, sizeof(frame));

    auto* eth = reinterpret_cast<EthHeader*>(frame);
    eth->dst = dstMac;
    eth->src = g_netIf->mac;
    eth->etherType = htons(ETH_TYPE_ARP);

    auto* arp = reinterpret_cast<ArpPacket*>(frame + sizeof(EthHeader));
    arp->htype = htons(1);
    arp->ptype = htons(0x0800);
    arp->hlen  = 6;
    arp->plen  = 4;
    arp->oper  = htons(ARP_OP_REPLY);
    arp->sha   = g_netIf->mac;
    arp->spa   = g_netIf->ipAddr;
    arp->tha   = dstMac;
    arp->tpa   = dstIp;

    g_netIf->transmit(g_netIf, frame, 42);
}

static void HandleArp(const uint8_t* frame, uint32_t len)
{
    if (len < sizeof(EthHeader) + sizeof(ArpPacket)) return;

    auto* arp = reinterpret_cast<const ArpPacket*>(frame + sizeof(EthHeader));
    uint16_t op = ntohs(arp->oper);

    // Always learn from any ARP packet
    ArpCacheInsert(arp->spa, arp->sha);

    if (op == ARP_OP_REQUEST) {
        // Is this for us?
        if (g_netIf && arp->tpa == g_netIf->ipAddr) {
            SerialPrintf("net: ARP request for our IP, sending reply\n");
            ArpSendReply(arp->sha, arp->spa);
        }
    } else if (op == ARP_OP_REPLY) {
        SerialPrintf("net: ARP reply from %d.%d.%d.%d\n",
                     arp->spa & 0xFF, (arp->spa >> 8) & 0xFF,
                     (arp->spa >> 16) & 0xFF, (arp->spa >> 24) & 0xFF);

        // Wake up anyone waiting for this ARP reply
        if (arp->spa == g_arpReplyIp) {
            g_arpReplyMac = arp->sha;
            __asm__ volatile("mfence" ::: "memory");
            g_arpReplyPending = true;
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

    for (int attempt = 0; attempt < 3; attempt++) {
        ArpSendRequest(ip);

        // Wait up to ~500ms per attempt
        for (int i = 0; i < 500000; i++) {
            if (g_netIf && g_netIf->poll) g_netIf->poll(g_netIf);
            __asm__ volatile("pause");
            if (g_arpReplyPending) {
                *outMac = g_arpReplyMac;
                ArpCacheInsert(ip, *outMac);
                return true;
            }
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

int NetSendIpv4(uint32_t dstIp, uint8_t proto,
                const void* payload, uint32_t payloadLen)
{
    if (!g_netIf || !g_netIf->ipAddr) return -1;

    // Determine next-hop: if same subnet, send direct; else use gateway
    uint32_t nextHop = dstIp;
    if ((dstIp & g_netIf->netmask) != (g_netIf->ipAddr & g_netIf->netmask)) {
        nextHop = g_netIf->gateway;
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
    eth->src = g_netIf->mac;
    eth->etherType = htons(ETH_TYPE_IPV4);

    auto* ip = reinterpret_cast<Ipv4Header*>(frame + sizeof(EthHeader));
    ip->verIhl   = 0x45; // IPv4, 20-byte header
    ip->tos      = 0;
    ip->totalLen = htons(static_cast<uint16_t>(ipLen));
    ip->id       = htons(g_ipId++);
    ip->flagsFrag = htons(0x4000); // Don't Fragment
    ip->ttl      = 64;
    ip->protocol = proto;
    ip->srcIp    = g_netIf->ipAddr;
    ip->dstIp    = dstIp;
    ip->checksum = 0;
    ip->checksum = InetChecksum(ip, sizeof(Ipv4Header));

    NetMemcpy(frame + sizeof(EthHeader) + sizeof(Ipv4Header), payload, payloadLen);

    return g_netIf->transmit(g_netIf, frame, frameLen);
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

    // Check destination: us, broadcast, or multicast
    if (g_netIf && ip->dstIp != g_netIf->ipAddr &&
        ip->dstIp != 0xFFFFFFFF &&
        (ip->dstIp & g_netIf->netmask) != (0xFFFFFFFF & g_netIf->netmask))
        return;

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
        HandleArp(static_cast<const uint8_t*>(frame), len);
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
    SerialPuts("net: initialised\n");
}

void NetRegisterIf(NetIf* nif)
{
    g_netIf = nif;
    SerialPrintf("net: interface registered, MAC=%02x:%02x:%02x:%02x:%02x:%02x\n",
                 nif->mac.b[0], nif->mac.b[1], nif->mac.b[2],
                 nif->mac.b[3], nif->mac.b[4], nif->mac.b[5]);
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

    for (int attempt = 0; attempt < 3; attempt++) {
        SerialPrintf("net: DNS query '%s' (attempt %d)\n", hostname, attempt + 1);
        NetSendUdp(g_netIf->dns, DNS_LOCAL_PORT, DNS_PORT, pkt, pktLen);

        // Wait up to 2 seconds, polling
        for (int i = 0; i < 2000000; i++) {
            if (g_netIf->poll) g_netIf->poll(g_netIf);
            __asm__ volatile("pause");
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

            return static_cast<int>(i);
        }
    }
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
        static uint16_t ephemeralPort = 49152;
        s.localPort = htons(ephemeralPort++);
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

    // TCP: send FIN if connected
    if (s.type == SOCK_STREAM && s.tcpState == TcpState::Established) {
        TcpSendSegment(s, TCP_FIN | TCP_ACK, nullptr, 0);
        s.tcpSndNxt++;
        s.tcpState = TcpState::FinWait1;
        // Brief wait for FIN-ACK (non-blocking, best-effort)
        for (int i = 0; i < 500000; i++) {
            if (g_netIf && g_netIf->poll) g_netIf->poll(g_netIf);
            if (s.tcpState == TcpState::Closed || s.tcpState == TcpState::TimeWait)
                break;
            __asm__ volatile("pause");
        }
    }

    if (s.rxBuf)
        kfree(s.rxBuf);

    NetMemset(&g_sockets[sockIdx], 0, sizeof(Socket));
    g_sockUsed[sockIdx] = false;
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
        uint32_t avail = Socket::RX_BUF_SIZE - s.rxCount;
        if (needed > avail) {
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
        return; // only deliver to first matching socket
    }
}

// ---------------------------------------------------------------------------
// TCP implementation
// ---------------------------------------------------------------------------

static uint16_t TcpChecksum(uint32_t srcIp, uint32_t dstIp,
                            const void* tcpSeg, uint32_t tcpLen)
{
    // Pseudo-header + TCP segment
    uint32_t sum = 0;

    // Pseudo-header: srcIp, dstIp, zero, proto, tcpLen (all in network order)
    sum += (srcIp >> 16) & 0xFFFF;
    sum += srcIp & 0xFFFF;
    sum += (dstIp >> 16) & 0xFFFF;
    sum += dstIp & 0xFFFF;
    sum += htons(IP_PROTO_TCP);
    sum += htons(static_cast<uint16_t>(tcpLen));

    // TCP segment
    const uint16_t* p = static_cast<const uint16_t*>(tcpSeg);
    uint32_t remaining = tcpLen;
    while (remaining > 1) {
        sum += *p++;
        remaining -= 2;
    }
    if (remaining == 1)
        sum += *reinterpret_cast<const uint8_t*>(p);

    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return static_cast<uint16_t>(~sum);
}

static void TcpSendSegment(Socket& s, uint8_t flags,
                           const void* data, uint32_t dataLen)
{
    uint32_t tcpLen = sizeof(TcpHeader) + dataLen;
    uint8_t buf[ETH_MTU];
    NetMemset(buf, 0, tcpLen);

    auto* tcp = reinterpret_cast<TcpHeader*>(buf);
    tcp->srcPort  = s.localPort;
    tcp->dstPort  = s.remotePort;
    tcp->seqNum   = htonl(s.tcpSndNxt);
    tcp->ackNum   = htonl(s.tcpRcvNxt);
    tcp->dataOff  = (sizeof(TcpHeader) / 4) << 4; // 20 bytes = 5 words
    tcp->flags    = flags;
    tcp->window   = htons(Socket::RX_BUF_SIZE < 65535
                          ? static_cast<uint16_t>(Socket::RX_BUF_SIZE)
                          : 65535u);
    tcp->urgentPtr = 0;

    if (data && dataLen > 0)
        NetMemcpy(buf + sizeof(TcpHeader), data, dataLen);

    // Compute TCP checksum
    tcp->checksum = 0;
    tcp->checksum = TcpChecksum(g_netIf->ipAddr, s.remoteIp, buf, tcpLen);

    NetSendIpv4(s.remoteIp, IP_PROTO_TCP, buf, tcpLen);
}

// Append raw bytes to socket RX ring buffer (no framing, stream mode)
static void TcpEnqueueData(Socket& s, const void* data, uint32_t len)
{
    uint32_t avail = Socket::RX_BUF_SIZE - s.rxCount;
    if (len > avail) len = avail; // drop excess

    const uint8_t* d = static_cast<const uint8_t*>(data);
    for (uint32_t i = 0; i < len; i++) {
        s.rxBuf[s.rxHead] = d[i];
        s.rxHead = (s.rxHead + 1) % Socket::RX_BUF_SIZE;
    }
    __asm__ volatile("mfence" ::: "memory");
    s.rxCount += len;
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
    const uint8_t* tcpData = static_cast<const uint8_t*>(payload) + dataOff;
    uint32_t dataLen = len - dataOff;

    // Find matching socket
    for (uint32_t i = 0; i < MAX_SOCKETS; i++) {
        if (!g_sockUsed[i]) continue;
        Socket& s = g_sockets[i];
        if (s.type != SOCK_STREAM) continue;
        if (s.localPort != dstPort) continue;
        if (s.remotePort != srcPort) continue;
        if (s.remoteIp != ip->srcIp) continue;

        switch (s.tcpState) {
        case TcpState::SynSent:
            // Expecting SYN-ACK
            if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK)) {
                s.tcpRcvNxt = seq + 1;
                s.tcpSndUna = ack;
                s.tcpState  = TcpState::Established;
                s.connected = true;
                // Send ACK to complete 3-way handshake
                TcpSendSegment(s, TCP_ACK, nullptr, 0);
            } else if (flags & TCP_RST) {
                s.tcpState = TcpState::Closed;
                s.connected = false;
            }
            break;

        case TcpState::Established:
            // RST: abort
            if (flags & TCP_RST) {
                s.tcpState = TcpState::Closed;
                s.connected = false;
                s.tcpFinRecv = true;
                return;
            }

            // Update send window
            if (flags & TCP_ACK) {
                s.tcpSndUna = ack;
            }

            // Receive data
            if (dataLen > 0 && seq == s.tcpRcvNxt) {
                TcpEnqueueData(s, tcpData, dataLen);
                s.tcpRcvNxt += dataLen;
                // Send ACK
                TcpSendSegment(s, TCP_ACK, nullptr, 0);
            } else if (dataLen > 0) {
                // Out of order — ACK with expected seq to trigger retransmit
                TcpSendSegment(s, TCP_ACK, nullptr, 0);
            }

            // FIN
            if (flags & TCP_FIN) {
                s.tcpRcvNxt++;
                s.tcpFinRecv = true;
                TcpSendSegment(s, TCP_ACK, nullptr, 0);
                s.tcpState = TcpState::CloseWait;
            }
            break;

        case TcpState::FinWait1:
            if (flags & TCP_ACK) {
                s.tcpSndUna = ack;
                if (flags & TCP_FIN) {
                    s.tcpRcvNxt = seq + 1;
                    TcpSendSegment(s, TCP_ACK, nullptr, 0);
                    s.tcpState = TcpState::TimeWait;
                } else {
                    s.tcpState = TcpState::FinWait2;
                }
            }
            // Handle data in FIN_WAIT1
            if (dataLen > 0 && seq == s.tcpRcvNxt) {
                TcpEnqueueData(s, tcpData, dataLen);
                s.tcpRcvNxt += dataLen;
            }
            break;

        case TcpState::FinWait2:
            if (dataLen > 0 && seq == s.tcpRcvNxt) {
                TcpEnqueueData(s, tcpData, dataLen);
                s.tcpRcvNxt += dataLen;
                TcpSendSegment(s, TCP_ACK, nullptr, 0);
            }
            if (flags & TCP_FIN) {
                s.tcpRcvNxt = seq + (dataLen > 0 ? dataLen : 0) + 1;
                TcpSendSegment(s, TCP_ACK, nullptr, 0);
                s.tcpState = TcpState::TimeWait;
            }
            break;

        case TcpState::LastAck:
            if (flags & TCP_ACK) {
                s.tcpState = TcpState::Closed;
            }
            break;

        case TcpState::CloseWait:
            // Waiting for application to close — just ACK data
            if (flags & TCP_ACK) s.tcpSndUna = ack;
            break;

        default:
            break;
        }
        return; // handled
    }

    // No matching socket — send RST
    if (!(flags & TCP_RST) && g_netIf) {
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
            s.localPort = htons(g_tcpEphemeralPort++);
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
        s.localPort = htons(g_tcpEphemeralPort++);
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
    s.tcpState = TcpState::SynSent;

    // Send SYN
    TcpSendSegment(s, TCP_SYN, nullptr, 0);
    s.tcpSndNxt++; // SYN consumes one sequence number

    SerialPrintf("tcp: SYN sent to %u.%u.%u.%u:%u\n",
                 (ntohl(s.remoteIp) >> 24) & 0xFF,
                 (ntohl(s.remoteIp) >> 16) & 0xFF,
                 (ntohl(s.remoteIp) >> 8) & 0xFF,
                 ntohl(s.remoteIp) & 0xFF,
                 ntohs(s.remotePort));

    // Wait for SYN-ACK (polling-based, timeout ~5 seconds)
    for (int i = 0; i < 5000000; i++) {
        if (g_netIf && g_netIf->poll) g_netIf->poll(g_netIf);
        if (s.tcpState == TcpState::Established) {
            SerialPrintf("tcp: connected!\n");
            return 0;
        }
        if (s.tcpState == TcpState::Closed) {
            SerialPrintf("tcp: connection refused (RST)\n");
            return -111; // ECONNREFUSED
        }
        __asm__ volatile("pause");
    }

    // Timeout — retry once
    s.tcpSndNxt = s.tcpSndIss;
    TcpSendSegment(s, TCP_SYN, nullptr, 0);
    s.tcpSndNxt++;
    for (int i = 0; i < 5000000; i++) {
        if (g_netIf && g_netIf->poll) g_netIf->poll(g_netIf);
        if (s.tcpState == TcpState::Established) {
            SerialPrintf("tcp: connected (retry)!\n");
            return 0;
        }
        if (s.tcpState == TcpState::Closed) return -111;
        __asm__ volatile("pause");
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
    if (s.tcpState != TcpState::Established) return -104; // ECONNRESET

    const uint8_t* data = static_cast<const uint8_t*>(buf);
    uint32_t sent = 0;

    // Send in MSS-sized chunks (max ~1460 for Ethernet)
    static constexpr uint32_t MSS = 1460;

    while (sent < len) {
        uint32_t chunk = len - sent;
        if (chunk > MSS) chunk = MSS;

        TcpSendSegment(s, TCP_ACK | TCP_PSH, data + sent, chunk);
        s.tcpSndNxt += chunk;
        sent += chunk;

        // Brief poll to process ACKs and avoid overwhelming the link
        for (int i = 0; i < 50000; i++) {
            if (g_netIf && g_netIf->poll) g_netIf->poll(g_netIf);
            if (s.tcpSndUna >= s.tcpSndNxt) break; // all ACK'd
            __asm__ volatile("pause");
        }

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

    // Wait for data (with timeout, ~10 seconds)
    for (int i = 0; i < 10000000; i++) {
        if (s.rxCount > 0) break;
        if (s.tcpFinRecv) return 0; // EOF
        if (s.tcpState == TcpState::Closed) return 0;
        if (g_netIf && g_netIf->poll) g_netIf->poll(g_netIf);
        __asm__ volatile("pause");
    }

    if (s.rxCount == 0) {
        if (s.tcpFinRecv || s.tcpState != TcpState::Established)
            return 0; // EOF
        return -11; // EAGAIN
    }

    // Stream read — no framing, just read bytes from ring buffer
    uint32_t copyLen = s.rxCount < len ? s.rxCount : len;
    uint8_t* dst = static_cast<uint8_t*>(buf);
    for (uint32_t i = 0; i < copyLen; i++) {
        dst[i] = s.rxBuf[s.rxTail];
        s.rxTail = (s.rxTail + 1) % Socket::RX_BUF_SIZE;
    }
    __asm__ volatile("mfence" ::: "memory");
    s.rxCount -= copyLen;

    return static_cast<int>(copyLen);
}

bool SockPollReady(int sockIdx, bool checkRead, bool checkWrite)
{
    if (sockIdx < 0 || sockIdx >= static_cast<int>(MAX_SOCKETS)) return false;
    if (!g_sockUsed[sockIdx]) return false;

    Socket& s = g_sockets[sockIdx];

    // Poll network to process any pending packets
    if (g_netIf && g_netIf->poll) g_netIf->poll(g_netIf);

    if (checkRead) {
        if (s.rxCount > 0) return true;
        if (s.tcpFinRecv) return true; // EOF is readable
        if (s.type == SOCK_STREAM &&
            s.tcpState != TcpState::Established &&
            s.tcpState != TcpState::SynSent) return true; // closed/error
    }
    if (checkWrite) {
        if (s.type == SOCK_DGRAM) return true; // UDP always writable
        if (s.type == SOCK_STREAM && s.tcpState == TcpState::Established)
            return true;
    }
    return false;
}

} // namespace brook
