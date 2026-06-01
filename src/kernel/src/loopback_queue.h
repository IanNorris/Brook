// loopback_queue.h — Iterative loopback delivery trampoline (BRO-163).
//
// PROBLEM: NetSendIpv4's loopback fast-path used to deliver frames by calling
// HandleIpv4() synchronously on the sender's stack. Because HandleIpv4 ->
// HandleTcp -> TcpSendSegment -> NetSendIpv4 can itself emit another loopback
// frame (ACK-clocked transmission, dup-ACKs, window updates), a single
// loopback TCP transfer collapsed into unbounded mutual recursion. Each level
// carries a ~1.5 KiB on-stack Ethernet frame, so ~70-85 levels exhaust the
// 256 KiB kernel stack and #DF (guard-page push fault). See BRO-163 / BRO-010.
//
// FIX: convert the recursion into iteration. Loopback frames are copied into a
// bounded FIFO. The first (non-re-entrant) submitter becomes the drainer and
// processes frames in a loop. Frames produced *during* delivery (i.e. while a
// drain is already in progress) are enqueued and picked up by the running
// loop instead of recursing. Stack depth is therefore O(1) — exactly one
// HandleIpv4 frame is ever live per drainer — regardless of how many frames
// bounce.
//
// This header is intentionally free of kernel dependencies (only <stdint.h>
// and a memcpy provided by the includer) so the trampoline invariant can be
// unit-tested host-natively (see src/tests/test_loopback). Synchronization and
// the per-stack drain guard are injected by the caller via callables.

#pragma once

#include <stdint.h>

namespace brook {

// Fixed-capacity FIFO of pending loopback Ethernet frames.
//
// NOT internally synchronized: callers serialize Enqueue/Dequeue with an
// external lock (kernel: net SpinLock; host test: single-threaded or mutex).
// Frames are *copied* in/out because a re-entrant producer's source buffer
// (a deeper stack frame) is destroyed before the drain loop reaches it.
template <uint32_t SlotCount, uint32_t SlotSize>
struct LoopbackQueueT {
    static constexpr uint32_t kSlotCount = SlotCount;
    static constexpr uint32_t kSlotSize  = SlotSize;

    struct Slot {
        uint32_t len;
        uint8_t  data[SlotSize];
    };

    Slot     slots[SlotCount];
    uint32_t head;       // index of next frame to dequeue
    uint32_t tail;       // index of next free slot
    uint32_t count;      // frames currently queued
    uint32_t highWater;  // max 'count' ever observed (diagnostics)
    uint64_t dropped;    // frames dropped (queue full or bad length)

    void Reset() {
        head = tail = count = highWater = 0;
        dropped = 0;
    }

    bool Enqueue(const uint8_t* frame, uint32_t len) {
        if (len == 0 || len > SlotSize) { dropped++; return false; }
        if (count >= SlotCount)         { dropped++; return false; }
        Slot& s = slots[tail];
        s.len = len;
        memcpy(s.data, frame, len);
        tail = (tail + 1) % SlotCount;
        if (++count > highWater) highWater = count;
        return true;
    }

    bool Dequeue(uint8_t* out, uint32_t* outLen) {
        if (count == 0) return false;
        Slot& s = slots[head];
        *outLen = s.len;
        memcpy(out, s.data, s.len);
        head = (head + 1) % SlotCount;
        count--;
        return true;
    }
};

// Submit a loopback frame for delivery and, if no drain is already running,
// drain the queue iteratively.
//
//   q        : the LoopbackQueueT instance (shared, lock-protected).
//   draining : drain-in-progress guard. Reads/writes happen under 'lock'.
//              Backed by a single shared bool in the kernel — guarantees at
//              most one live deliver() frame on any stack at a time.
//   frame/len: the loopback frame to deliver (copied into the queue).
//   lock/unlock: serialize access to 'q' and 'draining'. Held only around the
//              queue ops, NEVER across deliver().
//   deliver  : processes one frame (e.g. HandleIpv4). Runs UNLOCKED and may
//              re-enter LoopbackSubmit — such frames are queued, not recursed.
//
// CRITICAL: 'lock' must not be held while 'deliver' runs; deliver acquires
// other locks and can block on the remote path.
template <typename Q, typename LockFn, typename UnlockFn, typename DeliverFn>
inline void LoopbackSubmit(Q& q, bool& draining,
                           const uint8_t* frame, uint32_t len,
                           LockFn lock, UnlockFn unlock, DeliverFn deliver) {
    lock();
    q.Enqueue(frame, len);
    if (draining) {
        // A drain loop is already running (this thread, deeper on the stack,
        // or another CPU). It will pick up the frame we just queued.
        unlock();
        return;
    }
    draining = true;
    unlock();

    uint8_t   scratch[Q::kSlotSize];
    uint32_t  slen = 0;
    for (;;) {
        lock();
        bool got = q.Dequeue(scratch, &slen);
        if (!got) {
            draining = false;
            unlock();
            return;
        }
        unlock();
        deliver(scratch, slen);  // may re-enter LoopbackSubmit -> enqueues
    }
}

} // namespace brook
