// net.h — Brook OS network stack.
//
// Minimal network stack: Ethernet, ARP, IPv4, ICMP, UDP, TCP.

#pragma once
#include "spinlock.h"

#include <stdint.h>

namespace brook {

struct Process;  // forward declaration for poll waiter

// ---------------------------------------------------------------------------
// Ethernet
// ---------------------------------------------------------------------------

static constexpr uint16_t ETH_TYPE_IPV4 = 0x0800;
static constexpr uint16_t ETH_TYPE_ARP  = 0x0806;
static constexpr uint32_t ETH_MTU       = 1500;
static constexpr uint32_t ETH_FRAME_MAX = 1514;   // 14-byte header + 1500 payload

struct __attribute__((packed)) MacAddr {
    uint8_t b[6];
};

struct __attribute__((packed)) EthHeader {
    MacAddr  dst;
    MacAddr  src;
    uint16_t etherType;   // big-endian
};

// ---------------------------------------------------------------------------
// ARP
// ---------------------------------------------------------------------------

static constexpr uint16_t ARP_OP_REQUEST = 1;
static constexpr uint16_t ARP_OP_REPLY   = 2;

struct __attribute__((packed)) ArpPacket {
    uint16_t htype;       // 1 = Ethernet
    uint16_t ptype;       // 0x0800 = IPv4
    uint8_t  hlen;        // 6
    uint8_t  plen;        // 4
    uint16_t oper;
    MacAddr  sha;         // sender hardware address
    uint32_t spa;         // sender protocol address (big-endian)
    MacAddr  tha;         // target hardware address
    uint32_t tpa;         // target protocol address (big-endian)
};

// ---------------------------------------------------------------------------
// IPv4
// ---------------------------------------------------------------------------

static constexpr uint8_t IP_PROTO_ICMP = 1;
static constexpr uint8_t IP_PROTO_TCP  = 6;
static constexpr uint8_t IP_PROTO_UDP  = 17;

struct __attribute__((packed)) Ipv4Header {
    uint8_t  verIhl;      // version (4) | IHL (5 = 20 bytes)
    uint8_t  tos;
    uint16_t totalLen;    // big-endian
    uint16_t id;
    uint16_t flagsFrag;   // flags + fragment offset
    uint8_t  ttl;
    uint8_t  protocol;
    uint16_t checksum;
    uint32_t srcIp;       // big-endian
    uint32_t dstIp;       // big-endian
};

// ---------------------------------------------------------------------------
// ICMP
// ---------------------------------------------------------------------------

static constexpr uint8_t ICMP_ECHO_REPLY   = 0;
static constexpr uint8_t ICMP_ECHO_REQUEST = 8;

struct __attribute__((packed)) IcmpHeader {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint16_t id;
    uint16_t seq;
};

// ---------------------------------------------------------------------------
// UDP
// ---------------------------------------------------------------------------

struct __attribute__((packed)) UdpHeader {
    uint16_t srcPort;     // big-endian
    uint16_t dstPort;     // big-endian
    uint16_t length;      // big-endian (header + payload)
    uint16_t checksum;    // big-endian (0 = no checksum)
};

// ---------------------------------------------------------------------------
// TCP
// ---------------------------------------------------------------------------

struct __attribute__((packed)) TcpHeader {
    uint16_t srcPort;     // big-endian
    uint16_t dstPort;     // big-endian
    uint32_t seqNum;      // big-endian
    uint32_t ackNum;      // big-endian
    uint8_t  dataOff;     // upper 4 bits = header length in 32-bit words
    uint8_t  flags;
    uint16_t window;      // big-endian
    uint16_t checksum;    // big-endian
    uint16_t urgentPtr;   // big-endian
};

static constexpr uint8_t TCP_FIN = 0x01;
static constexpr uint8_t TCP_SYN = 0x02;
static constexpr uint8_t TCP_RST = 0x04;
static constexpr uint8_t TCP_PSH = 0x08;
static constexpr uint8_t TCP_ACK = 0x10;

enum class TcpState : uint8_t {
    Closed,
    Listen,
    SynRecv,
    SynSent,
    Established,
    FinWait1,
    FinWait2,
    CloseWait,
    LastAck,
    TimeWait,
};

// ---------------------------------------------------------------------------
// Byte-order helpers (our kernel is little-endian x86-64)
// ---------------------------------------------------------------------------

static inline uint16_t htons(uint16_t h) { return __builtin_bswap16(h); }
static inline uint16_t ntohs(uint16_t n) { return __builtin_bswap16(n); }
static inline uint32_t htonl(uint32_t h) { return __builtin_bswap32(h); }
static inline uint32_t ntohl(uint32_t n) { return __builtin_bswap32(n); }

// Pack an IPv4 address from four octets (host byte order → big-endian).
static inline uint32_t Ipv4Addr(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
    return static_cast<uint32_t>(a) | (static_cast<uint32_t>(b) << 8) |
           (static_cast<uint32_t>(c) << 16) | (static_cast<uint32_t>(d) << 24);
}

// ---------------------------------------------------------------------------
// Network interface (NIC abstraction)
// ---------------------------------------------------------------------------

struct NetIf {
    MacAddr  mac;
    uint32_t ipAddr;      // our IP (big-endian), 0 = unconfigured
    uint32_t netmask;     // big-endian
    uint32_t gateway;     // big-endian
    uint32_t dns;         // big-endian

