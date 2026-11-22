// net.h — Brook OS network stack.
//
// Minimal network stack: Ethernet, ARP, IPv4, ICMP, UDP, TCP.

#pragma once
#include "netlink_route.h"
#include "socket_identity.h"
#include "spinlock.h"
#include "tcp_flight_control.h"

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

// Addresses here, like sockaddr_in, are in network byte order.
static inline bool Ipv4IsLoopback(uint32_t ip) {
    return (ntohl(ip) >> 24) == 127;
}
static inline uint32_t Ipv4Source(uint32_t bound, uint32_t destination, uint32_t primary) {
    return bound ? bound : Ipv4IsLoopback(destination) ? htonl(0x7f000001) : primary;
}

// Pack an IPv4 address from four octets (host byte order → big-endian).
static inline uint32_t Ipv4Addr(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
    return static_cast<uint32_t>(a) | (static_cast<uint32_t>(b) << 8) |
           (static_cast<uint32_t>(c) << 16) | (static_cast<uint32_t>(d) << 24);
}

// ---------------------------------------------------------------------------
// Network interface (NIC abstraction)
// ---------------------------------------------------------------------------

struct NetDriverSnapshot {
    uint64_t received, transmitted, dropped, txBusy;
    uint64_t rxBackpressureEntries, rxZeroOutstandingEntries, rxDispatchBusy;
    uint32_t rxDepth, rxOutstanding, rxQueued, rxQueueHighWater;
    uint32_t txDepth;
    bool failed;
};

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

    // Optional read-only snapshot; the driver supplies its own synchronization.
    bool (*snapshot)(NetIf* nif, NetDriverSnapshot* out);

    // Per-interface traffic counters (updated by stack, not driver)
    volatile uint64_t rxBytes;
    volatile uint64_t txBytes;
    volatile uint32_t rxPackets;
    volatile uint32_t txPackets;

    // Optional immediate submission: EAGAIN means no frame was published;
    // success means the entire frame was copied into driver-owned storage.
    int (*tryTransmit)(NetIf* nif, const void* frame, uint32_t len);
};

// ---------------------------------------------------------------------------
// Network stack API
// ---------------------------------------------------------------------------

// Initialise network subsystem.
void NetInit();
void NetConfigureBoot();

// Register a network interface. Up to NET_MAX_IFS (4) may be registered.
bool NetRegisterIf(NetIf* nif);

// Called by the NIC driver when a frame arrives.
void NetReceive(NetIf* nif, const void* frame, uint32_t len);

// Get the primary (first-registered) network interface.
NetIf* NetGetIf();

// Total registered interface count and accessor.
uint32_t NetIfCount();
NetIf* NetIfAt(uint32_t idx);
// Coherent, bounded read-only interface view, including the working loopback.
// Names/indices remain stable for the lifetime of the registered interface.
uint32_t NetInterfaceViews(InterfaceView* out,uint32_t capacity,uint64_t scope=0);
SocketIdentity NetAllocateSocketIdentity(uint32_t uid,uint32_t gid);
// Caller protects the handle with an FD reference/lock; identity is immutable.
bool NetlinkGetIdentity(void* handle,SocketIdentity* out);

// Socket table accessors for procfs.
struct SocketSnapshot {
    SocketIdentity identity;
    uint64_t networkScope;
    bool     used;
    int      type;        // SOCK_STREAM, SOCK_DGRAM
    uint32_t localIp;
    uint16_t localPort;   // network byte order
    uint32_t remoteIp;
    uint16_t remotePort;  // network byte order
    TcpState tcpState;
    uint32_t rxCount;
    uint32_t txQueued;    // unacked bytes
};
uint32_t NetMaxSockets();
SocketSnapshot NetSnapshotSocket(uint32_t idx);

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
// Background DHCP for every interface without an address: retries forever
// with exponential backoff. Call periodically from a kernel thread after the
// net_poll thread runs. Returns true when any client state changed.
bool DhcpService();
// Per-interface client state: 0 idle, 1 discovering, 2 requesting, 3 bound.
bool DhcpIfState(uint32_t index, uint8_t* state, uint32_t* attempts);

