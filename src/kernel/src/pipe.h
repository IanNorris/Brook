#pragma once

#include <stdint.h>
#include "memory/heap.h"
#include "spinlock.h"
#if defined(__BROOK_KERNEL__)
#include "string.h"
#else
#include <cstring>
#endif

namespace brook {

struct Process;  // forward declaration for waiter
struct PipePollNode;
struct PipeBuffer;
void PipePollNotify(PipeBuffer* pipe, uint32_t events);

// Kernel pipe buffer — ring buffer with reader/writer reference counts.
// Blocking read/write via SchedulerBlock() when buffer is empty/full.
static constexpr uint32_t PIPE_BUF_DEFAULT_SIZE = 65536;
static constexpr uint32_t PIPE_BUF_UNIX_SIZE    = 65536;

enum class PipeReadWait { Retry, Eof, WouldBlock, Registered };

struct PipeBuffer
{
    char*    data = nullptr;
    uint32_t capacity = 0;
    volatile uint32_t head = 0;
    volatile uint32_t tail = 0;
    SpinLock lock = {};

    volatile uint32_t readers = 0;   // Number of FDs open for reading
    volatile uint32_t writers = 0;   // Number of FDs open for writing
    volatile uint64_t fullWriteWaits = 0; // Confirmed-full blocking attempts

    // AF_UNIX direction shutdown, independent of endpoint ownership. Protected
    // by lock for mutation; readiness paths take atomic snapshots. Ordinary
    // pipes leave these false. Shutdown must never release a live pipe's refs.
    bool unixReadShutdown = false;
    bool unixWriteShutdown = false;

    // Poll subscriptions own storage-only holds; these do not keep an endpoint
    // open or suppress EOF. The list and destruction state use lock.
    PipePollNode* pollWaiters = nullptr;
    uint64_t pollHolds = 0;
    uint64_t pollSequence = 0;
    bool destroyPending = false;

    // Waiter processes (set before blocking, cleared on wake)
    Process* volatile readerWaiter = nullptr;
    Process* volatile writerWaiter = nullptr;
    // Dedicated kernel terminal reader. Registration/removal and wake execute
    // under lock, with the terminal thread retaining itself until removal.
    Process* terminalReader = nullptr;

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

    // Recheck after an empty read while excluding writes and endpoint close.
    // Buffered bytes take precedence over EOF and nonblocking EAGAIN. This
    // closes the arrival-before-registration race; the scheduler's pending
    // wake flag handles a notification after registration but before block.
    // This retains the existing single-waiter model, not a general wait queue.
    PipeReadWait prepareReadWait(Process* self, bool nonblocking)
    {
        SpinLockAcquire(&lock);
        PipeReadWait result;
        if (!empty()) result = PipeReadWait::Retry;
        else if (__atomic_load_n(&writers, __ATOMIC_ACQUIRE) == 0)
            result = PipeReadWait::Eof;
        else if (nonblocking) result = PipeReadWait::WouldBlock;
        else { readerWaiter = self; result = PipeReadWait::Registered; }
        SpinLockRelease(&lock);
        return result;
    }

    void removeReadWait(Process* self)
    {
        SpinLockAcquire(&lock);
        if (readerWaiter == self) readerWaiter = nullptr;
        SpinLockRelease(&lock);
    }

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
        if (len) PipePollNotify(this, 1);
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
        if (len) PipePollNotify(this, 4);
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

static inline void PipeBufferFree(PipeBuffer* pipe)
{
    if (!pipe) return;
    if (pipe->data)
        kfree(pipe->data);
    kfree(pipe);
}

// Callers must own a live endpoint or storage hold when acquiring another hold.
static inline void PipeBufferHold(PipeBuffer* pipe)
{
    SpinLockAcquire(&pipe->lock);
    // Corrupted/wrapped ownership must never make live storage reclaimable.
    if (pipe->pollHolds == ~0ULL) __builtin_trap();
    ++pipe->pollHolds;
    SpinLockRelease(&pipe->lock);
}

static inline void PipeBufferDrop(PipeBuffer* pipe, bool destroy = false)
{
    SpinLockAcquire(&pipe->lock);
    if (!pipe->pollHolds) __builtin_trap();
    --pipe->pollHolds;
    if (destroy) pipe->destroyPending = true;
    const bool release = pipe->destroyPending && !pipe->pollHolds;
    SpinLockRelease(&pipe->lock);
    if (release) PipeBufferFree(pipe);
}

static inline void PipeBufferDestroy(PipeBuffer* pipe)
{
    if (!pipe) return;
    SpinLockAcquire(&pipe->lock);
    pipe->destroyPending = true;
    const bool release = !pipe->pollHolds;
    SpinLockRelease(&pipe->lock);
    if (release) PipeBufferFree(pipe);
}

void PipeReleaseEnd(PipeBuffer* pipe,bool writer);

} // namespace brook
