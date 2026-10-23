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
    // Optional packet length at each ring offset. Zero means stream data.
    uint16_t* packets = nullptr;
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

    // Allocate before taking the spinlock. Only packet-enabled pipes pay for
    // metadata; keeping it after disabling O_DIRECT preserves queued packets.
    bool enablePackets()
    {
        SpinLockAcquire(&lock);
        bool ready = packets != nullptr;
        SpinLockRelease(&lock);
        if (ready) return true;
        auto* fresh = static_cast<uint16_t*>(kmalloc(uint64_t(capacity) * sizeof(uint16_t)));
        if (!fresh) return false;
        memset(fresh, 0, uint64_t(capacity) * sizeof(uint16_t));
        SpinLockAcquire(&lock);
        if (!packets) { __atomic_store_n(&packets,fresh,__ATOMIC_RELEASE); fresh = nullptr; }
        SpinLockRelease(&lock);
        if (fresh) kfree(fresh);
        return true;
    }

    // Non-blocking write. Returns bytes written (may be partial).
    uint32_t write(const char* src, uint32_t len, bool atomicRecord=false, bool packet=false)
    {
        SpinLockAcquire(&lock);

        if (!data || capacity == 0)
        {
            SpinLockRelease(&lock);
            return 0;
        }

        uint32_t avail = capacity - 1 - ((head - tail + capacity) % capacity);
        if(packet && (!packets || len>4096)) {SpinLockRelease(&lock);return 0;}
        if((atomicRecord || packet) && len>avail) {SpinLockRelease(&lock);return 0;}
        if (len > avail) len = avail;

        // Copy in up to two contiguous chunks (ring buffer wrap)
        uint32_t firstChunk = capacity - head;
        if (firstChunk > len) firstChunk = len;
        memcpy(data + head, src, firstChunk);

        if (len > firstChunk)
            memcpy(data, src + firstChunk, len - firstChunk);

        if (packets && len) {
            for (uint32_t i=0;i<len;++i) packets[(head+i)%capacity]=0;
            if (packet) packets[head]=static_cast<uint16_t>(len);
        }
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

        uint32_t available = (head - tail + capacity) % capacity;
        uint32_t copied = 0;
        while (available && copied < len) {
            const uint32_t packet = packets ? packets[tail] : 0;
            uint32_t consume = packet ? packet : available;
            if (!packet && packets) {
                for (uint32_t i=1;i<consume;++i)
                    if (packets[(tail+i)%capacity]) { consume=i; break; }
            }
            uint32_t take = consume < len-copied ? consume : len-copied;
            uint32_t first = capacity-tail;
            if (first>take) first=take;
            memcpy(dst+copied,data+tail,first);
            if(take>first)memcpy(dst+copied+first,data,take-first);
            // A short packet read discards its unread suffix. Zero-size reads
            // never enter this loop and therefore never consume a packet.
            if(!packet)consume=take;
            if(packets)for(uint32_t i=0;i<consume;++i)packets[(tail+i)%capacity]=0;
            tail=(tail+consume)%capacity;
            available-=consume;copied+=take;
            if(packet)break;
        }
        SpinLockRelease(&lock);
        if (copied) PipePollNotify(this, 4);
        return copied;
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
    if (pipe->packets) kfree(pipe->packets);
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
