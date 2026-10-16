#include "file_page_cache.h"
#include "physical_memory.h"
#include "vfs.h"

namespace brook {
uint32_t FilePageCache::Hash(uint64_t identity, uint64_t pageIndex) const {
    return static_cast<uint32_t>((identity ^ (pageIndex * 2654435761ULL)) % slots_);
}
PhysicalAddress FilePageCache::Lookup(Vnode* vnode, uint64_t pageIndex) {
    const uint64_t identity = vnode->cacheId;
    if (!identity || !slots_) return PhysicalAddress{};
    uint64_t irq = IrqSpinLockAcquire(&lock_);
    const uint32_t slot = Hash(identity, pageIndex);
    for (uint32_t i = 0; i < probes_; ++i) {
        auto& entry = entries_[(slot + i) % slots_];
        if (!entry.cacheId) break;
        if (entry.cacheId == identity && entry.pageIndex == pageIndex) {
            PmmRefPage(entry.physical);
            const auto physical = entry.physical;
            IrqSpinLockRelease(&lock_, irq);
            return physical;
        }
    }
    IrqSpinLockRelease(&lock_, irq);
    return PhysicalAddress{};
}
PhysicalAddress FilePageCache::Adopt(Vnode* vnode, uint64_t pageIndex,
                                     PhysicalAddress candidate, bool allowUncached) {
    if (!candidate) return PhysicalAddress{};
    const uint64_t identity = vnode->cacheId;
    if (identity && slots_) {
        uint64_t irq = IrqSpinLockAcquire(&lock_);
        const uint32_t slot = Hash(identity, pageIndex);
        for (uint32_t i = 0; i < probes_; ++i) {
            auto& entry = entries_[(slot + i) % slots_];
            if (!entry.cacheId) {
                // Candidate's original reference goes back to the caller;
                // the cache owns a separate reference and pins the vnode.
                PmmRefPage(candidate);
                VfsRetain(vnode);
                entry = {identity, pageIndex, candidate, vnode};
                IrqSpinLockRelease(&lock_, irq);
                return candidate;
            }
            if (entry.cacheId == identity && entry.pageIndex == pageIndex) {
                const auto winner = entry.physical;
                PmmRefPage(winner);
                IrqSpinLockRelease(&lock_, irq);
                // Final loser release may run outside the short cache lock.
                PmmUnrefPage(candidate);
                return winner;
            }
        }
        IrqSpinLockRelease(&lock_, irq);
    }
    if (allowUncached) return candidate;
    PmmUnrefPage(candidate);
    return PhysicalAddress{};
}
namespace {
FilePageCacheEntry entries[65536];
FilePageCache cache(entries, 65536);
}
PhysicalAddress FilePageCacheLookup(Vnode* vnode, uint64_t pageIndex) {
    return cache.Lookup(vnode, pageIndex);
}
PhysicalAddress FilePageCacheAdopt(Vnode* vnode, uint64_t pageIndex,
                                   PhysicalAddress candidate, bool allowUncached) {
    return cache.Adopt(vnode, pageIndex, candidate, allowUncached);
}
}
