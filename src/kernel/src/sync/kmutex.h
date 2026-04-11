#pragma once

// KMutex — kernel sleeping mutex
//
// A mutual-exclusion lock that blocks (sleeps) the calling process when
// contended, rather than spin-waiting. Built on the scheduler's
// Block/Unblock mechanism and protected by a brief internal spinlock.
//
// Design:
//   - `owner` is the PID of the holding process (0 = unlocked).
//   - `waitHead/waitTail` is a singly-linked list of blocked processes.
//   - `guard` is a spinlock protecting the mutex's internal state; it is
//     held only for the few instructions needed to inspect/update the
//     wait queue, never across a sleep.
//
// Fairness: FIFO — waiters are woken in the order they blocked.

#include <stdint.h>

namespace brook {

struct Process;

// Forward-declared wait-queue node embedded in Process (added below).
// Each blocked waiter links to the next via Process::mutexNext.

struct KMutex {
    volatile uint32_t locked;     // 1 = held, 0 = free
    volatile uint32_t ownerPid;   // PID of holder (0 = none)
    Process*          waitHead;   // First waiter (FIFO queue)
    Process*          waitTail;   // Last waiter

    // Internal spinlock (interrupt-disabling ticket lock).
    // Only held for a handful of instructions — never across a sleep.
    volatile uint32_t guardNext;
    volatile uint32_t guardServing;
};

// Initialise a mutex to unlocked state.
void KMutexInit(KMutex* m);

// Acquire the mutex. If already held, the calling process is blocked
// (put to sleep) until the holder releases it. Must be called from
// process context (not interrupt context).
void KMutexLock(KMutex* m);

// Release the mutex. If waiters are queued, the first one is unblocked.
// Must be called by the holder.
void KMutexUnlock(KMutex* m);

// Try to acquire the mutex without blocking.
// Returns true if acquired, false if already held.
bool KMutexTryLock(KMutex* m);

} // namespace brook