    // Transmit a raw Ethernet frame. Returns 0 on success.
    int (*transmit)(NetIf* nif, const void* frame, uint32_t len);

    // Poll for received packets. Calls NetReceive for each packet found.
    void (*poll)(NetIf* nif);

    void* driverPriv;     // driver-specific state
};

// ---------------------------------------------------------------------------
// Network stack API
// ---------------------------------------------------------------------------

// Initialise network subsystem.
void NetInit();

// Register a network interface. Up to NET_MAX_IFS (4) may be registered.
void NetRegisterIf(NetIf* nif);

// Called by the NIC driver when a frame arrives.
void NetReceive(NetIf* nif, const void* frame, uint32_t len);

// Get the primary (first-registered) network interface.
NetIf* NetGetIf();

// Total registered interface count and accessor.
uint32_t NetIfCount();
NetIf* NetIfAt(uint32_t idx);

// Pick the interface that should source packets destined for dstIp. Chooses
// a configured interface whose subnet matches dstIp; falls back to the
// primary (default-route) interface when no subnet matches.
NetIf* NetIfForDst(uint32_t dstIp);

static constexpr uint32_t NET_MAX_IFS = 4;

// Send an IPv4 packet. Handles Ethernet framing and ARP resolution.
// Returns 0 on success, negative on error.
int NetSendIpv4(uint32_t dstIp, uint8_t proto,
                const void* payload, uint32_t payloadLen);

// Send a UDP datagram.
int NetSendUdp(uint32_t dstIp, uint16_t srcPort, uint16_t dstPort,
               const void* data, uint32_t dataLen);

// ---------------------------------------------------------------------------
// ARP
// ---------------------------------------------------------------------------

// Resolve an IP address to a MAC address. May block briefly.
// Returns true if resolved, false on timeout.
bool ArpResolve(uint32_t ip, MacAddr* outMac);

// Insert a known mapping into the ARP cache.
void ArpCacheInsert(uint32_t ip, const MacAddr& mac);

// ---------------------------------------------------------------------------
// DHCP (minimal)
// ---------------------------------------------------------------------------

// Perform DHCP discovery to obtain an IP address.
// Blocks until complete or timeout. Returns true on success.
bool DhcpDiscover(NetIf* nif);

// Start the net_poll background thread. Must be called AFTER SchedulerInit().
void NetStartPollThread();

// ---------------------------------------------------------------------------
// Socket layer (kernel-side)
// ---------------------------------------------------------------------------

static constexpr int AF_INET     = 2;
static constexpr int SOCK_STREAM = 1;
static constexpr int SOCK_DGRAM  = 2;
static constexpr int SOCK_RAW    = 3;

static constexpr int IPPROTO_IP   = 0;
static constexpr int IPPROTO_ICMP = 1;
static constexpr int IPPROTO_TCP  = 6;
static constexpr int IPPROTO_UDP  = 17;

struct __attribute__((packed)) SockAddrIn {
    uint16_t sin_family;   // AF_INET
    uint16_t sin_port;     // big-endian
    uint32_t sin_addr;     // big-endian
    uint8_t  sin_zero[8];
};

struct Socket {
    int       domain;      // AF_INET
    int       type;        // SOCK_DGRAM, SOCK_STREAM
    int       protocol;
    uint32_t  localIp;
    uint16_t  localPort;   // big-endian
    uint32_t  remoteIp;
    uint16_t  remotePort;  // big-endian
    bool      bound;
    bool      connected;
    // Receive buffer (ring buffer for incoming data).
    // 512 KB gives enough headroom for multi-MB NAR downloads: curl does TLS
    // crypto + pipes to xz while the server sends at line rate, so the kernel
    // buffer needs to absorb a burst without triggering zero-window flow-control
    // on every other segment (which would require many round-trips and risks
    // the server's RTO firing and RST'ing the connection).
    static constexpr uint32_t RX_BUF_SIZE = 524288; // 512 KB
    uint8_t*  rxBuf;
    uint32_t  rxHead;
    uint32_t  rxTail;
    uint32_t  rxCount;     // bytes available

    // For recvfrom: store source address of last received packet
    uint32_t  lastSrcIp;
    uint16_t  lastSrcPort;

