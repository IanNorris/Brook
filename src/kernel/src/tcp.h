#pragma once

// tcp.h — TCP state machine, separated for testability.
//
// The core state machine (TcpProcessSegment) is a pure function that takes
// input data and produces output actions, without directly touching the
// network interface.  This lets us unit-test the state transitions.

#include "net.h"
#include "clock_interval.h"
#include <stdint.h>

namespace brook {

// Passive opens advertise MSS but do not negotiate window scaling until they
// can parse the peer's scaling offer.
inline uint32_t TcpWriteSynOptions(uint8_t flags, uint8_t windowScale, uint8_t* out)
{
    if (!(flags & TCP_SYN)) return 0;
    out[0] = 2; out[1] = 4; out[2] = 1460 >> 8; out[3] = 1460 & 0xff;
    if (flags & TCP_ACK) return 4;
    out[4] = 1; out[5] = 3; out[6] = 3; out[7] = windowScale;
    return 8;
}

// RFC 879: the IPv4 default is 536 when no usable MSS is advertised.
// For duplicate options retain the smallest valid value. Invalid and zero
// values never turn into zero-length sends.
inline uint16_t TcpPeerMss(const uint8_t* options, uint32_t length)
{
    uint16_t mss = 0;
    for (uint32_t i = 0; i < length;) {
        const uint8_t kind = options[i];
        if (kind == 0) break;
        if (kind == 1) { ++i; continue; }
        if (i + 1 >= length) break;
        const uint32_t n = options[i + 1];
        if (n < 2 || n > length - i) break;
        if (kind == 2 && n == 4) {
            const uint16_t value = (uint16_t(options[i + 2]) << 8) | options[i + 3];
            if (value && (!mss || value < mss)) mss = value;
        }
        i += n;
    }
    return mss ? mss : 536;
}

inline uint16_t TcpEgressLimit(uint16_t peerMss, uint16_t ceiling)
{
    uint16_t limit = peerMss ? peerMss : 536;
    if (limit > 1460) limit = 1460;
    if (limit > ceiling) limit = ceiling;
    return limit;
}

// Calibration can move the LAPIC-derived clock backwards. Only a forward
// interval shorter than half the counter range can expire a TCP timer. This
// also preserves ordinary uint64 wraparound for short elapsed intervals.
inline bool TcpTimerElapsed(uint64_t now, uint64_t previous, uint64_t delay)
{
    return ClockIntervalElapsed(now, previous, delay);
}

// A received FIN closes only the peer's send direction. Our send direction
// remains available until we close it or receive a reset.
inline bool TcpCanSendData(TcpState state)
{
    return state == TcpState::Established || state == TcpState::CloseWait;
}

inline bool TcpCanAccept(const Socket& socket)
{
    return !socket.tcpRstRecv &&
           (socket.tcpState == TcpState::Established || socket.tcpState == TcpState::CloseWait);
}

// Action the caller should take after processing a segment.
struct TcpAction {
    bool     sendSynAck;     // retransmit original server SYN-ACK
    bool     sendAck;        // send an ACK segment
    bool     sendRst;        // send a RST segment
    bool     enqueueData;    // data should be enqueued to RX buffer
    bool     holdOooData;    // future data should be retained for reassembly
    bool     justConnected;  // SynSent → Established transition this segment
    const uint8_t* dataPtr;  // pointer to data to enqueue (within input payload)
    uint32_t dataLen;        // bytes to enqueue
    const uint8_t* oooDataPtr; // pointer to future data (within input payload)
    uint32_t oooDataLen;       // bytes of future data to retain
    uint32_t oooSeq;           // sequence number for oooDataPtr
};

// Bound incomplete passive opens without discarding established/half-closed FDs.
inline bool TcpPassiveOpenExpired(const Socket& socket, uint64_t now)
{
    return socket.tcpState == TcpState::SynRecv && TcpTimerElapsed(now, socket.tcpLastRxTick, 30000);
}

// Process an incoming TCP segment for a socket.
// Updates socket state (tcpState, tcpRcvNxt, tcpSndUna, connected, tcpFinRecv).
// Returns a TcpAction describing what the caller should do.
TcpAction TcpProcessSegment(Socket& s,
                            uint32_t seq, uint32_t ack,
                            uint8_t flags,
                            const uint8_t* data, uint32_t dataLen);

// Compute TCP checksum over pseudo-header + segment.
uint16_t TcpChecksum(uint32_t srcIp, uint32_t dstIp,
                     const void* tcpSeg, uint32_t tcpLen);

} // namespace brook