// Read /boot/BROOK.CFG and, if NET0_MODE=static, configure the interface's
// ipAddr/netmask/gateway/dns from NET0_IP / NET0_NETMASK / NET0_GATEWAY /
// NET0_DNS. Returns true if a static config was applied (caller should skip
// DHCP); false otherwise.
bool NetApplyStaticConfig(NetIf* nif);

// Start the net_poll background thread. Must be called AFTER SchedulerInit().
void NetStartPollThread();
// True only during the single-BSP boot-time network configuration phase.
bool NetEarlyBootPolling();

// Wake the net_poll thread so it drains the RX ring promptly.
// Safe to call from ISR context (delegates to SchedulerUnblock, which is
// ISR-safe). Used by NIC drivers in their RX interrupt handler instead of
// processing packets inline in IRQ context — keeping all packet processing
// (and the socket/ARP SpinLocks it takes) in process context.
void NetWakePollThread();

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
    uint64_t  networkScope; // immutable after socket publication
    int       domain;      // AF_INET
    int       type;        // SOCK_DGRAM, SOCK_STREAM
    int       protocol;
    uint32_t  localIp;
    uint16_t  localPort;   // big-endian
    uint32_t  remoteIp;
    uint16_t  remotePort;  // big-endian
    bool      bound;
    bool      connected;
    bool      nonblock;    // SOCK_NONBLOCK — connect returns EINPROGRESS, recv returns EAGAIN
    // Power-of-two backing storage, with an independently configurable limit.
    static constexpr uint32_t RX_DEFAULT = 128 * 1024;
    static constexpr uint32_t RX_MIN = 2304;
    static constexpr uint32_t RX_MAX = 8 * 1024 * 1024;
    static constexpr uint8_t RX_WINDOW_SCALE = 7;
    uint8_t*  rxBuf;
    uint32_t  rxCapacity;
    uint32_t  rxLimit;
    uint32_t  rxHead;
    uint32_t  rxTail;
    uint32_t  rxCount;     // bytes available
    bool      rxUserLimit;

    // A byte per eight ring positions marks future TCP data relative to RCV.NXT.
    uint64_t*  tcpOooMap;
    uint64_t   tcpOooSince;
    uint32_t   tcpOooBytes; // future bytes held, not yet visible to recv()
    uint32_t   tcpRxRightEdge;
    uint16_t   tcpRxSynGrant;
    bool       tcpRxRightEdgeValid;
    uint64_t   tcpRxSynTick;
    uint32_t   tcpRxRtt;
    uint64_t   tcpRxDrainStart;
    uint32_t   tcpRxDrained;

    // For recvfrom: store source address of last received packet
    uint32_t  lastSrcIp;
    uint16_t  lastSrcPort;

    // TCP state
    TcpState  tcpState;
    uint32_t  tcpSndNxt;   // next sequence number to send
    uint32_t  tcpSndUna;   // oldest unacknowledged
    uint32_t  tcpRcvNxt;   // next expected receive sequence
    uint32_t  tcpSndIss;   // initial send sequence number
    uint32_t  tcpSndWnd;   // peer's advertised receive window (host-endian, scaled)
    TcpFlightControl tcpFlightControl; // persistent bounded experimental congestion state
    uint32_t  tcpSndWl1, tcpSndWl2; // sequence and ACK of last accepted window update
    bool      tcpSndWlValid;
    uint16_t  tcpPeerMss;  // peer's SYN MSS, or the IPv4 default when absent
    uint8_t   tcpSndWndScale; // peer's window scale factor (0-14, from SYN-ACK)
    uint8_t   tcpRcvWndScale; // our window scale factor (advertised in SYN)
    volatile bool tcpFinRecv; // FIN received from peer
    volatile bool tcpRstRecv; // RST received from peer (→ ECONNRESET)
    int       connectError;  // async connect() result for getsockopt(SO_ERROR)
    uint64_t tcpCloseWaitTick; // tick when socket entered CloseWait (for reaper)

    // Delayed ACK — ACK every 2nd data segment or after 40ms timer
    uint32_t tcpUnackedSegs;   // data segments received since last ACK
    uint64_t tcpLastDataTick;  // tick when first unacked data segment arrived
    uint64_t tcpLastRxTick;    // last data tick; initial SYN tick while passive open is incomplete

    // Listen/accept queue (server-side)
    static constexpr int ACCEPT_QUEUE_MAX = 16;
    bool      listening;               // true if listen() was called
    bool      peerLocalIngress;        // passive open from the kernel's local delivery path
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
    uint32_t oooHeldCount; // TCP future segments retained for reassembly
    uint32_t oooDrainCount; // TCP retained segments/partials drained to rxBuf
    uint64_t lastStaleAckTick; // last tick we dup-ACKed a stale/duplicate segment

    // Owner PID (tgid) — set when socket is created via sys_socket. Diagnostic only.
    uint32_t ownerPid;
    SocketIdentity identity; // inode fixed; accepted credentials finalized before FD publication

    // Poll waiter — owns a Process liveness reference until cleared or woken.
    Process* pollWaiter;

    // Spinlock protecting rxBuf/rxHead/rxTail/rxCount and TCP state fields.
    // Acquired by RX (HandleTcp) and userspace syscall paths. Permanent slot
    // synchronization: exclude this field from all incarnation payload resets.
    SpinLock lock;
};

