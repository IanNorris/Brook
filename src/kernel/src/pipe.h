#pragma once

#include <stdint.h>
#include "memory/heap.h"
#include "spinlock.h"
#include "string.h"

namespace brook {

struct Process;  // forward declaration for waiter

// Kernel pipe buffer — ring buffer with reader/writer reference counts.
// Blocking read/write via SchedulerBlock() when buffer is empty/full.
static constexpr uint32_t PIPE_BUF_DEFAULT_SIZE = 4096;
static constexpr uint32_t PIPE_BUF_UNIX_SIZE    = 65536;

struct PipeBuffer
{
    char*    data = nullptr;
    uint32_t capacity = 0;
    volatile uint32_t head = 0;
    volatile uint32_t tail = 0;
    SpinLock lock = {};

    volatile uint32_t readers = 0;   // Number of FDs open for reading
    volatile uint32_t writers = 0;   // Number of FDs open for writing

    // Waiter processes (set before blocking, cleared on wake)
    Process* volatile readerWaiter = nullptr;
    Process* volatile writerWaiter = nullptr;

    // Process currently blocked inside epoll_wait watching this pipe for
    // readability. Set by epoll_wait_impl before it blocks, cleared on wake.
    // Writers check this after appending data and SchedulerUnblock() it.
    Process* volatile epollWaiter = nullptr;

    uint32_t count() const
    {
        if (capacity == 0) return 0;
        return (head - tail + capacity) % capacity;
    }

    uint32_t space() const
    {
        if (capacity == 0) return 0;
        return capacity - 1 - count();
    }

    bool empty() const { return head == tail; }

    // Non-blocking write. Returns bytes written (may be partial).
    uint32_t write(const char* src, uint32_t len)
    {
        SpinLockAcquire(&lock);

        if (!data || capacity == 0)
        {
            SpinLockRelease(&lock);
            return 0;
        }

        uint32_t avail = capacity - 1 - ((head - tail + capacity) % capacity);
        if (len > avail) len = avail;

        // Copy in up to two contiguous chunks (ring buffer wrap)
        uint32_t firstChunk = capacity - head;
        if (firstChunk > len) firstChunk = len;
        memcpy(data + head, src, firstChunk);

        if (len > firstChunk)
            memcpy(data, src + firstChunk, len - firstChunk);

        head = (head + len) % capacity;

        SpinLockRelease(&lock);
        return len;
    }

    // Non-blocking read. Returns bytes read (may be 0).
    uint32_t read(char* dst, uint32_t len)
    {
        SpinLockAcquire(&lock);

        if (!data || capacity == 0)
        {
            SpinLockRelease(&lock);
            return 0;
        }

        uint32_t avail = (head - tail + capacity) % capacity;
        if (len > avail) len = avail;

        // Copy in up to two contiguous chunks (ring buffer wrap)
        uint32_t firstChunk = capacity - tail;
        if (firstChunk > len) firstChunk = len;
        memcpy(dst, data + tail, firstChunk);

        if (len > firstChunk)
            memcpy(dst + firstChunk, data, len - firstChunk);

        tail = (tail + len) % capacity;

        SpinLockRelease(&lock);
        return len;
    }
};

static inline PipeBuffer* PipeBufferCreate(uint32_t capacity)
{
    if (capacity < 2) return nullptr;

    auto* pipe = static_cast<PipeBuffer*>(kmalloc(sizeof(PipeBuffer)));
    if (!pipe) return nullptr;
    memset(pipe, 0, sizeof(PipeBuffer));

    pipe->data = static_cast<char*>(kmalloc(capacity));
    if (!pipe->data)
    {
        kfree(pipe);
        return nullptr;
    }
    memset(pipe->data, 0, capacity);

    pipe->capacity = capacity;
    return pipe;
}

static inline void PipeBufferDestroy(PipeBuffer* pipe)
{
    if (!pipe) return;
    if (pipe->data)
        kfree(pipe->data);
    kfree(pipe);
}

} // namespace brook