    // TCP state
    TcpState  tcpState;
    uint32_t  tcpSndNxt;   // next sequence number to send
    uint32_t  tcpSndUna;   // oldest unacknowledged
    uint32_t  tcpRcvNxt;   // next expected receive sequence
    uint32_t  tcpSndIss;   // initial send sequence number
    uint16_t  tcpSndWnd;   // peer's advertised receive window (host-endian)
    volatile bool tcpFinRecv; // FIN received from peer
    volatile bool tcpRstRecv; // RST received from peer (→ ECONNRESET)

    // Listen/accept queue (server-side)
    static constexpr int ACCEPT_QUEUE_MAX = 16;
    bool      listening;               // true if listen() was called
    int       listenBacklog;           // max pending connections
    int       acceptQueue[ACCEPT_QUEUE_MAX]; // socket indices of accepted connections
    volatile int acceptQueueCount;     // number in queue
    int       listenSockIdx;           // for SynRecv sockets: parent listen socket index (-1 if none)

    // Reference counting for fork/dup
    volatile uint32_t refCount;        // number of fds pointing at this socket (1 on create)

    // Diagnostic counters (not reset on reuse — per-socket lifetime)
    uint32_t rxPktCount;   // total TCP segments received on this socket
    uint32_t txPktCount;   // total TCP segments transmitted on this socket
    uint32_t oooDropCount; // TCP segments dropped for being out-of-order
    uint64_t lastStaleAckTick; // last tick we dup-ACKed a stale/duplicate segment

    // Owner PID (tgid) — set when socket is created via sys_socket. Diagnostic only.
    uint32_t ownerPid;

    // Poll waiter — process to wake when data arrives
    Process* pollWaiter;

    // Spinlock protecting rxBuf/rxHead/rxTail/rxCount and TCP state fields.
    // Acquired by both the IRQ handler (HandleTcp) and userspace syscall paths.
    SpinLock lock;
};

// Create a kernel socket. Returns socket index or negative error.
int  SockCreate(int domain, int type, int protocol);
int  SockBind(int sockIdx, const SockAddrIn* addr);
int  SockConnect(int sockIdx, const SockAddrIn* addr);
int  SockSendTo(int sockIdx, const void* buf, uint32_t len,
                const SockAddrIn* dest);
int  SockRecvFrom(int sockIdx, void* buf, uint32_t len,
                  SockAddrIn* src);
int  SockSend(int sockIdx, const void* buf, uint32_t len);
int  SockRecv(int sockIdx, void* buf, uint32_t len);
bool SockIsStream(int sockIdx);  // returns true for SOCK_STREAM (TCP)
int  SockGetType(int sockIdx);  // returns SOCK_STREAM(1) or SOCK_DGRAM(2)
void SockGetLocal(int sockIdx, uint32_t* ip, uint16_t* port);
int  SockListen(int sockIdx, int backlog);
int  SockAccept(int sockIdx, SockAddrIn* addr);
bool SockPollReady(int sockIdx, bool checkRead, bool checkWrite);
uint32_t SockRxCount(int sockIdx);
void SockSetPollWaiter(int sockIdx, Process* waiter);
void SockClose(int sockIdx);
void SockRef(int sockIdx);    // increment refcount (fork/dup)
void SockUnref(int sockIdx);  // decrement refcount, destroy at 0

// Deliver a UDP datagram to the matching socket (called by stack).
void SockDeliverUdp(uint32_t srcIp, uint16_t srcPort,
                    uint32_t dstIp, uint16_t dstPort,
                    const void* data, uint32_t len);

// Handle an incoming TCP segment (called by IP handler).
void HandleTcp(const Ipv4Header* ip, const void* payload, uint32_t len);

// ---------------------------------------------------------------------------
// DNS resolver
// ---------------------------------------------------------------------------

// Resolve a hostname to an IPv4 address (big-endian).
// Returns 0 on failure. Blocks up to ~5 seconds.
uint32_t DnsResolve(const char* hostname);

// Simple DNS cache lookup (no network). Returns 0 if not cached.
uint32_t DnsCacheLookup(const char* hostname);

// ---------------------------------------------------------------------------
// Debug channel — TCP connection to host for realtime debugging
// ---------------------------------------------------------------------------

// Attempt to connect to a debug server on the QEMU host (10.0.2.2:9999).
// Non-fatal if it fails. Call after DHCP completes.
void DebugChannelInit();

// Send a message to the debug channel (if connected). Safe to call anytime.
void DebugChannelSend(const char* msg);
void DebugChannelSendf(const char* fmt, ...);

// Poll for incoming commands from the debug server.
// Called periodically (e.g., from compositor or heartbeat).
void DebugChannelPoll();

// Is the debug channel connected?
bool DebugChannelConnected();

} // namespace brook
