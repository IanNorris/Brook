#pragma once
// futex_waitv (x86-64 449, Linux 5.16) argument policy. Pure and host-tested:
// the kernel queueing protocol lives in syscall_futex.cpp.
#include <stdint.h>

namespace brook {

// Linux uapi struct futex_waitv.
struct FutexWaitvEntry {
    uint64_t val;
    uint64_t uaddr;
    uint32_t flags;
    uint32_t reserved;
};
static_assert(sizeof(FutexWaitvEntry) == 24, "struct futex_waitv is 24 bytes");

static constexpr uint32_t FUTEX_WAITV_MAX = 128;
static constexpr uint32_t FUTEX2_SIZE_U8 = 0x00;
static constexpr uint32_t FUTEX2_SIZE_U16 = 0x01;
static constexpr uint32_t FUTEX2_SIZE_U32 = 0x02;
static constexpr uint32_t FUTEX2_SIZE_U64 = 0x03;
static constexpr uint32_t FUTEX2_SIZE_MASK = 0x03;
static constexpr uint32_t FUTEX2_NUMA = 0x04;
static constexpr uint32_t FUTEX2_MPOL = 0x08;
static constexpr uint32_t FUTEX2_PRIVATE = 128;

// Syscall-level checks, in Linux order: flags, count/pointer, then clock.
// The clock is only examined when a timeout is supplied.
inline int FutexWaitvCheckCall(uint64_t waiters, uint64_t count, uint64_t flags,
                               uint64_t timeout, uint64_t clockid)
{
    constexpr int Einval = -22;
    if (flags) return Einval;
    if (!count || count > FUTEX_WAITV_MAX || !waiters) return Einval;
    if (timeout && clockid != 0 /*REALTIME*/ && clockid != 1 /*MONOTONIC*/) return Einval;
    return 0;
}

// One copied entry. Only 32-bit futexes exist on Linux, so every other size
// is EINVAL. NUMA/MPOL node hints (6.16+) are not implemented here and are
// refused rather than silently ignored.
inline int FutexWaitvCheckEntry(const FutexWaitvEntry& entry)
{
    constexpr int Einval = -22;
    if (entry.flags & ~(FUTEX2_SIZE_MASK | FUTEX2_PRIVATE)) return Einval;
    if (entry.reserved) return Einval;
    if ((entry.flags & FUTEX2_SIZE_MASK) != FUTEX2_SIZE_U32) return Einval;
    if (entry.val > 0xffffffffull) return Einval;
    if (entry.uaddr & 3) return Einval;
    return 0;
}

// Linux unqueue_multiple() reports the highest index whose queue entry a
// waker already removed; -1 when none was woken.
template <typename Queued>
inline int32_t FutexWaitvWokenIndex(uint32_t count, Queued queued)
{
    int32_t result = -1;
    for (uint32_t i = 0; i < count; ++i)
        if (!queued(i)) result = static_cast<int32_t>(i);
    return result;
}

} // namespace brook
