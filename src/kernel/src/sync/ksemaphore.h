#pragma once

// KSemaphore — kernel sleeping counting semaphore
//
// A counting semaphore that blocks the calling process when the count
// reaches zero. Built on the scheduler's Block/Unblock mechanism.
//
// Design:
//   - `count` is the current value (>0 = available permits).
//   - `waitHead/waitTail` is a FIFO queue of blocked processes.
//   - Internal guard: interrupt-disabling ticket spinlock.
//
// Use cases:
//   - Resource limiting (e.g., max N concurrent disk operations)
//   - Producer-consumer synchronisation
//   - Binary semaphore (init with count=1)

#include <stdint.h>

namespace brook {

struct Process;

struct KSemaphore {
    volatile int32_t count;      // Current count (>0 = available)
    Process*         waitHead;   // First waiter (FIFO)
    Process*         waitTail;   // Last waiter

    // Internal guard spinlock
    volatile uint32_t guardNext;
    volatile uint32_t guardServing;
};

// Initialise with the given initial count.
void KSemaphoreInit(KSemaphore* sem, int32_t initialCount);

// Decrement (wait/P). Blocks if count would go below zero.
void KSemaphoreWait(KSemaphore* sem);

// Increment (signal/V). Wakes one blocked waiter if any.
void KSemaphoreSignal(KSemaphore* sem);

// Try to decrement without blocking.
// Returns true if acquired, false if count was zero.
bool KSemaphoreTryWait(KSemaphore* sem);

} // namespace brook
