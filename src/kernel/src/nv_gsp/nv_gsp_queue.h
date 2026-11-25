#pragma once
// GSP-RM 570.144 command/message queues (CPU side), over the shared sysmem
// region named in GSP_ARGUMENTS_CACHED.messageQueueInitArguments.
//
// Layout (r535_gsp_shared_init): [PTE array, 1 page][cmdq 256 KiB][msgq 256 KiB].
// Each queue: page 0 = msgqTxHeader (+ the *other* queue's read pointer at
// rxHdrOff 32), pages 1..63 = 4 KiB slots. An element is
// GSP_MSG_QUEUE_ELEMENT (48 B) + rpc_message_header_v (32 B) + payload,
// rounded up to whole slots, at most 16 slots; it may wrap from the last
// slot to slot 0 as one byte stream. Larger RPCs are split into a first
// element plus CONTINUATION_RECORD (fn 71) elements carrying the rest of
// the payload.
//
// Pointer ownership (cross-wired on purpose):
//   cmdq.tx.writePtr  CPU writes   | msgq.rx.readPtr (in msgq page 0) GSP writes
//   msgq.tx.writePtr  GSP writes   | cmdq.rx.readPtr (in cmdq page 0) CPU writes
//
// Clean-room from Linux nouveau (MIT) nvkm/subdev/gsp/rm/r535/rpc.c
// (msgq_wait 79-110, recv_one_elem 145-200, msgq_recv 202-265, cmdq_push
// 267-330, rpc_get 520-540, rpc_push 552-610, msg_recv 380-450) and
// r535/gsp.c shared_init 1137-1186.
#include <stdint.h>
#include <stddef.h>
#include "nvrm_r570.h"