// Create a kernel socket. Returns socket index or negative error.
int  SockCreate(int domain, int type, int protocol,uint64_t networkScope=0);
uint64_t SockNetworkScope(int sockIdx);
// Caller owns a socket reference (including a protected live FD). No locks or callbacks.
bool SockGetIdentity(int sockIdx, SocketIdentity* out);
// Dequeued child only, before publishing any FD or descriptor alias.
void SockSetAcceptedOwner(int sockIdx, uint32_t uid, uint32_t gid);
struct SocketBindActor {
    uint32_t uid = 0;
    bool initialRoot = true; // Default only for direct kernel callers.
};
enum class SocketReuse : uint8_t { Address = 1, Port = 2 };
int SockSetReuse(int sockIdx, SocketReuse option, bool enabled);
int SockGetReuse(int sockIdx, SocketReuse option);
int SockBind(int sockIdx, const SockAddrIn* addr, SocketBindActor actor = {});
int  SockConnect(int sockIdx, const SockAddrIn* addr);
int  SockSendTo(int sockIdx, const void* buf, uint32_t len,
                const SockAddrIn* dest);
int  SockRecvFrom(int sockIdx, void* buf, uint32_t len,
                  SockAddrIn* src);
// Sleepable process context only; must not be called from an IRQ handler.
// SockSend accepts a trusted kernel buffer; SockSendUser stages user memory.
int  SockSend(int sockIdx, const void* buf, uint32_t len);
// Synchronous local-only sends share the caller's absolute monotonic deadline.
int  SockSendUntil(int sockIdx, const void* buf, uint32_t len, uint64_t deadline);
// Returns bytes ACKed by the given absolute monotonic deadline, or a negative
// errno. Remote submission requires a driver's nonblocking transmit callback.
int  SockSendBefore(int sockIdx, const void* buf, uint32_t len, uint64_t deadline);
int  SockSendUser(int sockIdx, uint64_t address, uint32_t len);
struct TcpSendVector { uint64_t base, length; };
// Descriptors are a stable kernel snapshot; payload pointers address user memory.
int  SockSendUserVector(int sockIdx, const TcpSendVector* vectors, unsigned count, uint32_t len);
// Destination must be resident kernel memory. The receive-ring copy holds a
// spinlock; syscall callers stage bytes here and fault-aware copy to userspace
// only after return.
int  SockRecv(int sockIdx, void* buf, uint32_t len, bool dontwait = false);
bool SockIsStream(int sockIdx);  // returns true for SOCK_STREAM (TCP)
int  SockSetReceiveBuffer(int sockIdx, int32_t requested);
int  SockGetReceiveBuffer(int sockIdx);
int  SockGetType(int sockIdx);  // returns SOCK_STREAM(1) or SOCK_DGRAM(2)
int  SockGetProtocol(int sockIdx); // immutable creation protocol, or -1 for invalid slot
void SockGetLocal(int sockIdx, uint32_t* ip, uint16_t* port);
void SockGetRemote(int sockIdx, uint32_t* ip, uint16_t* port);
int  SockListen(int sockIdx, int backlog, SocketBindActor actor = {});
int  SockAccept(int sockIdx, SockAddrIn* addr, bool* peerLocalIngress=nullptr);
bool SockPollReady(int sockIdx, bool checkRead, bool checkWrite);
// Optional async-send worker-pool notification for a poll/epoll invocation.
// A zero token means disabled/full; callers retain their timed fallback.
uint64_t NetTcpPoolWaitRegister(Process* waiter);
void NetTcpPoolWaitUnregister(uint64_t token);
bool SockPollHangup(int sockIdx);
uint32_t SockRxCount(int sockIdx);
void SockSetPollWaiter(int sockIdx, Process* waiter);
// Remove only this registration, even after its numeric FD starts closing.
void SockClearPollWaiter(int sockIdx, Process* waiter);
void SockSetNonblock(int sockIdx, bool nonblock);
bool SockIsNonblock(int sockIdx);
int  SockGetConnectError(int sockIdx); // consume async connect error (SO_ERROR)
struct TcpInfoSource;
// Snapshot TCP_INFO inputs under the socket lock. False unless a live stream socket.
bool SockGetTcpInfo(int sockIdx, TcpInfoSource* out);
void SockClose(int sockIdx);
void SockRef(int sockIdx);    // increment refcount (fork/dup)
void SockUnref(int sockIdx);  // decrement refcount, destroy at 0
void SockUnrefUntil(int sockIdx, uint64_t deadline); // bounded local-only graceful close
void SockUnrefBefore(int sockIdx, uint64_t deadline); // bounded remote-capable graceful close

