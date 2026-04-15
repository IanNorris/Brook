#pragma once

// tcp.h — TCP state machine, separated for testability.
//
// The core state machine (TcpProcessSegment) is a pure function that takes
// input data and produces output actions, without directly touching the
// network interface.  This lets us unit-test the state transitions.

#include "net.h"
#include <stdint.h>

namespace brook {

// Action the caller should take after processing a segment.
struct TcpAction {
    bool     sendAck;        // send an ACK segment
    bool     sendRst;        // send a RST segment
    bool     enqueueData;    // data should be enqueued to RX buffer
    const uint8_t* dataPtr;  // pointer to data to enqueue (within input payload)
    uint32_t dataLen;        // bytes to enqueue
};

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
