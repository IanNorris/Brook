#pragma once
#include "address.h"
#include "spinlock.h"

namespace brook {
struct ExecPageCacheEntry { uint64_t hash = 0; PhysicalAddress physical; };
struct ExecPageCacheStats {
    uint64_t hits = 0, misses = 0, admissions = 0, full = 0, collisions = 0;
    uint64_t trimmed = 0, entries = 0, capacity = 0;
};
using ExecPageHasher = uint64_t (*)(PhysicalAddress);

// Immutable full initialized ELF pages only. No vnode/file-generation aliases:
// hashes are followed by a full byte comparison. Every cache entry owns a ref;
// every user mapping must independently own a ref and remain COW while shared.
class ExecPageCache {
public:
    constexpr ExecPageCache(ExecPageCacheEntry* entries, uint32_t slots,
                             uint32_t capacity, uint32_t probes = 32,
                             ExecPageHasher hasher = nullptr)
        : entries_(entries), slots_(slots), capacity_(capacity<slots?capacity:slots),
          probes_(probes<slots?probes:slots), hasher_(hasher) {}
    // Always consumes candidate's owned ref; returns one owned mapping ref.
    // Candidate must remain immutable throughout this operation. Capacity or
    // probe exhaustion returns it uncached, preserving normal loader behavior.
    PhysicalAddress Adopt(PhysicalAddress candidate);
    uint64_t ReclaimIdleStep(uint32_t maximumScan); // bounded pressure-worker batch
    uint64_t TrimIdle(); // no allocation/I/O/waits; bounded lock holds
    uint64_t Clear();    // dropping cache refs is safe even with live COW users
    ExecPageCacheStats Stats();
private:
    uint64_t Trim(bool all);
    ExecPageCacheEntry* entries_;
    uint32_t slots_, capacity_, probes_;
    ExecPageHasher hasher_;
    IrqSpinLock lock_;
    ExecPageCacheStats stats_;
    uint32_t reclaimCursor_=0;
};
PhysicalAddress ExecPageCacheAdopt(PhysicalAddress candidate);
uint64_t ExecPageCacheTrimIdle();
uint64_t ExecPageCacheReclaimIdleStep(uint32_t maximumScan);
ExecPageCacheStats ExecPageCacheGetStats();
#ifdef BROOK_HOST_TEST
void ExecPageCacheClearForTest();
#endif
}
