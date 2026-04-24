#pragma once

#include <stdint.h>
#include "spinlock.h"

namespace brook {

struct Process;  // forward declaration for waiter

// Kernel pipe buffer — fixed-size ring buffer with reader/writer reference counts.
// Blocking read/write via SchedulerBlock() when buffer is empty/full.
static constexpr uint32_t PIPE_BUF_SIZE = 4096;

struct PipeBuffer
{
    char     data[PIPE_BUF_SIZE];
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
        return (head - tail + PIPE_BUF_SIZE) % PIPE_BUF_SIZE;
    }

    uint32_t space() const
    {
        return PIPE_BUF_SIZE - 1 - count();
    }

    bool empty() const { return head == tail; }

    // Non-blocking write. Returns bytes written (may be partial).
    uint32_t write(const char* src, uint32_t len)
    {
        uint64_t flags = SpinLockAcquire(&lock);

        uint32_t avail = PIPE_BUF_SIZE - 1 - ((head - tail + PIPE_BUF_SIZE) % PIPE_BUF_SIZE);
        if (len > avail) len = avail;

        for (uint32_t i = 0; i < len; ++i)
        {
            data[head] = src[i];
            head = (head + 1) % PIPE_BUF_SIZE;
        }

        SpinLockRelease(&lock, flags);
        return len;
    }

    // Non-blocking read. Returns bytes read (may be 0).
    uint32_t read(char* dst, uint32_t len)
    {
        uint64_t flags = SpinLockAcquire(&lock);

        uint32_t avail = (head - tail + PIPE_BUF_SIZE) % PIPE_BUF_SIZE;
        if (len > avail) len = avail;

        for (uint32_t i = 0; i < len; ++i)
        {
            dst[i] = data[tail];
            tail = (tail + 1) % PIPE_BUF_SIZE;
        }

        SpinLockRelease(&lock, flags);
        return len;
    }
};

} // namespace brook