namespace brook::nvgsp {

constexpr uint32_t kQueueBytes      = 0x40000;
constexpr uint32_t kQueuePage       = 0x1000;
constexpr uint32_t kQueueSlots      = (kQueueBytes - kQueuePage) / kQueuePage;  // 63
constexpr uint32_t kElemHdr         = sizeof(rm570::GSP_MSG_QUEUE_ELEMENT);     // 48
constexpr uint32_t kRpcHdr          = sizeof(rm570::rpc_message_header_v);      // 32
constexpr uint32_t kMaxElemBytes    = kQueuePage * 16;
constexpr uint32_t kMaxRpcBytes     = kMaxElemBytes - kElemHdr;                 // 65488
constexpr uint32_t kMaxPayloadBytes = kMaxRpcBytes - kRpcHdr;                   // 65456
constexpr uint32_t kRxHdrOff        = sizeof(rm570::msgqTxHeader);              // 32

struct QueueShm {
    uint8_t* base = nullptr;         // CPU mapping of the whole region
    uint64_t bus = 0;                // its bus address (sharedMemPhysAddr)
    uint32_t pte_count = 0;          // pageTableEntryCount (129)
    uint32_t ptes_size = 0;          // bytes of the PTE array (1 page)
    uint32_t cmdq_off = 0, msgq_off = 0;  // cmdQueueOffset / statQueueOffset
    uint32_t total = 0;
    uint8_t* cmdq() const { return base + cmdq_off; }
    uint8_t* msgq() const { return base + msgq_off; }
};

inline uint32_t QueueShmPteCount() {
    uint32_t n = (2 * kQueueBytes) / kQueuePage;
    return n + (n * 8 + kQueuePage - 1) / kQueuePage;            // 128 + 1
}
inline uint32_t QueueShmBytes() {
    const uint32_t ptes = (QueueShmPteCount() * 8 + kQueuePage - 1) / kQueuePage * kQueuePage;
    return ptes + 2 * kQueueBytes;                               // 0x81000
}

inline uint32_t Ld32(const uint8_t* p) { return __atomic_load_n(reinterpret_cast<const uint32_t*>(p), __ATOMIC_ACQUIRE); }
inline void St32(uint8_t* p, uint32_t v) { __atomic_store_n(reinterpret_cast<uint32_t*>(p), v, __ATOMIC_RELEASE); }
inline uint32_t Get32(const uint8_t* p) { uint32_t v; __builtin_memcpy(&v, p, 4); return v; }
inline void Put32(uint8_t* p, uint32_t v) { __builtin_memcpy(p, &v, 4); }

// Zero the region, write the PTEs (bus address of every page, contiguous
// region) and the cmdq tx header. The GSP initialises the msgq header.
inline bool InitQueueShm(uint8_t* base, uint64_t bus, uint32_t cap, QueueShm* q) {
    const uint32_t total = QueueShmBytes();
    if (!base || cap < total || (bus & (kQueuePage - 1))) return false;
    for (uint32_t i = 0; i < total; ++i) base[i] = 0;
    q->base = base; q->bus = bus; q->total = total;
    q->pte_count = QueueShmPteCount();
    q->ptes_size = (q->pte_count * 8 + kQueuePage - 1) / kQueuePage * kQueuePage;
    q->cmdq_off = q->ptes_size;
    q->msgq_off = q->cmdq_off + kQueueBytes;
    for (uint32_t i = 0; i < q->pte_count; ++i) {
        const uint64_t a = bus + uint64_t(i) * kQueuePage;
        __builtin_memcpy(base + 8 * i, &a, 8);
    }
    rm570::msgqTxHeader tx = {};
    tx.version = 0; tx.size = kQueueBytes; tx.entryOff = kQueuePage; tx.msgSize = kQueuePage;
    tx.msgCount = kQueueSlots; tx.writePtr = 0; tx.flags = 1; tx.rxHdrOff = kRxHdrOff;
    __builtin_memcpy(q->cmdq(), &tx, sizeof(tx));
    return true;
}

// XOR of the element's u64 words, folded to 32 bits (cmdq_push).
inline uint32_t ElemChecksum(const uint8_t* a, uint32_t alen, const uint8_t* b, uint32_t blen) {
    uint64_t c = 0, w;
    for (uint32_t i = 0; i + 8 <= alen; i += 8) { __builtin_memcpy(&w, a + i, 8); c ^= w; }
    for (uint32_t i = 0; i + 8 <= blen; i += 8) { __builtin_memcpy(&w, b + i, 8); c ^= w; }
    return uint32_t(c >> 32) ^ uint32_t(c);
}

enum class QStatus : uint8_t { Ok, Timeout, TooBig, BadElement, BadChecksum, NotContinuation, Overflow, RpcError };

// Message ring geometry shared by both directions.
struct Ring {
    uint8_t* hdr_page;   // page 0 of this queue (tx header)
    uint8_t* rptr;       // reader's pointer (lives in the other queue's page 0)
    uint8_t* slot(uint32_t i) const { return hdr_page + kQueuePage + i * kQueuePage; }
    uint32_t wptr() const { return Ld32(hdr_page + offsetof(rm570::msgqTxHeader, writePtr)); }
    void set_wptr(uint32_t v) const { St32(hdr_page + offsetof(rm570::msgqTxHeader, writePtr), v); }
    uint32_t rd() const { return Ld32(rptr); }
    void set_rd(uint32_t v) const { St32(rptr, v); }
};
inline uint32_t RingUsed(uint32_t w, uint32_t r) { return (w + kQueueSlots - r) % kQueueSlots; }
inline uint32_t RingFree(uint32_t w, uint32_t r) { return kQueueSlots - 1 - RingUsed(w, r); }

// Copy `len` bytes of an element stream into the ring at slot w, wrapping.
inline void RingWrite(const Ring& q, uint32_t w, const uint8_t* src, uint32_t len) {
    const uint32_t to_end = (kQueueSlots - w) * kQueuePage;
    const uint32_t a = len < to_end ? len : to_end;
    __builtin_memcpy(q.slot(w), src, a);
    if (len > a) __builtin_memcpy(q.slot(0), src + a, len - a);
}
inline void RingRead(const Ring& q, uint32_t r, uint8_t* dst, uint32_t off, uint32_t len) {
    // `off` is the byte offset into the element stream starting at slot r.
    const uint32_t to_end = (kQueueSlots - r) * kQueuePage;
    for (uint32_t done = 0; done < len;) {
        const uint32_t pos = off + done;
        uint32_t n;
        const uint8_t* s;
        if (pos < to_end) { s = q.slot(r) + pos; n = to_end - pos; }
        else { s = q.slot(0) + (pos - to_end); n = kQueueSlots * kQueuePage; }
        if (n > len - done) n = len - done;
        __builtin_memcpy(dst + done, s, n);
        done += n;
    }
}

// CPU-side channel. Waiter: `bool operator()()` spins/yields once and
// returns false when the caller's budget is exhausted. Doorbell: called
// after each published element (GSP falcon +0xc00 = 0 on hardware).
class RpcChannel {
public:
    RpcChannel() = default;
    explicit RpcChannel(const QueueShm& shm)
        : cmdq_{shm.cmdq(), shm.msgq() + kRxHdrOff},
          msgq_{shm.msgq(), shm.cmdq() + kRxHdrOff} {}

