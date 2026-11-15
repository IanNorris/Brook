// syscall_common.cpp -- shared syscall helpers: errno values, signal-pending checks, wakeups, fd pin guard, random.
//
// Split from syscall.cpp (see docs/agent/syscall-split-plan.md).
// Adapted from Enkel OS (IanNorris/Enkel, MIT license).

#include "posix_timer.h"
#include "cpu.h"
#include "syscall_internal.h"

// ---------------------------------------------------------------------------
// Random helpers: RDRAND with TSC fallback
// ---------------------------------------------------------------------------

// Software PRNG fallback using TSC + LCG mixing (used when rdrand unavailable).
uint64_t SoftRandU64()
{
    uint64_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    uint64_t tsc = (hi << 32) | lo;
    tsc ^= tsc << 13;
    tsc ^= tsc >> 7;
    tsc ^= tsc << 17;
    return tsc * 6364136223846793005ULL + 1442695040888963407ULL;
}

bool RdrandU64(uint64_t* out)
{
    if (!CpuHasRdrand()) return false;
    uint64_t val;
    uint8_t ok;
    for (int i = 0; i < 10; i++)
    {
        __asm__ volatile("rdrand %0; setc %1" : "=r"(val), "=qm"(ok));
        if (ok) { *out = val; return true; }
    }
    return false;
}

namespace brook {

// ---------------------------------------------------------------------------
// Profiling counters (atomic, dumped on title changes)
// ---------------------------------------------------------------------------
volatile uint64_t g_profOpenCount = 0;
volatile uint64_t g_profMmapCount = 0;
volatile uint64_t g_profCacheHit = 0;
volatile uint64_t g_profCacheMiss = 0;
volatile uint64_t g_profPreloadPages = 0;

// ---------------------------------------------------------------------------
// Error codes (Linux)
// ---------------------------------------------------------------------------

// Check if the current process has deliverable signals pending.
// Call after SchedulerBlock() returns to decide whether to return -EINTR.
bool HasPendingSignals()
{
    Process* proc = ProcessCurrent();
    if (!proc) return false;
    return __atomic_load_n(&proc->terminalPending, __ATOMIC_ACQUIRE) ||
           ((ProcessPendingSignals(proc) | PosixTimerPending(proc)) & ~proc->sigMask) != 0;
}

// Child exit also queues default-ignored SIGCHLD to wake wait4. Blocking
// reads may discard that notification only when no user handler needs it.
// In particular, libuv's exec-handshake read must preserve caught SIGCHLD
// so its handler can notify the event loop and reap the exited child.
bool HasReadInterruptSignal()
{
    Process* proc = ProcessCurrent();
    if (!proc) return false;
    constexpr uint64_t childBit = 1ULL << (17 - 1);
    if ((ProcessPendingSignals(proc) & ~proc->sigMask & childBit) && proc->tgid < MAX_PROCESSES) {
        uint64_t handler = ReadSignalAction(proc->tgid,17 - 1).handler;
        if (handler == 0 || handler == 1)
            ProcessDequeueSignal(proc, 17, nullptr);
    }
    return HasPendingSignals();
}

void WakeProcess(Process* proc)
{
    uint64_t addr = reinterpret_cast<uint64_t>(proc);
    if (!proc || (addr >> 47) != 0x1FFFFULL) return;
    if (proc->magic != PROCESS_MAGIC) return;
    __atomic_store_n(&proc->pendingWakeup, 1, __ATOMIC_RELEASE);
    SchedulerUnblock(proc);
}

// ---------------------------------------------------------------------------
// sys_open (2)
// ---------------------------------------------------------------------------

// Simple string comparison helper
bool StrEq(const char* a, const char* b)
{
    while (*a && *b) { if (*a++ != *b++) return false; }
    return *a == *b;
}

} // namespace brook
