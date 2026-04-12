#pragma once

#include <stdint.h>
#include "spinlock.h"

namespace brook {

// Fixed-size ring buffer for multi-producer, single-consumer use.
// Protected by a spinlock (IRQ-safe). Used for async serial/TTY output.
template <uint32_t Capacity>
struct KRingBuffer {
    char     data[Capacity];
    volatile uint32_t head = 0;   // Next write position (producer)
    volatile uint32_t tail = 0;   // Next read position  (consumer)
    SpinLock lock = {};

    // Returns number of bytes currently in the buffer.
    uint32_t count() const
    {
        uint32_t h = __atomic_load_n(&head, __ATOMIC_ACQUIRE);
        uint32_t t = __atomic_load_n(&tail, __ATOMIC_ACQUIRE);
        return (h - t + Capacity) % Capacity;
    }

    // Returns available space (one slot reserved to distinguish full from empty).
    uint32_t space() const { return Capacity - 1 - count(); }

    bool empty() const { return __atomic_load_n(&head, __ATOMIC_ACQUIRE) ==
                                __atomic_load_n(&tail, __ATOMIC_ACQUIRE); }

    // Enqueue up to `len` bytes. Returns number of bytes actually written.
    // Drops excess bytes if the buffer is full (never blocks).
    uint32_t write(const char* src, uint32_t len)
    {
        uint64_t flags = SpinLockAcquire(&lock);

        uint32_t avail = Capacity - 1 - ((head - tail + Capacity) % Capacity);
        if (len > avail) len = avail;

        for (uint32_t i = 0; i < len; ++i) {
            data[head] = src[i];
            head = (head + 1) % Capacity;
        }

        SpinLockRelease(&lock, flags);
        return len;
    }

    // Dequeue up to `maxLen` bytes into `dst`. Returns bytes read.
    // Only called by the single consumer thread.
    uint32_t read(char* dst, uint32_t maxLen)
    {
        uint64_t flags = SpinLockAcquire(&lock);

        uint32_t avail = (head - tail + Capacity) % Capacity;
        if (maxLen > avail) maxLen = avail;

        for (uint32_t i = 0; i < maxLen; ++i) {
            dst[i] = data[tail];
            tail = (tail + 1) % Capacity;
        }

        SpinLockRelease(&lock, flags);
        return maxLen;
    }
};

} // namespace brook
