# Networking Subsystem — Brook OS

## Overview

Brook implements a complete IPv4 networking stack from Ethernet framing through TCP
sockets. The stack is single-NIC focused (supports up to 4 interfaces) and runs a
dedicated poll thread for packet I/O.

**Files:**
| File | Lines | Purpose |
|------|-------|---------|
| `net.h` | ~410 | Protocol headers, Socket struct, public API |
| `net.cpp` | ~3800 | Stack implementation: ARP, IP, UDP, TCP, DHCP, DNS, sockets, debug channel |
| `tcp.h` | ~40 | TcpAction struct and TcpProcessSegment declaration |
| `tcp.cpp` | ~240 | Pure TCP state machine (testable independently) |

## Architecture

```
┌──────────────┐
│  Userspace   │  socket()/connect()/send()/recv()/poll()
│  (glibc)     │
└──────┬───────┘
       │ syscall
┌──────┴───────┐
│  Socket      │  SockCreate/SockBind/SockConnect/SockSend/SockRecv
│  Layer       │  Fixed-size socket table, ref-counted, per-socket SpinLock
└──────┬───────┘
       │
┌──────┴───────┐
│  TCP / UDP   │  HandleTcp() / SockDeliverUdp()
│              │  TCP: 3-way handshake, data transfer, FIN/RST, OOO reassembly
└──────┬───────┘
       │
┌──────┴───────┐
│  IPv4        │  NetSendIpv4() / HandleIpv4()
│              │  Checksum, TTL, protocol dispatch
└──────┬───────┘
       │
┌──────┬───────┐
│  ARP         │  ArpResolve() / ArpCacheInsert()
│  (32 entries)│  Blocking resolve with retry, gateway routing
└──────┬───────┘
       │
┌──────┴───────┐
│  Ethernet    │  NetIf::transmit() / NetReceive()
│              │  14-byte header, MTU 1500
└──────┴───────┘
       │
   virtio-net driver (or other NIC)
```

## Protocol Stack

### Ethernet
- MTU: 1500 bytes, max frame: 1514 bytes (14-byte header + payload)
- `EthHeader`: dst MAC, src MAC, etherType (big-endian)
- Dispatches on etherType: 0x0800 → IPv4, 0x0806 → ARP

### ARP
- `arp_neighbor.h`: 64-entry neighbor table plus a 16-frame queue of packets
  awaiting resolution (4 per neighbor), guarded by `g_arpLock`; host-tested by
  `test_arp_neighbor` and the RX-context cases in `test_socket_lifetime`.
- Insertion always succeeds (LRU eviction; unresolved entries capped at half
  the table). Requests are retried every 500 ms, 3 times, then the entry fails
  and its queued frames are dropped. Entries older than 60 s are refreshed on
  use while the old MAC stays usable.
- Learning follows Linux `arp_process()`: any ARP updates an existing entry for
  its sender (gratuitous/late replies included); entries are created only for
  packets addressed to us. Probes (sender 0.0.0.0), our own address and
  multicast senders are ignored.
- Receive processing (`NetReceive`, tracked per thread) and `net_poll` never
  wait for a reply: `NetSendIpv4` queues the frame and the reply releases it.
  Other threads block in `ArpResolve()` (woken by the reply). Before the
  scheduler, `ArpResolve()` polls the NIC.
- Gateway routing: non-local IPs resolve the gateway's MAC instead.
- Diagnostics: `bridge net arp` (table), `bridge net arp flush`, and
  `bridge net stats` (ARP, UDP receive-full, SYN backlog counters).

### IPv4
- `NetSendIpv4()`: builds IPv4 header, computes checksum, resolves destination MAC via ARP
- Multi-NIF support: `NetIfForDst()` picks the interface whose subnet matches `dstIp`
- Protocol dispatch: ICMP (1) → echo reply, UDP (17) → `SockDeliverUdp()`, TCP (6) → `HandleTcp()`
- ICMP: responds to echo requests with echo replies (ping)

### UDP
- `NetSendUdp()`: wraps payload in UDP header + IPv4
- No checksum verification on receive (checksum field = 0 is valid)
- `SockDeliverUdp()`: finds matching bound socket by port, enqueues to socket rx buffer

### TCP
- **State machine** (`tcp.cpp`): pure function `TcpProcessSegment()` — takes segment,
  updates socket state, returns `TcpAction` (send ACK/RST, enqueue data, OOO hold)
- **States**: Closed, Listen, SynRecv, SynSent, Established, FinWait1, FinWait2,
  CloseWait, LastAck, TimeWait
- **Features**:
  - 3-way handshake (client and server)
  - Data transfer with sequence/ack tracking
  - FIN/RST handling
  - Out-of-order segment reassembly (16 slots × MTU, drained on sequence match)
  - Delayed ACK (~50ms coalescing)
  - Window advertisement (based on rx buffer space)
  - Listen/accept queue (16 backlog slots)
  - Non-blocking connect (EINPROGRESS + getsockopt SO_ERROR)
  - CloseWait reaper thread (closes stale connections after timeout)
