#include "tcp.h"
#ifndef BROOK_HOST_TEST
#include "serial.h"
#else
// Host-test stub — tcp.cpp is compiled standalone without the kernel.
#include <cstdio>
#include <cstdarg>
static void SerialPrintf(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
}
#endif

namespace brook {

static bool TcpSeqBefore(uint32_t a, uint32_t b)
{
    return static_cast<int32_t>(a - b) < 0;
}

static bool TcpSeqAfter(uint32_t a, uint32_t b)
{
    return static_cast<int32_t>(a - b) > 0;
}

TcpAction TcpProcessSegment(Socket& s,
                            uint32_t seq, uint32_t ack,
                            uint8_t flags,
                            const uint8_t* data, uint32_t dataLen)
{
    TcpAction act = {};

    switch (s.tcpState) {
    case TcpState::SynRecv:
        // Server side: waiting for ACK to complete 3-way handshake
        if (flags & TCP_ACK) {
            s.tcpSndUna = ack;
            s.tcpState  = TcpState::Established;
            s.connected = true;
            // If data piggy-backed on the ACK, enqueue it
            if (dataLen > 0 && seq == s.tcpRcvNxt) {
                act.enqueueData = true;
                act.dataPtr     = data;
                act.dataLen     = dataLen;
                s.tcpRcvNxt += dataLen;
                act.sendAck = true;
            }
        } else if (flags & TCP_RST) {
            s.tcpState  = TcpState::Closed;
            s.connected = false;
        }
        break;

    case TcpState::SynSent:
        if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK)) {
            s.tcpRcvNxt = seq + 1;
            s.tcpSndUna = ack;
            s.tcpState  = TcpState::Established;
            s.connected = true;
            act.justConnected = true;
            act.sendAck = true;
        } else if (flags & TCP_RST) {
            SerialPrintf("tcp: RST received in SynSent (lport=%u rport=%u) — connection refused\n",
                         ntohs(s.localPort), ntohs(s.remotePort));
            s.tcpState   = TcpState::Closed;
            s.connected  = false;
            s.tcpRstRecv = true;
        }
        break;

    case TcpState::Established:
        if (flags & TCP_RST) {
            SerialPrintf("tcp: RST received in Established (lport=%u rport=%u seq=%u rcvNxt=%u)\n",
                         ntohs(s.localPort), ntohs(s.remotePort), seq, s.tcpRcvNxt);
            s.tcpState   = TcpState::Closed;
            s.connected  = false;
            s.tcpRstRecv = true; // RST ≠ FIN: report as ECONNRESET, not EOF
            return act;
        }

        if (flags & TCP_ACK)
            s.tcpSndUna = ack;

        if (dataLen > 0) {
            const uint8_t* dataPtr = data;
            uint32_t dataSeq = seq;
            uint32_t len = dataLen;
            uint32_t segEnd = dataSeq + len;

            if (TcpSeqBefore(dataSeq, s.tcpRcvNxt)) {
                if (!TcpSeqAfter(segEnd, s.tcpRcvNxt)) {
                    int32_t gap = static_cast<int32_t>(dataSeq - s.tcpRcvNxt);
                    s.oooDropCount++;
                    if (s.oooDropCount <= 5 || (s.oooDropCount % 50) == 0) {
                        SerialPrintf("tcp: OOO STALE pid=%u seq=%u rcvNxt=%u gap=%d len=%u count=%u\n",
                                     s.ownerPid, dataSeq, s.tcpRcvNxt, gap, len, s.oooDropCount);
                    }
                    extern volatile uint64_t g_lapicTickCount;
                    if (g_lapicTickCount - s.lastStaleAckTick > 100) {
                        act.sendAck = true;
                        s.lastStaleAckTick = g_lapicTickCount;
                    }
                    return act;
                }

                uint32_t staleBytes = s.tcpRcvNxt - dataSeq;
                dataPtr += staleBytes;
                len -= staleBytes;
                dataSeq = s.tcpRcvNxt;
            }

            if (dataSeq == s.tcpRcvNxt) {
                act.enqueueData = true;
                act.dataPtr     = dataPtr;
                act.dataLen     = len;
                s.tcpRcvNxt += len;
                act.sendAck = true;
            } else {
                act.holdOooData = true;
                act.oooDataPtr  = dataPtr;
                act.oooDataLen  = len;
                act.oooSeq      = dataSeq;
                act.sendAck = true; // future gap — dup-ACK to drive fast retransmit
            }
        }

        // Only accept FIN if it's at the expected sequence number.
        // An OOO FIN must be ignored — accepting it would prematurely
        // signal EOF to the application while data is still in flight.
        //
        // For a data+FIN segment: after processing the in-order data above,
        // s.tcpRcvNxt == seq+dataLen.  The FIN sits right after the data, so
        // we check seq+dataLen == s.tcpRcvNxt (works for FIN-only too, since
        // dataLen is 0 in that case and rcvNxt was never advanced here).
        if ((flags & TCP_FIN) && (seq + dataLen) == s.tcpRcvNxt) {
            s.tcpRcvNxt++;
            s.tcpFinRecv = true;
            act.sendAck = true;
            s.tcpState = TcpState::CloseWait;
            extern volatile uint64_t g_lapicTickCount;
            s.tcpCloseWaitTick = g_lapicTickCount;
        }
        break;

    case TcpState::FinWait1:
        if (flags & TCP_RST) {
            s.tcpState = TcpState::Closed;
            s.tcpRstRecv = true;
            return act;
        }
        if (dataLen > 0 && seq == s.tcpRcvNxt) {
            act.enqueueData = true;
            act.dataPtr     = data;
            act.dataLen     = dataLen;
            s.tcpRcvNxt += dataLen;
            act.sendAck = true;
        }
        if (flags & TCP_ACK) {
            s.tcpSndUna = ack;
            if ((flags & TCP_FIN) && (seq + dataLen) == s.tcpRcvNxt) {
                s.tcpRcvNxt++;
                act.sendAck = true;
                s.tcpState = TcpState::TimeWait;
            } else {
                s.tcpState = TcpState::FinWait2;
            }
        }
        break;

    case TcpState::FinWait2:
        if (flags & TCP_RST) {
            s.tcpState = TcpState::Closed;
            return act;
        }
        if (dataLen > 0 && seq == s.tcpRcvNxt) {
            act.enqueueData = true;
            act.dataPtr     = data;
            act.dataLen     = dataLen;
            s.tcpRcvNxt += dataLen;
            act.sendAck = true;
        }
        // Same combined data+FIN check as Established
        if ((flags & TCP_FIN) && (seq + dataLen) == s.tcpRcvNxt) {
            s.tcpRcvNxt++;
            act.sendAck = true;
            s.tcpState = TcpState::TimeWait;
        }
        break;

    case TcpState::LastAck:
        if (flags & TCP_ACK)
            s.tcpState = TcpState::Closed;
        break;

    case TcpState::CloseWait:
        if (flags & TCP_ACK)
            s.tcpSndUna = ack;
        break;

    default:
        break;
    }

    return act;
}

uint16_t TcpChecksum(uint32_t srcIp, uint32_t dstIp,
                     const void* tcpSeg, uint32_t tcpLen)
{
    uint32_t sum = 0;

    sum += (srcIp >> 16) & 0xFFFF;
    sum += srcIp & 0xFFFF;
    sum += (dstIp >> 16) & 0xFFFF;
    sum += dstIp & 0xFFFF;
    sum += htons(IP_PROTO_TCP);
    sum += htons(static_cast<uint16_t>(tcpLen));

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

} // namespace brook