// Deliver a UDP datagram to the matching socket (called by stack).
void SockDeliverUdp(uint32_t srcIp, uint16_t srcPort,
                    uint32_t dstIp, uint16_t dstPort,
                    const void* data, uint32_t len,uint64_t scope=0);

// Handle an incoming TCP segment (called by IP handler).
void HandleTcp(const Ipv4Header* ip, const void* payload, uint32_t len,uint64_t scope=0,bool localIngress=false);

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

// Route DebugChannelSend responses to a specific socket (for port 1234 server).
void DebugChannelSetReplySock(int sock, uint64_t deadline=0);
void DebugChannelClearReplySock();
#if defined(BROOK_BRIDGE_V3)
using DebugReplyWriter = int (*)(void*, const char*, uint32_t);
bool DebugChannelSetReplyWriter(DebugReplyWriter writer, void* context, bool authenticated);
void DebugChannelClearReplyWriter();
bool DebugChannelReplyAuthenticated();
#endif

// Dispatch a debug command string (used by both outbound and listener channels).
void DebugHandleCommand(const char* cmd, uint32_t len);

// Poll for incoming commands from the debug server.
// Called periodically (e.g., from compositor or heartbeat).
void DebugChannelPoll();

// Is the debug channel connected?
bool DebugChannelConnected();
// Debug TCP listener accounting for `tcp stats` (no log line per client).
void NetCountDebugClient(bool connected);

} // namespace brook