    // (Re)bind to a freshly initialised shared region (live: the channel's
    // 128 KiB of buffers are allocated before the queue memory exists).
    void Attach(const QueueShm& shm) {
        cmdq_ = Ring{shm.cmdq(), shm.msgq() + kRxHdrOff};
        msgq_ = Ring{shm.msgq(), shm.cmdq() + kRxHdrOff};
        cmdq_seq_ = 0; rpc_seq_ = 0;
    }
    uint32_t cmdq_seq() const { return cmdq_seq_; }
    uint32_t rpc_seq() const { return rpc_seq_; }

    // One element: `rpc` = rpc_message_header_v + payload, length rpc_len.
    template <class Waiter, class Doorbell>
    QStatus PushElement(const uint8_t* rpc, uint32_t rpc_len, Waiter& wait, Doorbell& ring) {
        if (rpc_len < kRpcHdr || rpc_len > kMaxRpcBytes) return QStatus::TooBig;
        const uint32_t bytes = (kElemHdr + rpc_len + kQueuePage - 1) / kQueuePage * kQueuePage;
        const uint32_t slots = bytes / kQueuePage;
        uint8_t* e = elem_;
        for (uint32_t i = 0; i < bytes; ++i) e[i] = 0;
        __builtin_memcpy(e + kElemHdr, rpc, rpc_len);
        Put32(e + offsetof(rm570::GSP_MSG_QUEUE_ELEMENT, seqNum), cmdq_seq_++);
        Put32(e + offsetof(rm570::GSP_MSG_QUEUE_ELEMENT, elemCount), slots);
        Put32(e + offsetof(rm570::GSP_MSG_QUEUE_ELEMENT, checkSum), ElemChecksum(e, bytes, nullptr, 0));
        const uint32_t w = cmdq_.wptr();
        while (RingFree(w, cmdq_.rd()) < slots)
            if (!wait()) return QStatus::Timeout;
        RingWrite(cmdq_, w, e, bytes);
        cmdq_.set_wptr((w + slots) % kQueueSlots);
        ring();
        return QStatus::Ok;
    }

    // Build and send one RPC, splitting into CONTINUATION_RECORDs when the
    // payload exceeds one element (rpc_push).
    template <class Waiter, class Doorbell>
    QStatus Send(uint32_t fn, const uint8_t* payload, uint32_t len, Waiter& wait, Doorbell& ring,
                 bool noseq = false) {
        uint32_t first = len > kMaxPayloadBytes ? kMaxPayloadBytes : len;
        QStatus s = SendOne(fn, payload, first, noseq ? 0 : rpc_seq_++, wait, ring);
        for (uint32_t off = first; s == QStatus::Ok && off < len;) {
            const uint32_t n = len - off > kMaxPayloadBytes ? kMaxPayloadBytes : len - off;
            s = SendOne(rm570::NV_VGPU_MSG_FUNCTION_CONTINUATION_RECORD, payload + off, n, rpc_seq_++, wait, ring);
            off += n;
        }
        return s;
    }

    bool MsgPending() const { return msgq_.wptr() != msgq_.rd(); }

    // Receive one logical message into buf (rpc header + payload). With
    // expected_total > one element, continuation records are appended and
    // the returned header's length is set to the total (msgq_recv).
    template <class Waiter>
    QStatus Recv(uint8_t* buf, uint32_t cap, uint32_t* out_len, uint32_t expected_total, Waiter& wait) {
        uint32_t len = 0;
        QStatus s = RecvElement(buf, cap, &len, false, wait);
        if (s != QStatus::Ok) return s;
        uint32_t have = len;
        while (expected_total > kMaxRpcBytes && have < expected_total) {
            uint32_t n = 0;
            s = RecvElement(buf + have, cap - have, &n, true, wait);
            if (s != QStatus::Ok) return s;
            have += n;
        }
        if (have != len) Put32(buf + offsetof(rm570::rpc_message_header_v, length), have);
        *out_len = have;
        return QStatus::Ok;
    }

