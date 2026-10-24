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
