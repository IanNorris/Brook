#pragma once

#include <stdint.h>

namespace brook {

struct Process;  // forward declaration — the pool only stores the pointer

// ---------------------------------------------------------------------------
// Futex waiter node.
//
// One node per blocked thread; nodes are chained into the futex hash buckets
// (the chain pointer `next` is managed by the futex hash table under its own
// spinlock, NOT by this pool).
// ---------------------------------------------------------------------------
struct FutexWaiter {
    uint64_t     uaddr;   // User virtual address being waited on
    uint64_t     owner;   // 0 for shared futexes; tgid for FUTEX_PRIVATE_FLAG
    Process*     proc;    // Blocked process
    uint32_t     bitset;  // Bitset accepted (FUTEX_BITSET_MATCH_ANY for plain WAIT)
    FutexWaiter* next;    // Next in hash bucket chain
};

// ---------------------------------------------------------------------------
// Fixed-capacity pool of futex waiter nodes (BRO-160).
//
// Slot ownership is tracked with a per-slot atomic flag claimed via
// __atomic_exchange, so Alloc()/Free() are correct independently of any
// external lock. The previous implementation used plain reads/writes that were
// only safe while the caller held g_futexLock; but FutexWake drops and
// reacquires g_futexLock around WakeProcess, so a concurrent Alloc on another
// CPU during that window observed the pool without consistent ordering. The
// atomic test-and-set claim removes that fragile invariant entirely.
//
// Capacity should be sized to the maximum number of threads: a thread blocked
// in a futex wait cannot start another wait, so at most one waiter exists per
// thread at a time. Sizing N >= MAX_PROCESSES therefore makes the pool
// impossible to exhaust legitimately (the old 128-entry cap returned -ENOMEM
// under heavily-threaded workloads such as the Go runtime).
//
// Instances are expected to live in static storage (BSS), which zero-initialises
// `used_`; a value-initialised instance ({}) is equally valid.
// ---------------------------------------------------------------------------
template <uint32_t N>
class FutexWaiterPool {
public:
    // Claim a free slot. Returns nullptr if the pool is full.
    FutexWaiter* Alloc()
    {
        for (uint32_t i = 0; i < N; ++i) {
            // Atomically claim slot i: exchange old->1, succeed iff it was 0.
            if (__atomic_exchange_n(&used_[i], uint8_t(1), __ATOMIC_ACQUIRE) == 0) {
                return &pool_[i];
            }
        }
        return nullptr;
    }

    // Release a previously-claimed slot. Safe to call with nullptr or a pointer
    // outside the pool (ignored).
    void Free(FutexWaiter* w)
    {
        if (!w) return;
        const uint32_t idx = static_cast<uint32_t>(w - pool_);
        if (idx < N) {
            __atomic_store_n(&used_[idx], uint8_t(0), __ATOMIC_RELEASE);
        }
    }

    static constexpr uint32_t Capacity() { return N; }

    // Index of a node within this pool, or -1 if the pointer is not ours.
    // Useful for callers that key external state by slot (and for tests that
    // need to detect a slot being handed out twice).
    int32_t IndexOf(const FutexWaiter* w) const
    {
        if (!w) return -1;
        const uint32_t idx = static_cast<uint32_t>(w - pool_);
        return idx < N ? static_cast<int32_t>(idx) : -1;
    }

private:
    FutexWaiter pool_[N];
    uint8_t     used_[N];   // 0 = free, 1 = claimed; accessed atomically only
};

} // namespace brook
