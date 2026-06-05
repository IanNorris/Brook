#pragma once

#include <stdint.h>

namespace brook {

// Per-CPU lock tracking for deadlock diagnostics.
// Updated by IrqSpinLockAcquire; read by TLB shootdown timeout dump.
struct LockDiagInfo {
    const char* file;       // __FILE__ of most recent IrqSpinLockAcquire
    uint32_t    line;       // __LINE__ of most recent IrqSpinLockAcquire
    uint32_t    held;       // 1 = currently holding an IrqSpinLock (IF=0)
};

#if defined(__BROOK_KERNEL__)
extern LockDiagInfo g_lockDiag[];
#else
inline LockDiagInfo g_lockDiag[64] = {};
#endif

// ---------------------------------------------------------------------------
// SpinLock — does NOT disable interrupts.
//
// Use for data that is NEVER accessed from an ISR (interrupt service routine).
// This is the correct choice for ~90% of kernel locks: ext2 caches, page cache,
// device registry, heap, VMM, networking, audio mixer, klog, etc.
//
// Because IF stays set, maskable IPIs (including TLB shootdown) can be
// delivered while this lock is held, preventing deadlock.
// ---------------------------------------------------------------------------

struct SpinLock {
    volatile uint32_t next    = 0;
    volatile uint32_t serving = 0;
};

static inline void SpinLockAcquire(SpinLock* lock)
{
    uint32_t ticket = __atomic_fetch_add(&lock->next, 1, __ATOMIC_RELAXED);
    while (__atomic_load_n(&lock->serving, __ATOMIC_ACQUIRE) != ticket)
        __asm__ volatile("pause" ::: "memory");
}

static inline void SpinLockRelease(SpinLock* lock)
{
    __atomic_fetch_add(&lock->serving, 1, __ATOMIC_RELEASE);
}

// Non-blocking acquire: succeeds only when the lock is completely free
// (no holder and no waiters). Returns true if acquired (caller must
// SpinLockRelease), false if the lock is currently busy.
//
// Required for code that runs in ISR context but shares a plain SpinLock
// with normal kernel code (e.g. the periodic DeviceCheckIntegrity run from
// the timer tick). Blocking there would self-deadlock if the timer fires on
// a CPU that already holds the lock in interrupted normal-context code.
static inline bool SpinLockTryAcquire(SpinLock* lock)
{
    uint32_t serving = __atomic_load_n(&lock->serving, __ATOMIC_ACQUIRE);
    uint32_t next    = __atomic_load_n(&lock->next, __ATOMIC_RELAXED);
    if (next != serving)
        return false; // held or has waiters
    // Claim ticket `next` only if `next` is still unchanged. On success we
    // hold the lock (serving == our ticket already).
    return __atomic_compare_exchange_n(&lock->next, &next, next + 1,
                                       false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

// ---------------------------------------------------------------------------
// IrqSpinLock — disables interrupts (cli) while held.
//
// Use ONLY for data shared between normal kernel code and ISR/interrupt
// handlers (e.g. SchedLock used by timer ISR, TLB shootdown request lock).
// Keeping IF=0 prevents the ISR from firing on this CPU while the lock
// is held, avoiding re-entrant deadlock.
//
// WARNING: Holding an IrqSpinLock blocks delivery of TLB shootdown IPIs
// on this CPU. Keep critical sections short.
// ---------------------------------------------------------------------------

struct IrqSpinLock {
    volatile uint32_t next    = 0;
    volatile uint32_t serving = 0;
};

static inline uint64_t IrqSpinLockAcquireImpl(IrqSpinLock* lock,
                                               const char* file, uint32_t line)
{
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");

    uint32_t ticket = __atomic_fetch_add(&lock->next, 1, __ATOMIC_RELAXED);
    while (__atomic_load_n(&lock->serving, __ATOMIC_ACQUIRE) != ticket)
        __asm__ volatile("pause" ::: "memory");

#ifdef __BROOK_KERNEL__
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

static inline void IrqSpinLockRelease(IrqSpinLock* lock, uint64_t savedFlags)
{
#ifdef __BROOK_KERNEL__
    uint32_t cpu;
    __asm__ volatile("movl %%gs:176, %0" : "=r"(cpu));
    if (cpu < 64) {
        __atomic_store_n(&g_lockDiag[cpu].held, 0u, __ATOMIC_RELEASE);
    }
#endif

    __atomic_fetch_add(&lock->serving, 1, __ATOMIC_RELEASE);

    if (savedFlags & 0x200)
        __asm__ volatile("sti" ::: "memory");
}

#define IrqSpinLockAcquire(lock) IrqSpinLockAcquireImpl((lock), __FILE__, __LINE__)

} // namespace brook
