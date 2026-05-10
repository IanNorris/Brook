#pragma once

#include <stdint.h>

namespace brook {

// Per-CPU lock tracking for deadlock diagnostics.
// Updated by SpinLockAcquire; read by TLB shootdown timeout dump.
struct LockDiagInfo {
    const char* file;       // __FILE__ of most recent SpinLockAcquire
    uint32_t    line;       // __LINE__ of most recent SpinLockAcquire
    uint32_t    held;       // 1 = currently holding a lock with IF=0
};

// Per-CPU array (indexed by SmpCurrentCpuIndex).
// Weak definition — overridden by full kernel link which defines it in apic.cpp.
// Host tests get a stub that satisfies the linker.
#if defined(__BROOK_KERNEL__)
extern LockDiagInfo g_lockDiag[];
#else
// Weak inline storage for host test builds
inline LockDiagInfo g_lockDiag[64] = {};
#endif

// Simple ticket spinlock for kernel synchronization.
// Disables interrupts while held to prevent deadlock from timer preemption.
struct SpinLock {
    volatile uint32_t next   = 0; // Next ticket number
    volatile uint32_t serving = 0; // Currently serving ticket
};

// Acquire the spinlock. Disables interrupts and spins until the lock is acquired.
// Returns the previous RFLAGS value (for restoring interrupt state on release).
// Use via SpinLockAcquire() macro which captures file/line for diagnostics.
static inline uint64_t SpinLockAcquireImpl(SpinLock* lock,
                                            const char* file, uint32_t line)
{
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");

    // Atomically fetch-and-increment the ticket counter.
    uint32_t ticket = __atomic_fetch_add(&lock->next, 1, __ATOMIC_RELAXED);

    // Spin until our ticket is being served.
    while (__atomic_load_n(&lock->serving, __ATOMIC_ACQUIRE) != ticket)
        __asm__ volatile("pause" ::: "memory");

#ifdef __BROOK_KERNEL__
    // Record for deadlock diagnostics (best-effort, no lock needed).
    uint32_t cpu;
    __asm__ volatile("movl %%gs:176, %0" : "=r"(cpu));
    if (cpu < 64) {
        g_lockDiag[cpu].file = file;
        g_lockDiag[cpu].line = line;
        __atomic_store_n(&g_lockDiag[cpu].held, 1u, __ATOMIC_RELEASE);
    }
#else
    (void)file; (void)line;
#endif

    return flags;
}

// Release the spinlock and restore previous interrupt state.
static inline void SpinLockRelease(SpinLock* lock, uint64_t savedFlags)
{
#ifdef __BROOK_KERNEL__
    // Clear lock-held diagnostic before re-enabling interrupts.
    uint32_t cpu;
    __asm__ volatile("movl %%gs:176, %0" : "=r"(cpu));
    if (cpu < 64) {
        __atomic_store_n(&g_lockDiag[cpu].held, 0u, __ATOMIC_RELEASE);
    }
#endif

    __atomic_fetch_add(&lock->serving, 1, __ATOMIC_RELEASE);

    // Restore interrupts if they were enabled before.
    if (savedFlags & 0x200)
        __asm__ volatile("sti" ::: "memory");
}

// Macro that captures __FILE__/__LINE__ for diagnostics.
#define SpinLockAcquire(lock) SpinLockAcquireImpl((lock), __FILE__, __LINE__)

} // namespace brook
