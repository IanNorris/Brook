#pragma once
#include "address.h"
#include "spinlock.h"

namespace brook {
struct Vnode;
struct FilePageCacheEntry {
    uint64_t cacheId = 0, pageIndex = 0;
    PhysicalAddress physical;
    Vnode* vnode = nullptr;
};
// Clean canonical pages only. Ordinary I/O coherence, dirty writeback and
// eviction are separate obligations before writable shared files can use this.
class FilePageCache {
public:
    constexpr FilePageCache(FilePageCacheEntry* entries, uint32_t slots, uint32_t probes = 32)
        : entries_(entries), slots_(slots), probes_(probes < slots ? probes : slots) {}
    // Each successful result owns one frame reference.
    PhysicalAddress Lookup(Vnode* vnode, uint64_t pageIndex);
    // Always consumes candidate's reference. A duplicate returns the retained
    // canonical winner, never the losing candidate. With allowUncached=false,
    // disabled identity or capacity exhaustion drops candidate and returns zero.
    // Private mappings may explicitly allow an uncached fallback; shared callers
    // must require canonical storage, in addition to implementing I/O coherence.
    PhysicalAddress Adopt(Vnode* vnode, uint64_t pageIndex, PhysicalAddress candidate,
                          bool allowUncached);
private:
    uint32_t Hash(uint64_t identity, uint64_t pageIndex) const;
    FilePageCacheEntry* entries_;
    uint32_t slots_, probes_;
    IrqSpinLock lock_;
};
PhysicalAddress FilePageCacheLookup(Vnode* vnode, uint64_t pageIndex);
PhysicalAddress FilePageCacheAdopt(Vnode* vnode, uint64_t pageIndex,
                                   PhysicalAddress candidate, bool allowUncached);
}
