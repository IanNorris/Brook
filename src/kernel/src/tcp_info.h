#pragma once
// tcp_info.h -- Linux getsockopt(IPPROTO_TCP, TCP_INFO) encoding.
//
// The layout is Linux's uapi struct tcp_info (248 bytes through
// tcpi_total_rto_time). Brook fills what its TCP stack tracks and leaves the
// rest zero, which Linux readers treat as "not measured" (Brook keeps no RTT
// estimate, so tcpi_rtt/rttvar/rto are 0). Pure and host-testable.
#include <stddef.h>
#include <stdint.h>

namespace brook {

struct LinuxTcpInfo {
    uint8_t state, caState, retransmits, probes, backoff, options;
    uint8_t wscale;                 // snd_wscale:4 (low), rcv_wscale:4 (high)
    uint8_t flags;                  // delivery_rate_app_limited:1, fastopen_client_fail:2
    uint32_t rto, ato, sndMss, rcvMss;
    uint32_t unacked, sacked, lost, retrans, fackets;
    uint32_t lastDataSent, lastAckSent, lastDataRecv, lastAckRecv;
    uint32_t pmtu, rcvSsthresh, rtt, rttvar, sndSsthresh, sndCwnd, advmss, reordering;
    uint32_t rcvRtt, rcvSpace, totalRetrans;
    uint64_t pacingRate, maxPacingRate, bytesAcked, bytesReceived;
    uint32_t segsOut, segsIn, notsentBytes, minRtt, dataSegsIn, dataSegsOut;
    uint64_t deliveryRate, busyTime, rwndLimited, sndbufLimited;
    uint32_t delivered, deliveredCe;
    uint64_t bytesSent, bytesRetrans;
    uint32_t dsackDups, reordSeen, rcvOoopack, sndWnd, rcvWnd, rehash;
    uint16_t totalRto, totalRtoRecoveries;
    uint32_t totalRtoTime;
};
static_assert(sizeof(LinuxTcpInfo) == 248, "Linux struct tcp_info");
static_assert(offsetof(LinuxTcpInfo, rto) == 8 && offsetof(LinuxTcpInfo, unacked) == 24);
static_assert(offsetof(LinuxTcpInfo, pmtu) == 60 && offsetof(LinuxTcpInfo, sndCwnd) == 80);
static_assert(offsetof(LinuxTcpInfo, totalRetrans) == 100 && offsetof(LinuxTcpInfo, pacingRate) == 104);
static_assert(offsetof(LinuxTcpInfo, segsOut) == 136 && offsetof(LinuxTcpInfo, deliveryRate) == 160);
static_assert(offsetof(LinuxTcpInfo, delivered) == 192 && offsetof(LinuxTcpInfo, bytesSent) == 200);
static_assert(offsetof(LinuxTcpInfo, sndWnd) == 228 && offsetof(LinuxTcpInfo, totalRto) == 240);

// Linux TCP_* state numbers (include/net/tcp_states.h).
enum : uint8_t {
    LinuxTcpEstablished = 1, LinuxTcpSynSent, LinuxTcpSynRecv, LinuxTcpFinWait1, LinuxTcpFinWait2,
    LinuxTcpTimeWait, LinuxTcpClose, LinuxTcpCloseWait, LinuxTcpLastAck, LinuxTcpListen, LinuxTcpClosing,
};

// What Brook's stack knows about one TCP socket, captured under its lock.
struct TcpInfoSource {
    uint8_t brookState;         // TcpState value
    uint8_t sndWscale, rcvWscale;
    uint16_t peerMss;           // 0 before the SYN exchange
    uint32_t advertisedMss;     // our SYN MSS
    uint32_t mtu;
    uint32_t sndIss, sndUna, sndNxt;
    uint32_t cwndBytes, ssthreshBytes; // 0 when flight control is inactive
    uint32_t peerWindow;        // scaled bytes
    uint32_t receiveFree;       // free receive buffer bytes
    uint32_t segmentsIn, segmentsOut;
    uint32_t delayedAckUs;
    uint64_t nowMs, lastReceiveMs; // lastReceiveMs 0 = never
};

inline uint8_t LinuxTcpStateFor(uint8_t brookState)
{
    // Brook order: Closed, Listen, SynRecv, SynSent, Established, FinWait1,
    // FinWait2, CloseWait, LastAck, TimeWait.
    static constexpr uint8_t map[] = {LinuxTcpClose, LinuxTcpListen, LinuxTcpSynRecv, LinuxTcpSynSent,
        LinuxTcpEstablished, LinuxTcpFinWait1, LinuxTcpFinWait2, LinuxTcpCloseWait, LinuxTcpLastAck,
        LinuxTcpTimeWait};
    return brookState < sizeof(map) ? map[brookState] : LinuxTcpClose;
}

inline LinuxTcpInfo TcpInfoEncode(const TcpInfoSource& s)
{
    LinuxTcpInfo info{};
    info.state = LinuxTcpStateFor(s.brookState);
    const bool synchronized = info.state == LinuxTcpEstablished || info.state == LinuxTcpFinWait1 ||
        info.state == LinuxTcpFinWait2 || info.state == LinuxTcpCloseWait ||
        info.state == LinuxTcpLastAck || info.state == LinuxTcpTimeWait;
    if (s.sndWscale || s.rcvWscale) info.options |= 4; // TCPI_OPT_WSCALE
    info.wscale = uint8_t((s.sndWscale & 15) | ((s.rcvWscale & 15) << 4));
    const uint32_t mss = s.peerMss ? (s.peerMss < s.advertisedMss ? s.peerMss : s.advertisedMss) : s.advertisedMss;
    info.ato = s.delayedAckUs;
    info.sndMss = mss;
    info.rcvMss = s.advertisedMss;
    info.advmss = s.advertisedMss;
    info.pmtu = s.mtu;
    info.reordering = 3; // Linux initial tcp_reordering
    const uint32_t inFlight = synchronized ? s.sndNxt - s.sndUna : 0;
    info.unacked = mss ? (inFlight + mss - 1) / mss : 0;
    info.sndCwnd = mss && s.cwndBytes ? s.cwndBytes / mss : 10; // Linux initial window
    info.sndSsthresh = mss && s.ssthreshBytes ? s.ssthreshBytes / mss : 0x7fffffff; // TCP_INFINITE_SSTHRESH
    info.rcvSsthresh = s.receiveFree;
    info.rcvSpace = s.receiveFree;
    info.rcvWnd = s.receiveFree;
    info.sndWnd = s.peerWindow;
    info.segsIn = s.segmentsIn;
    info.segsOut = s.segmentsOut;
    // Sequence space acknowledged beyond the SYN.
    if (synchronized) info.bytesAcked = uint32_t(s.sndUna - s.sndIss - 1);
    if (s.lastReceiveMs && s.nowMs >= s.lastReceiveMs) {
        const uint64_t since = s.nowMs - s.lastReceiveMs;
        info.lastDataRecv = since > 0xffffffffull ? 0xffffffffu : uint32_t(since);
    }
    return info;
}

} // namespace brook