    // Wait for function `fn`, handing every other message to on_event(buf,len)
    // (out-of-order events such as RUN_CPU_SEQUENCER, POST_NOCAT_RECORD).
    // on_event returns false to abort.
    template <class Waiter, class OnEvent>
    QStatus WaitFor(uint32_t fn, uint8_t* buf, uint32_t cap, uint32_t* out_len, uint32_t expected_total,
                    Waiter& wait, OnEvent& on_event) {
        for (;;) {
            uint32_t len = 0;
            QStatus s = Recv(buf, cap, &len, 0, wait);
            if (s != QStatus::Ok) return s;
            const uint32_t f = Get32(buf + offsetof(rm570::rpc_message_header_v, function));
            if (f == fn) {
                if (expected_total > kMaxRpcBytes) {
                    uint32_t have = len;
                    while (have < expected_total) {
                        uint32_t n = 0;
                        s = RecvElement(buf + have, cap - have, &n, true, wait);
                        if (s != QStatus::Ok) return s;
                        have += n;
                    }
                    Put32(buf + offsetof(rm570::rpc_message_header_v, length), have);
                    len = have;
                }
                *out_len = len;
                return Get32(buf + offsetof(rm570::rpc_message_header_v, rpc_result)) ? QStatus::RpcError
                                                                                      : QStatus::Ok;
            }
            if (!on_event(buf, len)) return QStatus::BadElement;
        }
    }

private:
    template <class Waiter, class Doorbell>
    QStatus SendOne(uint32_t fn, const uint8_t* payload, uint32_t n, uint32_t seq, Waiter& wait, Doorbell& ring) {
        uint8_t* r = rpc_;
        rm570::rpc_message_header_v h = {};
        h.header_version = rm570::RPC_HEADER_VERSION; h.signature = rm570::RPC_SIGNATURE;
        h.length = kRpcHdr + n; h.function = fn; h.rpc_result = 0xffffffff;
        h.rpc_result_private = 0xffffffff; h.sequence = seq;
        __builtin_memcpy(r, &h, kRpcHdr);
        if (n) __builtin_memcpy(r + kRpcHdr, payload, n);
        return PushElement(r, kRpcHdr + n, wait, ring);
    }

    // continuation: strip the record's own rpc header, append its payload.
    template <class Waiter>
    QStatus RecvElement(uint8_t* dst, uint32_t cap, uint32_t* out, bool continuation, Waiter& wait) {
        const uint32_t r = msgq_.rd();
        while (RingUsed(msgq_.wptr(), r) < 1)
            if (!wait()) return QStatus::Timeout;
        uint8_t hdr[kElemHdr + kRpcHdr];
        RingRead(msgq_, r, hdr, 0, sizeof(hdr));
        const uint32_t len = Get32(hdr + kElemHdr + offsetof(rm570::rpc_message_header_v, length));
        const uint32_t fn = Get32(hdr + kElemHdr + offsetof(rm570::rpc_message_header_v, function));
        const uint32_t count = Get32(hdr + offsetof(rm570::GSP_MSG_QUEUE_ELEMENT, elemCount));
        if (len < kRpcHdr || len > kMaxRpcBytes) return QStatus::BadElement;
        const uint32_t slots = (kElemHdr + len + kQueuePage - 1) / kQueuePage;
        if (count != slots) return QStatus::BadElement;
        while (RingUsed(msgq_.wptr(), r) < slots)
            if (!wait()) return QStatus::Timeout;
        if (continuation && fn != rm570::NV_VGPU_MSG_FUNCTION_CONTINUATION_RECORD) return QStatus::NotContinuation;
        // checksum over the whole element (stored checksum included -> 0)
        const uint32_t bytes = slots * kQueuePage;
        const uint32_t to_end = (kQueueSlots - r) * kQueuePage;
        const uint32_t a = bytes < to_end ? bytes : to_end;
        if (ElemChecksum(msgq_.slot(r), a, msgq_.slot(0), bytes - a) != 0) return QStatus::BadChecksum;
        const uint32_t skip = continuation ? kRpcHdr : 0;
        const uint32_t n = len - skip;
        if (n > cap) return QStatus::Overflow;
        RingRead(msgq_, r, dst, kElemHdr + skip, n);
        msgq_.set_rd((r + slots) % kQueueSlots);
        *out = n;
        return QStatus::Ok;
    }

    Ring cmdq_ = {}, msgq_ = {};
    uint32_t cmdq_seq_ = 0, rpc_seq_ = 0;
    alignas(8) uint8_t elem_[kMaxElemBytes] = {};
    alignas(8) uint8_t rpc_[kMaxRpcBytes] = {};
};

} // namespace brook::nvgsp
