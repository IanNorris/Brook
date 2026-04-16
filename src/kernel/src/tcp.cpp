#include "tcp.h"

namespace brook {

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
            act.sendAck = true;
        } else if (flags & TCP_RST) {
            s.tcpState  = TcpState::Closed;
            s.connected = false;
        }
        break;

    case TcpState::Established:
        if (flags & TCP_RST) {
            s.tcpState   = TcpState::Closed;
            s.connected  = false;
            s.tcpFinRecv = true;
            return act;
        }

        if (flags & TCP_ACK)
            s.tcpSndUna = ack;

        if (dataLen > 0 && seq == s.tcpRcvNxt) {
            act.enqueueData = true;
            act.dataPtr     = data;
            act.dataLen     = dataLen;
            s.tcpRcvNxt += dataLen;
            act.sendAck = true;
        } else if (dataLen > 0) {
            // Out of order — ACK with expected seq
            act.sendAck = true;
        }

        if (flags & TCP_FIN) {
            s.tcpRcvNxt++;
            s.tcpFinRecv = true;
            act.sendAck = true;
            s.tcpState = TcpState::CloseWait;
        }
        break;

    case TcpState::FinWait1:
        if (flags & TCP_ACK) {
            s.tcpSndUna = ack;
            if (flags & TCP_FIN) {
                s.tcpRcvNxt = seq + 1;
                act.sendAck = true;
                s.tcpState = TcpState::TimeWait;
            } else {
                s.tcpState = TcpState::FinWait2;
            }
        }
        if (dataLen > 0 && seq == s.tcpRcvNxt) {
            act.enqueueData = true;
            act.dataPtr     = data;
            act.dataLen     = dataLen;
            s.tcpRcvNxt += dataLen;
        }
        break;

    case TcpState::FinWait2:
        if (dataLen > 0 && seq == s.tcpRcvNxt) {
            act.enqueueData = true;
            act.dataPtr     = data;
            act.dataLen     = dataLen;
            s.tcpRcvNxt += dataLen;
            act.sendAck = true;
        }
        if (flags & TCP_FIN) {
            s.tcpRcvNxt = seq + (dataLen > 0 ? dataLen : 0) + 1;
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
