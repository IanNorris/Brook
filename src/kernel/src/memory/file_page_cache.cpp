#include "file_page_cache.h"
#include "physical_memory.h"
#include "vfs.h"

namespace brook {
uint32_t FilePageCacheAllocateNamespace() {
    static uint64_t next=1;
    uint64_t value=__atomic_load_n(&next,__ATOMIC_RELAXED);
    do {if(value>0xffffffffULL)return 0;}
    while(!__atomic_compare_exchange_n(&next,&value,value+1,false,__ATOMIC_RELAXED,__ATOMIC_RELAXED));
    return value;
}

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
        if (!entry.cacheId) { if(entry.pageIndex==~uint64_t{0})continue;break; }
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
        FilePageCacheEntry* available=nullptr;
        for (uint32_t i = 0; i < probes_; ++i) {
            auto& entry = entries_[(slot + i) % slots_];
            if (!entry.cacheId) {
                if(!available)available=&entry;
                if(entry.pageIndex==~uint64_t{0})continue; // tombstone, not probe end
                break;
            }
            if (entry.cacheId == identity && entry.pageIndex == pageIndex) {
                const auto winner = entry.physical;
                PmmRefPage(winner);
                IrqSpinLockRelease(&lock_, irq);
                PmmUnrefPage(candidate);
                return winner;
            }
        }
        if(available) {
            PmmRefPage(candidate); // immutable bytes need no vnode lifetime pin
            *available={identity,pageIndex,candidate};++count_;
            IrqSpinLockRelease(&lock_,irq);return candidate;
        }
        IrqSpinLockRelease(&lock_, irq);
    }
    if (allowUncached) return candidate;
    PmmUnrefPage(candidate);
    return PhysicalAddress{};
}
uint32_t FilePageCache::Count() {
    auto irq=IrqSpinLockAcquire(&lock_);auto count=count_;IrqSpinLockRelease(&lock_,irq);return count;
}
FilePageCacheStats FilePageCache::Statistics() {
    auto irq=IrqSpinLockAcquire(&lock_);FilePageCacheStats s{count_,scanned_,reclaimed_};IrqSpinLockRelease(&lock_,irq);return s;
}
FilePageCache::ReclaimResult FilePageCache::ReclaimStep(uint32_t maximumScan) {
    ReclaimResult result{};uint32_t visits=maximumScan<slots_?maximumScan:slots_;
    for(uint32_t n=0;n<visits;++n) {
        PhysicalAddress retired{};
        auto irq=IrqSpinLockAcquire(&lock_);
        ++scanned_;
        auto& entry=entries_[reclaimCursor_++];if(reclaimCursor_==slots_)reclaimCursor_=0;
        // Every public lookup/adoption takes its ref under this lock. A sole
        // cache ref excludes in-flight callers and PTE retirement awaiting ACK;
        // this is ownership of this cache's frames, not general eviction safety.
        if(entry.cacheId && PmmGetRefCount(entry.physical)==1 && !PmmGetMapCount(entry.physical)) {
            retired=entry.physical;entry={0,~uint64_t{0},{}};--count_;++result.reclaimed;++reclaimed_;
        }
        IrqSpinLockRelease(&lock_,irq);++result.scanned;
        if(retired)PmmUnrefPage(retired);
    }
    return result;
}
namespace {
FilePageCacheEntry entries[65536];
FilePageCache cache(entries, 65536);
}
FilePageCache::ReclaimResult FilePageCacheReclaim(uint32_t maximumScan){return cache.ReclaimStep(maximumScan);}
uint32_t FilePageCacheCount(){return cache.Count();}
FilePageCacheStats FilePageCacheStatistics(){return cache.Statistics();}
PhysicalAddress FilePageCacheLookup(Vnode* vnode, uint64_t pageIndex) {
    return cache.Lookup(vnode, pageIndex);
}
PhysicalAddress FilePageCacheAdopt(Vnode* vnode, uint64_t pageIndex,
                                   PhysicalAddress candidate, bool allowUncached) {
    return cache.Adopt(vnode, pageIndex, candidate, allowUncached);
}
}
