#pragma once

// Lockless MPSC (multi-producer, single-consumer) fixed-slot ring queue.
//
// Design
// ------
// The queue holds N fixed-size slots. Producers atomically claim the next
// slot via a monotonic counter, fill it, then mark it "ready". The single
// consumer reads slots in claim order; it advances only when the next slot
// is marked ready, so writes from different producers never interleave.
//
// No spinlock, no mutex, no disabling interrupts. Producers may spin briefly
// only if the queue is full (consumer hasn't freed the slot yet). Because
// no lock is held during that spin, the scheduler can freely preempt the
// spinning producer and run the consumer thread.
//
// Constraints
// -----------
//  - Only ONE consumer thread may call dequeue().
//  - Any number of producers may call enqueue() concurrently.
//  - enqueue() must NOT be called from ISR context if the consumer thread
//    can be interrupted on the same CPU (the spin-until-free could deadlock
//    if the ISR preempts the consumer mid-drain). ISR producers should use
//    a separate lock-free ring and have a thread drain it — exactly what
//    profiler.cpp already does.
//  - N must be a power of two.
//  - SlotBytes is the maximum payload per enqueue call. Callers must split
//    larger writes; see SerialWriterEnqueue for an example.
//
// Memory
// ------
//  sizeof(MpscQueue<N, SlotBytes>) ≈ N × (8 + SlotBytes) bytes (static).
//
// State machine per slot
// ----------------------
//   Free (0) → [producer claims] → Ready (1) → [consumer reads] → Free (0)
//
// The Ready store uses RELEASE; the consumer load uses ACQUIRE. This ensures
// the payload bytes are visible to the consumer before the state transition.

#include <stdint.h>

namespace brook {

template<uint32_t N, uint32_t SlotBytes>
struct MpscQueue {
    static_assert((N & (N - 1)) == 0, "N must be a power of two");
    static_assert(SlotBytes >= 1, "SlotBytes must be >= 1");

    struct alignas(64) Slot {
        volatile uint32_t state;    // 0 = Free, 1 = Ready
        uint32_t          len;
        char              data[SlotBytes];
    };

    alignas(64) Slot            slots[N];
    alignas(64) volatile uint64_t claimed  = 0; // monotonic, incremented by producers
    alignas(64)          uint64_t consumed = 0; // consumer-private, no atomic needed

    // Enqueue up to SlotBytes bytes. Truncates silently if len > SlotBytes.
    // Spins (without holding any lock) if the queue is full.
    void enqueue(const char* src, uint32_t len) {
        if (len > SlotBytes) len = SlotBytes;

        // Claim the next slot atomically.
        uint64_t idx = __atomic_fetch_add(&claimed, 1, __ATOMIC_RELAXED);
        Slot& s = slots[idx & (N - 1)];

        // If the queue is full, this slot hasn't been freed yet. Spin until
        // the consumer releases it. No lock is held during this spin, so the
        // scheduler can preempt us and run the consumer.
        while (__atomic_load_n(&s.state, __ATOMIC_ACQUIRE) != 0)
            __asm__ volatile("pause" ::: "memory");

        s.len = len;
        __builtin_memcpy(s.data, src, len);

        // RELEASE ensures payload bytes are visible to consumer before state=1.
        __atomic_store_n(&s.state, 1u, __ATOMIC_RELEASE);
    }

    // Dequeue one slot. Returns bytes read, or 0 if the queue is empty.
    // Only call from the single consumer thread.
    uint32_t dequeue(char* dst, uint32_t maxLen) {
        Slot& s = slots[consumed & (N - 1)];
        if (__atomic_load_n(&s.state, __ATOMIC_ACQUIRE) != 1)
            return 0;

        uint32_t len = (s.len < maxLen) ? s.len : maxLen;
        __builtin_memcpy(dst, s.data, len);

        // RELEASE pairs with the producer's ACQUIRE spin, allowing reuse.
        __atomic_store_n(&s.state, 0u, __ATOMIC_RELEASE);
        consumed++;
        return len;
    }

    bool empty() const {
        return __atomic_load_n(&slots[consumed & (N - 1)].state, __ATOMIC_ACQUIRE) == 0;
    }
};

} // namespace brook