- **No** Nagle, congestion control, or retransmission — relies on reliable QEMU/virtio link
- **Per-socket SpinLock** (`sock.lock`): protects rx buffer and TCP state from concurrent
  access by the poll thread (IRQ-like) and syscall paths

### DHCP
- Minimal client: DISCOVER → OFFER → REQUEST → ACK
- Extracts IP, netmask, gateway, DNS from DHCP options
- Single retry with 2-second timeout
- Also supports static configuration via `/boot/BROOK.CFG`
  (`NET0_MODE=static`, `NET0_IP`, `NET0_NETMASK`, `NET0_GATEWAY`, `NET0_DNS`)

### DNS
- A-record only (no AAAA), single upstream server (from DHCP)
- 32-entry cache with hostname string matching
- `DnsResolve()`: blocking, sends UDP query, waits up to 5 seconds
- Encodes query manually (label-length encoding, type A class IN)

## Socket Layer

### Data Structures
- **Fixed socket table**: `g_sockets[MAX_SOCKETS]` (64 sockets)
- **Per-socket rx buffer**: 512 KB ring buffer (`rxBuf`, `rxHead`, `rxTail`, `rxCount`)
- **OOO buffer**: 16 slots × MTU for TCP out-of-order reassembly
- **Reference counting**: `refCount` for fork/dup; `SockRef()`/`SockUnref()`
- **Poll waiter**: single `Process*` per socket, woken on data arrival

### Socket API
| Function | Description |
|----------|-------------|
| `SockCreate(domain, type, protocol)` | Allocate socket, init rx buffer |
| `SockBind(idx, addr)` | Bind to local IP:port |
| `SockConnect(idx, addr)` | TCP: send SYN, block until established. UDP: set remote addr |
| `SockSend(idx, buf, len)` | TCP: segment and send with seq tracking |
| `SockRecv(idx, buf, len)` | Dequeue from rx ring buffer |
| `SockSendTo(idx, buf, len, dest)` | UDP: send datagram to specific addr |
| `SockRecvFrom(idx, buf, len, src)` | UDP: receive with source address |
| `SockListen(idx, backlog)` | Mark as listening, enable accept queue |
| `SockAccept(idx, addr)` | Dequeue from accept queue (blocks if empty) |
| `SockPollReady(idx, read, write)` | Check if socket is readable/writable (for epoll/poll) |
| `SockClose(idx)` | TCP: send FIN, transition to FinWait1/LastAck |

### Diagnostics
Each socket tracks per-lifetime counters:
- `rxPktCount` / `txPktCount`: total segments
- `oooDropCount` / `oooHeldCount` / `oooDrainCount`: OOO reassembly stats
- `lastStaleAckTick`: last stale/duplicate segment ACK

## Network Poll Thread

`NetStartPollThread()` creates a kernel thread that:
1. Polls all registered NICs for received packets (`nif->poll()`)
2. Runs the delayed-ACK timer (~50ms coalescing)
3. Reaps CloseWait sockets after timeout
4. Sleeps briefly between polls (yield-based)

## Debug Channel

A TCP connection to the QEMU host (`10.0.2.2:9999`) for realtime debugging:
- `DebugChannelInit()`: non-blocking connect attempt after DHCP
- `DebugChannelSend(msg)`: send debug strings (safe even if disconnected)
- `DebugChannelPoll()`: check for incoming commands
- Supports remote commands: strace enable/disable, mouse injection, screenshot,
  process list, memory stats, profiler control

## Initialization Sequence

1. `NetInit()` — reset global state
2. NIC driver registers via `NetRegisterIf()`
3. `NetApplyStaticConfig()` — check `/boot/BROOK.CFG` for static IP
4. `DhcpDiscover()` — if no static config, obtain IP via DHCP
5. `NetStartPollThread()` — start background packet processing
6. `DebugChannelInit()` — optional debug connection

## Procfs Integration

The networking subsystem exposes statistics via `/proc/net/`:

| Path | Content |
|------|---------|
| `/proc/net/dev` | Per-interface packet/byte counters (Linux-compatible format) |
| `/proc/net/tcp` | TCP socket table: local/remote addresses, state, queue depths |
| `/proc/net/udp` | UDP socket table: local/remote addresses, queue depths |

Per-interface counters (`rxBytes`, `txBytes`, `rxPackets`, `txPackets`) are
maintained as volatile counters on the `NetIf` struct, incremented in
`NetReceive()` and `NetSendIpv4()`.

Socket snapshots use `NetSnapshotSocket()` to safely read socket state
without exposing the internal `g_sockets[]` array.

## Known Limitations

1. **No retransmission**: TCP relies on reliable in-memory QEMU networking. Real hardware
   with packet loss would need retransmit timers and congestion control.
2. **No fragmentation**: IPv4 packets larger than MTU are not fragmented or reassembled.
3. **Single DNS server**: only uses the first DNS server from DHCP.
4. **No IPv6**: entire stack is IPv4-only.
5. **Fixed socket limit**: 1024 sockets max. Adequate for current workloads.
6. **No SO_REUSEADDR**: bind() rejects ports already in use, even in TIME_WAIT.
7. **Poll thread is cooperative**: no interrupt-driven receive; relies on regular polling.
