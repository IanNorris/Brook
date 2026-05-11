#pragma once

#include <stdint.h>
#include "spinlock.h"
#include "string.h"

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
        SpinLockAcquire(&lock);

        uint32_t avail = Capacity - 1 - ((head - tail + Capacity) % Capacity);
        if (len > avail) len = avail;

        uint32_t firstChunk = Capacity - head;
        if (firstChunk > len) firstChunk = len;
        memcpy(data + head, src, firstChunk);

        if (len > firstChunk)
            memcpy(data, src + firstChunk, len - firstChunk);

        head = (head + len) % Capacity;

        SpinLockRelease(&lock);
        return len;
    }

    // Dequeue up to `maxLen` bytes into `dst`. Returns bytes read.
    // Only called by the single consumer thread.
    uint32_t read(char* dst, uint32_t maxLen)
    {
        SpinLockAcquire(&lock);

        uint32_t avail = (head - tail + Capacity) % Capacity;
        if (maxLen > avail) maxLen = avail;

        uint32_t firstChunk = Capacity - tail;
        if (firstChunk > maxLen) firstChunk = maxLen;
        memcpy(dst, data + tail, firstChunk);

        if (maxLen > firstChunk)
            memcpy(dst + firstChunk, data, maxLen - firstChunk);

        tail = (tail + maxLen) % Capacity;

        SpinLockRelease(&lock);
        return maxLen;
    }

    // Dequeue bytes up to and including the first newline, or up to maxLen
    // if no newline is found. Returns bytes read. This allows line-buffered
    // output so serial messages don't interleave mid-line.
    uint32_t readUntilNewline(char* dst, uint32_t maxLen)
    {
        SpinLockAcquire(&lock);

        uint32_t avail = (head - tail + Capacity) % Capacity;
        if (maxLen > avail) maxLen = avail;

        // Scan for newline to determine how many bytes to dequeue.
        uint32_t limit = maxLen;
        uint32_t scan = tail;
        for (uint32_t i = 0; i < maxLen; ++i) {
            if (data[scan] == '\n') {
                limit = i + 1; // include the newline
                break;
            }
            scan = (scan + 1) % Capacity;
        }

        uint32_t firstChunk = Capacity - tail;
        if (firstChunk > limit) firstChunk = limit;
        memcpy(dst, data + tail, firstChunk);

        if (limit > firstChunk)
            memcpy(dst + firstChunk, data, limit - firstChunk);

        tail = (tail + limit) % Capacity;

        SpinLockRelease(&lock);
        return limit;
    }
};

} // namespace brook
