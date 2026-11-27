#pragma once

// tcp_receive.h — TCP receive reassembly over the socket RX ring.
//
// The ring holds readable bytes in [rxTail, rxHead) and future (out-of-order)
// bytes at their sequence offset from rxHead, marked by one bitmap bit per
// ring byte. rxHead is the ring position of RCV.NXT. Every function here
// requires the caller to hold s.lock. Kept in a header so the host fuzz test
// (src/tests/test_tcp_receive_fuzz) drives exactly the kernel code.

#include "net.h"
#include "tcp.h"
#include <stdint.h>

namespace brook {

struct TcpReceiveDiagnostics {
    uint64_t oooArrivals,oooStored,oooOverlap,oooCapacity,clippedSegments,checksumBad;
};

// Ring, reassembly bitmap and their power-of-two capacity.
struct SocketRxStorage {
    uint8_t* ring;
    uint64_t* bitmap;
    uint32_t capacity;
};

inline bool TcpRxSeqBefore(uint32_t a, uint32_t b)
{
    return static_cast<int32_t>(a - b) < 0;
}

inline uint32_t TcpReceiveFree(const Socket& s)
{
    // The window is a sequence span, not a count of unallocated bytes.
    // Future bytes already occupy positions within that span. Subtracting
    // them again moves its right edge backwards after every reordered packet.
    const uint32_t space = s.rxCount < s.rxLimit ? s.rxLimit - s.rxCount : 0;
    if (!s.tcpRxRightEdgeValid) return space;
    // SO_RCVBUF must not revoke already-advertised sequence space.
    const int32_t granted = static_cast<int32_t>(s.tcpRxRightEdge - s.tcpRcvNxt);
    return granted > static_cast<int32_t>(space) ? static_cast<uint32_t>(granted) : space;
}

// Record the right edge implied by a window about to be advertised.
// window is the scaled-down header value; synWindow the unscaled SYN value.
inline void TcpNoteReceiveGrant(Socket& s, uint16_t window, uint16_t synWindow)
{
    if (s.tcpState == TcpState::SynSent && synWindow > s.tcpRxSynGrant) {
        s.tcpRxSynGrant = synWindow;
    } else if (s.tcpState != TcpState::Closed && s.tcpState != TcpState::SynSent &&
               s.tcpState != TcpState::Listen) {
        const uint32_t grant = s.tcpState == TcpState::SynRecv ? synWindow : uint32_t(window) << s.tcpRcvWndScale;
        const uint32_t edge = s.tcpRcvNxt + grant;
        if (!s.tcpRxRightEdgeValid || static_cast<int32_t>(edge - s.tcpRxRightEdge) > 0) {
            s.tcpRxRightEdge = edge;
            s.tcpRxRightEdgeValid = true;
        }
    }
}

inline void TcpReapOoo(Socket& s,uint64_t now)
{
    static constexpr uint64_t TCP_OOO_REAP_MS = 30000;
    if(!s.tcpOooBytes || !TcpTimerElapsed(now,s.tcpOooSince,TCP_OOO_REAP_MS))return;
    __builtin_memset(s.tcpOooMap,0,s.rxCapacity / 8);
    s.tcpOooBytes=0;
}

inline uint64_t TcpOooMask(uint32_t count)
{
    return count==64 ? ~uint64_t{0} : (uint64_t{1}<<count)-1;
}

// The start and length are always within the same receive window. Traverse
// ring words rather than packet descriptors, including across ring wrap.
template<class Apply> inline void TcpVisitOooWords(const Socket& s,uint32_t position,uint32_t length,Apply&& apply)
{
    uint32_t consumed=0;
    while(consumed<length){
        const uint32_t bit=position%64;
        uint32_t count=64-bit;
        if(count>length-consumed)count=length-consumed;
        apply(position,bit,count,consumed);
        position=(position+count) & (s.rxCapacity - 1);
        consumed+=count;
    }
}

inline void TcpClearOoo(Socket& s,uint32_t position,uint32_t length)
{
    if(!s.tcpOooBytes)return;
    TcpVisitOooWords(s,position,length,[&](uint32_t byte,uint32_t bit,uint32_t count,uint32_t){
        const uint64_t mask=TcpOooMask(count)<<bit;
        auto& word=s.tcpOooMap[byte/64];
        // Clang lowers 64-bit popcount to a runtime helper on builds
        // without POPCNT, so count full words and scan only partial words.
        uint64_t occupied=word&mask;
        if(occupied==mask)s.tcpOooBytes-=count;
        else while(occupied){occupied&=occupied-1;--s.tcpOooBytes;}
        word&=~mask;
    });
}

inline bool TcpStoreOoo(Socket& s,uint32_t seq,const uint8_t* data,uint32_t len,uint64_t now,
                        TcpReceiveDiagnostics& diag)
{
    if(!s.tcpOooMap || !data || !len)return false;
    const uint32_t distance=seq-s.tcpRcvNxt;
    const uint32_t window=TcpReceiveFree(s);
    if(distance>=window){
        __atomic_fetch_add(&diag.oooCapacity,1,__ATOMIC_RELAXED);
        ++s.oooDropCount;
        return false;
    }
    if(len>window-distance){
        __atomic_fetch_add(&diag.oooCapacity,1,__ATOMIC_RELAXED);
        len=window-distance;
    }
    uint32_t added=0;
    TcpVisitOooWords(s,(s.rxHead+distance) & (s.rxCapacity - 1),len,
        [&](uint32_t position,uint32_t bit,uint32_t count,uint32_t consumed){
            const uint64_t mask=TcpOooMask(count)<<bit;
            auto& word=s.tcpOooMap[position/64];
            const uint64_t occupied=word&mask;
            if(!occupied){
                __builtin_memcpy(s.rxBuf+position,data+consumed,count);
                added+=count;
            }else if(occupied!=mask){
                uint64_t missing=(mask&~occupied)>>bit;
                while(missing){
                    const unsigned index=__builtin_ctzll(missing);
                    s.rxBuf[position+index]=data[consumed+index];
                    missing&=missing-1;
                    ++added;
                }
            }
            word|=mask;
        });
    if(added<len)__atomic_fetch_add(&diag.oooOverlap,1,__ATOMIC_RELAXED);
    if(!added)return false;
    if(!s.tcpOooBytes)s.tcpOooSince=now;
    s.tcpOooBytes+=added;
    ++s.oooHeldCount;
    __atomic_fetch_add(&diag.oooStored,1,__ATOMIC_RELAXED);
    return true;
}

inline bool TcpDrainOoo(Socket& s)
{
    // FIN consumes sequence space without moving the ring write index.
    // Discard any pre-FIN speculative data beyond that end of stream.
    if(s.tcpFinRecv || s.tcpRstRecv){
        if(s.tcpOooBytes)__builtin_memset(s.tcpOooMap,0,s.rxCapacity / 8);
        s.tcpOooBytes=0;
        return false;
    }
    uint32_t drained=0;
    while(s.tcpOooBytes){
        const unsigned bit=s.rxHead%64;
        auto& word=s.tcpOooMap[s.rxHead/64];
        const uint64_t pending=word>>bit;
        if(!(pending&1))break;
        const uint64_t missing=(~pending)&TcpOooMask(64-bit);
        const unsigned count=missing ? __builtin_ctzll(missing) : 64-bit;
        word&=~(TcpOooMask(count)<<bit);
        s.rxHead=(s.rxHead+count) & (s.rxCapacity - 1);
        s.tcpOooBytes-=count;
        drained+=count;
    }
    s.rxCount+=drained;
    s.tcpRcvNxt+=drained;
    if(drained)++s.oooDrainCount;
    return drained!=0;
}

// Append raw bytes to socket RX ring buffer.
inline void TcpEnqueueData(Socket& s, const void* data, uint32_t len)
{
    uint32_t avail = s.rxCapacity - s.rxCount;
    if (len > avail) len = avail;

    TcpClearOoo(s,s.rxHead,len);
    const auto* bytes=static_cast<const uint8_t*>(data);
    const uint32_t tail=s.rxCapacity-s.rxHead;
    const uint32_t first=len<tail?len:tail;
    __builtin_memcpy(s.rxBuf+s.rxHead,bytes,first);
    if(len>first)__builtin_memcpy(s.rxBuf,bytes+first,len-first);
    s.rxHead=(s.rxHead+len) & (s.rxCapacity - 1);
    s.rxCount += len;
    // Caller (HandleTcp) wakes pollWaiter after releasing the lock.
}

struct TcpReceiveResult {
    TcpAction act;
    bool bufferWasFull;
};

// Receive-side processing of one segment for a socket matched by 4-tuple:
// clamp new bytes to the receive window, run the state machine, then append
// in-order data, retain future data and drain any now-contiguous run.
// FIN is stripped when data was clamped away so EOF never skips bytes.
inline TcpReceiveResult TcpReceiveSegmentLocked(Socket& s, uint32_t seq, uint32_t ack, uint8_t flags,
                                                const uint8_t* data, uint32_t dataLen, uint64_t now,
                                                TcpReceiveDiagnostics& diag)
{
    // Clamp incoming data so rcvNxt never advances past bytes we can
    // actually store.  Only *new* bytes (past rcvNxt) consume buffer
    // space; stale/retransmitted bytes before rcvNxt are trimmed by
    // TcpProcessSegment and never enqueued.  The old code clamped the
    // total segment length, which could make a partially-stale retransmit
    // look fully stale — dropping its useful tail and stalling the
    // connection on large downloads.
    TcpReapOoo(s, now);
    const uint32_t freeSpace = TcpReceiveFree(s);
    bool bufferWasFull = false;
    if (dataLen > 0) {
        uint32_t staleBytes = 0;
        if (TcpRxSeqBefore(seq, s.tcpRcvNxt)) {
            uint32_t gap = s.tcpRcvNxt - seq;
            staleBytes = gap < dataLen ? gap : dataLen;
        }
        uint32_t newBytes = dataLen - staleBytes;
        bufferWasFull = (newBytes > 0 && freeSpace == 0);
        if (newBytes > freeSpace) {
            __atomic_fetch_add(&diag.clippedSegments,1,__ATOMIC_RELAXED);
            dataLen = staleBytes + freeSpace;
            // If we clamped data away and this segment carries FIN,
            // strip the FIN flag.  FIN occupies the sequence position
            // after the last data byte; accepting it when some data
            // was dropped would advance rcvNxt past bytes never
            // delivered to the application — causing premature EOF
            // and a truncated download.  The server will retransmit
            // the FIN once we ACK the data we did accept and re-open
            // the receive window.
            if (flags & TCP_FIN)
                flags &= ~TCP_FIN;
        }
    }

    TcpAction act = TcpProcessSegment(s, seq, ack, flags, data, dataLen);
    if (act.enqueueData && act.dataPtr && act.dataLen > 0)
        TcpEnqueueData(s, act.dataPtr, act.dataLen);
    if (act.holdOooData && act.oooDataPtr && act.oooDataLen > 0) {
        __atomic_fetch_add(&diag.oooArrivals,1,__ATOMIC_RELAXED);
        TcpStoreOoo(s, act.oooSeq, act.oooDataPtr, act.oooDataLen, now, diag);
    }
    if (TcpDrainOoo(s))
        act.sendAck = true;
    return {act, bufferWasFull};
}

// Copy n bytes out of a socket rx ring starting at tail (wraps once at most).
inline void SockRingRead(const Socket& s, uint32_t tail, void* out, uint32_t n)
{
    const uint32_t first = n < s.rxCapacity - tail ? n : s.rxCapacity - tail;
    if (out) {
        __builtin_memcpy(out, s.rxBuf + tail, first);
        if (n > first) __builtin_memcpy(static_cast<uint8_t*>(out) + first, s.rxBuf, n - first);
    }
}

// Consume up to len readable stream bytes into dst. Returns the count.
inline uint32_t TcpConsumeReceived(Socket& s, void* dst, uint32_t len)
{
    const uint32_t copyLen = s.rxCount < len ? s.rxCount : len;
    SockRingRead(s, s.rxTail, dst, copyLen);
    s.rxTail = (s.rxTail + copyLen) & (s.rxCapacity - 1);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    s.rxCount -= copyLen;
    return copyLen;
}

// Rebase readable and future bytes to the new tail without changing the
// sequence numbers represented by their ring offsets. fresh must be zeroed
// and at least as large as the current capacity. Returns the old storage.
inline SocketRxStorage InstallSocketRxLocked(Socket& s, SocketRxStorage fresh)
{
    const SocketRxStorage old{s.rxBuf, s.tcpOooMap, s.rxCapacity};
    SockRingRead(s, s.rxTail, fresh.ring, s.rxCount);
    if (s.tcpOooBytes) {
        for (uint32_t word = 0; word < old.capacity / 64; ++word) {
            uint64_t bits = old.bitmap[word];
            while (bits) {
                const unsigned bit = __builtin_ctzll(bits);
                const uint64_t clear = ~bits >> bit;
                const uint32_t run = clear ? __builtin_ctzll(clear) : 64 - bit;
                const uint32_t index = word * 64 + bit;
                const uint32_t offset = (index - s.rxTail) & (old.capacity - 1);
                const uint32_t first = run < old.capacity - offset ? run : old.capacity - offset;
                __builtin_memcpy(fresh.ring + offset, old.ring + index, first);
                if (run > first) __builtin_memcpy(fresh.ring, old.ring + index + first, run - first);
                // The old ring boundary is word-aligned, so a shifted word
                // crosses at most one destination-word boundary.
                const uint64_t mask = first == 64 ? ~uint64_t{0} : (uint64_t{1} << first) - 1;
                const unsigned shift = offset % 64;
                fresh.bitmap[offset / 64] |= mask << shift;
                if (first > 64 - shift) fresh.bitmap[offset / 64 + 1] |= mask >> (64 - shift);
                if (run > first) fresh.bitmap[0] |= (uint64_t{1} << (run - first)) - 1;
                const uint64_t consumed = run == 64 ? ~uint64_t{0} : ((uint64_t{1} << run) - 1) << bit;
                bits &= ~consumed;
            }
        }
    }
    s.rxTail = 0;
    s.rxHead = s.rxCount & (fresh.capacity - 1);
    s.rxBuf = fresh.ring;
    s.tcpOooMap = fresh.bitmap;
    s.rxCapacity = fresh.capacity;
    return old;
}

} // namespace brook
