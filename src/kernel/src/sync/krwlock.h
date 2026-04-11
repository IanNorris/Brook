#pragma once

// KRwLock — kernel sleeping reader-writer lock
//
// Multiple readers can hold the lock concurrently, but writers get
// exclusive access. Built on the scheduler's Block/Unblock mechanism.
//
// Design:
//   - `readerCount` tracks active readers (>0 = readers holding).
//   - `writerActive` is 1 when a writer holds the lock.
//   - Separate wait queues for readers and writers.
//   - Readers are admitted when no writer is active (readers-preferred
//     when no writer holds). Writers wait for all readers to finish.
//   - Internal guard: interrupt-disabling ticket spinlock, held only
//     for queue manipulation.
//
// Fairness: When a writer unlocks, all queued readers are woken at once
// (batch wakeup). If no readers are queued, the next writer is woken.

#include <stdint.h>

namespace brook {

struct Process;

struct KRwLock {
    volatile int32_t  readerCount;   // Active reader count
    volatile uint32_t writerActive;  // 1 = writer holding
    volatile uint32_t writersWaiting; // Count of waiting writers (for writer preference)

    Process*          readWaitHead;  // Blocked readers
    Process*          readWaitTail;
    Process*          writeWaitHead; // Blocked writers
    Process*          writeWaitTail;

    // Internal guard spinlock
    volatile uint32_t guardNext;
    volatile uint32_t guardServing;
};

void KRwLockInit(KRwLock* rw);

// Acquire for reading. Blocks if a writer is active or writers are waiting.
void KRwLockReadLock(KRwLock* rw);

// Release a read lock. If this was the last reader and writers are waiting,
// wakes the next writer.
void KRwLockReadUnlock(KRwLock* rw);

// Acquire for writing. Blocks if any readers or another writer are active.
void KRwLockWriteLock(KRwLock* rw);

// Release the write lock. Wakes all queued readers, or the next writer.
void KRwLockWriteUnlock(KRwLock* rw);

} // namespace brook
