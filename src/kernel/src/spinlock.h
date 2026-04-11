#pragma once

#include <stdint.h>

namespace brook {

// Simple ticket spinlock for kernel synchronization.
// Disables interrupts while held to prevent deadlock from timer preemption.
struct SpinLock {
    volatile uint32_t next   = 0; // Next ticket number
    volatile uint32_t serving = 0; // Currently serving ticket
};

// Acquire the spinlock. Disables interrupts and spins until the lock is acquired.
// Returns the previous RFLAGS value (for restoring interrupt state on release).
static inline uint64_t SpinLockAcquire(SpinLock* lock)
{
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");

    // Atomically fetch-and-increment the ticket counter.
    uint32_t ticket = __atomic_fetch_add(&lock->next, 1, __ATOMIC_RELAXED);

    // Spin until our ticket is being served.
    while (__atomic_load_n(&lock->serving, __ATOMIC_ACQUIRE) != ticket)
        __asm__ volatile("pause" ::: "memory");

    return flags;
}

// Release the spinlock and restore previous interrupt state.
static inline void SpinLockRelease(SpinLock* lock, uint64_t savedFlags)
{
    __atomic_fetch_add(&lock->serving, 1, __ATOMIC_RELEASE);

    // Restore interrupts if they were enabled before.
    if (savedFlags & 0x200)
        __asm__ volatile("sti" ::: "memory");
}

} // namespace brook
